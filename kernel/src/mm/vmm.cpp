#include "hk/mm/vmm.hpp"
#include "hk/mm/pmm.hpp"
#include "hk/lib/string.hpp"
#include "hk/smp/smp.hpp"
#include "hk/timer/pit.hpp"

namespace hk::mm {

namespace {
constexpr uint64_t kAddrMask = 0x000ffffffffff000ull;
constexpr uint64_t kDefaultTableFlags = PagePresent | PageWrite;
uint16_t pml4_index(uint64_t v) { return static_cast<uint16_t>((v >> 39) & 0x1ff); }
uint16_t pdpt_index(uint64_t v) { return static_cast<uint16_t>((v >> 30) & 0x1ff); }
uint16_t pd_index(uint64_t v) { return static_cast<uint16_t>((v >> 21) & 0x1ff); }
uint16_t pt_index(uint64_t v) { return static_cast<uint16_t>((v >> 12) & 0x1ff); }
void invlpg(uint64_t v) { asm volatile("invlpg (%0)" : : "r"(v) : "memory"); }
}

VirtualMemoryManager& vmm() {
    static VirtualMemoryManager manager;
    return manager;
}

void VirtualMemoryManager::initialize_identity() {
    uint64_t firmware_root = active_pml4() & kAddrMask;
    uint64_t owned_root = pmm().allocate_page();
    if (owned_root == 0) {
        root_ = firmware_root;
        return;
    }
    memcpy(reinterpret_cast<void*>(owned_root), reinterpret_cast<void*>(firmware_root), kPageSize);
    load_cr3(owned_root);
}

uint64_t VirtualMemoryManager::active_pml4() const {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

void VirtualMemoryManager::load_cr3(uint64_t physical) {
    root_ = physical & kAddrMask;
    asm volatile("mov %0, %%cr3" : : "r"(root_) : "memory");
}

void VirtualMemoryManager::invalidate_local_page(uint64_t virt) {
    invlpg(virt);
}

uint64_t* VirtualMemoryManager::table_from_phys(uint64_t physical) const {
    return reinterpret_cast<uint64_t*>(physical & kAddrMask);
}

uint64_t* VirtualMemoryManager::ensure_table(uint64_t* table, uint16_t index) {
    if ((table[index] & PagePresent) == 0) {
        uint64_t page = pmm().allocate_page();
        if (page == 0) return nullptr;
        memset(reinterpret_cast<void*>(page), 0, kPageSize);
        table[index] = page | kDefaultTableFlags;
    }
    return table_from_phys(table[index]);
}

MappingResult VirtualMemoryManager::map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    ++diagnostics_.map_page_calls;
    if ((virt & (kPageSize - 1)) || (phys & (kPageSize - 1))) {
        ++diagnostics_.failed_maps;
        ++diagnostics_.unaligned_map_rejects;
        return {false, "unaligned map_page address"};
    }
    uint64_t* pml4 = table_from_phys(root_);
    uint64_t* pdpt = ensure_table(pml4, pml4_index(virt));
    if (!pdpt) {
        ++diagnostics_.failed_maps;
        return {false, "failed to allocate PDPT"};
    }
    uint64_t* pd = ensure_table(pdpt, pdpt_index(virt));
    if (!pd) {
        ++diagnostics_.failed_maps;
        return {false, "failed to allocate PD"};
    }
    uint64_t* pt = ensure_table(pd, pd_index(virt));
    if (!pt) {
        ++diagnostics_.failed_maps;
        return {false, "failed to allocate PT"};
    }
    if (pt[pt_index(virt)] & PagePresent) {
        ++diagnostics_.failed_maps;
        ++diagnostics_.duplicate_map_rejects;
        return {false, "virtual page is already mapped"};
    }
    pt[pt_index(virt)] = (phys & kAddrMask) | (flags & ~kAddrMask) | PagePresent;
    invalidate_local_page(virt);
    if (!hk::timer::lapic_timer_active()) {
        hk::smp::shootdown_remote_tlbs(virt);
        ++diagnostics_.remote_shootdowns_requested;
    }
    ++diagnostics_.mapped_pages;
    diagnostics_.last_mapped_virt = virt;
    return {true, "ok"};
}

MappingResult VirtualMemoryManager::map_range(uint64_t virt, uint64_t phys, uint64_t length, uint64_t flags) {
    ++diagnostics_.map_range_calls;
    uint64_t pages = align_up(length) / kPageSize;
    for (uint64_t i = 0; i < pages; ++i) {
        auto result = map_page(virt + i * kPageSize, phys + i * kPageSize, flags);
        if (!result.ok) return result;
    }
    return {true, "ok"};
}

MappingResult VirtualMemoryManager::unmap_page(uint64_t virt) {
    ++diagnostics_.unmap_page_calls;
    uint64_t* pml4 = table_from_phys(root_);
    uint64_t pml4e = pml4[pml4_index(virt)];
    if ((pml4e & PagePresent) == 0) {
        ++diagnostics_.failed_unmaps;
        ++diagnostics_.absent_unmap_rejects;
        return {false, "PML4 entry absent"};
    }
    uint64_t* pdpt = table_from_phys(pml4e);
    uint64_t pdpte = pdpt[pdpt_index(virt)];
    if ((pdpte & PagePresent) == 0) {
        ++diagnostics_.failed_unmaps;
        ++diagnostics_.absent_unmap_rejects;
        return {false, "PDPT entry absent"};
    }
    uint64_t* pd = table_from_phys(pdpte);
    uint64_t pde = pd[pd_index(virt)];
    if ((pde & PagePresent) == 0) {
        ++diagnostics_.failed_unmaps;
        ++diagnostics_.absent_unmap_rejects;
        return {false, "PD entry absent"};
    }
    uint64_t* pt = table_from_phys(pde);
    uint64_t& pte = pt[pt_index(virt)];
    if ((pte & PagePresent) == 0) {
        ++diagnostics_.failed_unmaps;
        ++diagnostics_.absent_unmap_rejects;
        return {false, "PT entry absent"};
    }
    pte = 0;
    invalidate_local_page(virt);
    if (!hk::timer::lapic_timer_active()) {
        hk::smp::shootdown_remote_tlbs(virt);
        ++diagnostics_.remote_shootdowns_requested;
    }
    ++diagnostics_.unmapped_pages;
    diagnostics_.last_unmapped_virt = virt;
    return {true, "ok"};
}

uint64_t VirtualMemoryManager::translate(uint64_t virt) const {
    uint64_t* pml4 = table_from_phys(root_);
    uint64_t pml4e = pml4[pml4_index(virt)];
    if ((pml4e & PagePresent) == 0) return 0;
    uint64_t* pdpt = table_from_phys(pml4e);
    uint64_t pdpte = pdpt[pdpt_index(virt)];
    if ((pdpte & PagePresent) == 0) return 0;
    if (pdpte & (1ull << 7)) return (pdpte & 0x000fffffc0000000ull) + (virt & 0x3fffffffull);
    uint64_t* pd = table_from_phys(pdpte);
    uint64_t pde = pd[pd_index(virt)];
    if ((pde & PagePresent) == 0) return 0;
    if (pde & (1ull << 7)) return (pde & 0x000fffffffe00000ull) + (virt & 0x1fffffull);
    uint64_t* pt = table_from_phys(pde);
    uint64_t pte = pt[pt_index(virt)];
    if ((pte & PagePresent) == 0) return 0;
    return (pte & kAddrMask) + (virt & 0xfff);
}

} // namespace hk::mm
