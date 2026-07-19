#include "hk/mm/address_space.hpp"
#include "hk/mm/pmm.hpp"
#include "hk/lib/string.hpp"

namespace hk::mm {

namespace {
constexpr uint64_t kAddrMask = 0x000ffffffffff000ull;
uint16_t pml4_index(uint64_t v) { return static_cast<uint16_t>((v >> 39) & 0x1ff); }
uint16_t pdpt_index(uint64_t v) { return static_cast<uint16_t>((v >> 30) & 0x1ff); }
uint16_t pd_index(uint64_t v) { return static_cast<uint16_t>((v >> 21) & 0x1ff); }
uint16_t pt_index(uint64_t v) { return static_cast<uint16_t>((v >> 12) & 0x1ff); }
uint64_t* table(uint64_t physical) { return reinterpret_cast<uint64_t*>(physical & kAddrMask); }

uint64_t* ensure_table(uint64_t* parent, uint16_t index, uint64_t flags) {
    if ((parent[index] & PagePresent) == 0) {
        uint64_t page = pmm().allocate_page();
        if (page == 0) return nullptr;
        memset(reinterpret_cast<void*>(page), 0, kPageSize);
        parent[index] = page | PagePresent | PageWrite | (flags & PageUser);
    }
    return table(parent[index]);
}
}

AddressSpace create_address_space() {
    uint64_t root = pmm().allocate_page();
    if (root == 0) return {0};
    memset(reinterpret_cast<void*>(root), 0, kPageSize);
    auto* dst = table(root);
    auto* src = table(vmm().active_pml4());
    for (uint16_t i = 256; i < 512; ++i) dst[i] = src[i];
    return {root};
}

MappingResult map_page(AddressSpace& space, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (space.pml4 == 0) return {false, "address space has no PML4"};
    if ((virt & (kPageSize - 1)) || (phys & (kPageSize - 1))) return {false, "unaligned address-space map"};
    auto* pml4 = table(space.pml4);
    auto* pdpt = ensure_table(pml4, pml4_index(virt), flags);
    if (!pdpt) return {false, "failed to allocate address-space PDPT"};
    auto* pd = ensure_table(pdpt, pdpt_index(virt), flags);
    if (!pd) return {false, "failed to allocate address-space PD"};
    auto* pt = ensure_table(pd, pd_index(virt), flags);
    if (!pt) return {false, "failed to allocate address-space PT"};
    if (pt[pt_index(virt)] & PagePresent) return {false, "address-space virtual page is already mapped"};
    pt[pt_index(virt)] = (phys & kAddrMask) | (flags & ~kAddrMask) | PagePresent;
    return {true, "ok"};
}

uint64_t translate(const AddressSpace& space, uint64_t virt) {
    if (space.pml4 == 0) return 0;
    auto* pml4 = table(space.pml4);
    uint64_t pml4e = pml4[pml4_index(virt)];
    if ((pml4e & PagePresent) == 0) return 0;
    auto* pdpt = table(pml4e);
    uint64_t pdpte = pdpt[pdpt_index(virt)];
    if ((pdpte & PagePresent) == 0) return 0;
    auto* pd = table(pdpte);
    uint64_t pde = pd[pd_index(virt)];
    if ((pde & PagePresent) == 0) return 0;
    auto* pt = table(pde);
    uint64_t pte = pt[pt_index(virt)];
    if ((pte & PagePresent) == 0) return 0;
    return (pte & kAddrMask) + (virt & 0xfff);
}

uint64_t page_flags(const AddressSpace& space, uint64_t virt) {
    if (space.pml4 == 0) return 0;
    auto* pml4 = table(space.pml4);
    uint64_t pml4e = pml4[pml4_index(virt)];
    if ((pml4e & PagePresent) == 0) return 0;
    auto* pdpt = table(pml4e);
    uint64_t pdpte = pdpt[pdpt_index(virt)];
    if ((pdpte & PagePresent) == 0) return 0;
    auto* pd = table(pdpte);
    uint64_t pde = pd[pd_index(virt)];
    if ((pde & PagePresent) == 0) return 0;
    auto* pt = table(pde);
    return pt[pt_index(virt)];
}

void destroy_address_space(AddressSpace& space) {
    if (space.pml4 == 0) return;
    auto* pml4 = table(space.pml4);
    for (uint16_t pml4i = 0; pml4i < 256; ++pml4i) {
        uint64_t pml4e = pml4[pml4i];
        if ((pml4e & PagePresent) == 0) continue;
        auto* pdpt = table(pml4e);
        for (uint16_t pdpti = 0; pdpti < 512; ++pdpti) {
            uint64_t pdpte = pdpt[pdpti];
            if ((pdpte & PagePresent) == 0) continue;
            auto* pd = table(pdpte);
            for (uint16_t pdi = 0; pdi < 512; ++pdi) {
                uint64_t pde = pd[pdi];
                if ((pde & PagePresent) == 0) continue;
                pmm().free_page(pde & kAddrMask);
            }
            pmm().free_page(pdpte & kAddrMask);
        }
        pmm().free_page(pml4e & kAddrMask);
    }
    pmm().free_page(space.pml4);
    space.pml4 = 0;
}

bool address_space_self_test() {
    AddressSpace space = create_address_space();
    if (space.pml4 == 0) return false;
    uint64_t phys = pmm().allocate_page();
    if (phys == 0) return false;
    constexpr uint64_t user_page = 0x0000000000400000ull;
    auto mapped = map_page(space, user_page, phys, PageWrite | PageUser);
    if (!mapped.ok) return false;
    if (translate(space, user_page) != phys) return false;
    if (translate(space, user_page + 0x120) != phys + 0x120) return false;
    uint64_t free_before_destroy = pmm().free_pages();
    pmm().free_page(phys);
    destroy_address_space(space);
    if (space.pml4 != 0 || pmm().free_pages() <= free_before_destroy) return false;
    return true;
}

} // namespace hk::mm
