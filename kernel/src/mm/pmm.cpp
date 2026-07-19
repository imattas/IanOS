#include "hk/mm/pmm.hpp"
#include "hk/sync/spinlock.hpp"

namespace hk::mm {

namespace {
hk::sync::SpinLock pmm_lock;
}

PhysicalMemoryManager& pmm() {
    static PhysicalMemoryManager manager;
    return manager;
}

void PhysicalMemoryManager::set_used(uint64_t page, bool used) {
    if (page >= kMaxPages) return;
    uint8_t mask = static_cast<uint8_t>(1u << (page & 7));
    if (used) bitmap_[page >> 3] |= mask;
    else bitmap_[page >> 3] &= static_cast<uint8_t>(~mask);
}

bool PhysicalMemoryManager::is_used(uint64_t page) const {
    if (page >= kMaxPages) return true;
    return bitmap_[page >> 3] & static_cast<uint8_t>(1u << (page & 7));
}

void PhysicalMemoryManager::update_peak_used() {
    uint64_t used = total_pages_ - free_pages_;
    if (used > diagnostics_.peak_used_pages) diagnostics_.peak_used_pages = used;
}

void PhysicalMemoryManager::initialize(const hybrid::BootInfo& boot) {
    hk::sync::LockGuard guard(pmm_lock);
    for (uint64_t i = 0; i < sizeof(bitmap_); ++i) bitmap_[i] = 0xff;
    total_pages_ = 0;
    free_pages_ = 0;
    scan_hint_ = 0x100;
    stats_ = {};
    diagnostics_ = {};
    auto* regions = reinterpret_cast<const hybrid::MemoryRegion*>(boot.memory_map);
    for (uint64_t i = 0; i < boot.memory_map_entries; ++i) {
        const auto& r = regions[i];
        if (r.base + r.length > stats_.highest_physical) stats_.highest_physical = r.base + r.length;
        uint64_t first = align_up(r.base) / kPageSize;
        uint64_t last = align_down(r.base + r.length) / kPageSize;
        if (last > kMaxPages) last = kMaxPages;
        if (last > total_pages_) total_pages_ = last;
        if (r.type == hybrid::MemoryType::Usable || r.type == hybrid::MemoryType::BootServices) {
            stats_.usable_bytes += r.length;
            for (uint64_t page = first; page < last; ++page) {
                if (page < 0x100) continue;
                if (is_used(page)) {
                    set_used(page, false);
                    ++free_pages_;
                }
            }
        } else {
            stats_.reserved_bytes += r.length;
        }
    }
    mark_range_used(0, 0x100000);
    mark_range_used(boot.kernel_physical_base, boot.kernel_physical_end - boot.kernel_physical_base);
    mark_range_used(boot.user_init_base, boot.user_init_size);
    mark_range_used(boot.boot_modules, boot.boot_module_count * sizeof(hybrid::BootModule));
    auto* modules = reinterpret_cast<const hybrid::BootModule*>(boot.boot_modules);
    for (uint64_t i = 0; i < boot.boot_module_count; ++i) {
        mark_range_used(modules[i].base, modules[i].size);
    }
    mark_range_used(reinterpret_cast<uint64_t>(&boot), sizeof(boot));
    mark_range_used(boot.memory_map, boot.memory_map_entries * boot.memory_map_descriptor_size);
    mark_range_used(boot.framebuffer.base, static_cast<uint64_t>(boot.framebuffer.pixels_per_scanline) * boot.framebuffer.height * boot.framebuffer.bytes_per_pixel);
    if (boot.rsdp != 0) mark_range_used(align_down(boot.rsdp), kPageSize);
    stats_.total_pages = total_pages_;
    stats_.free_pages = free_pages_;
    stats_.used_pages = total_pages_ - free_pages_;
    update_peak_used();
}

uint64_t PhysicalMemoryManager::allocate_page() {
    hk::sync::LockGuard guard(pmm_lock);
    ++diagnostics_.allocate_page_calls;
    for (uint64_t page = scan_hint_; page < total_pages_; ++page) {
        if (!is_used(page)) {
            set_used(page, true);
            --free_pages_;
            stats_.free_pages = free_pages_;
            stats_.used_pages = total_pages_ - free_pages_;
            scan_hint_ = page + 1;
            diagnostics_.last_allocated_page = page;
            update_peak_used();
            return page * kPageSize;
        }
    }
    for (uint64_t page = 0x100; page < scan_hint_; ++page) {
        if (!is_used(page)) {
            set_used(page, true);
            --free_pages_;
            stats_.free_pages = free_pages_;
            stats_.used_pages = total_pages_ - free_pages_;
            scan_hint_ = page + 1;
            diagnostics_.last_allocated_page = page;
            update_peak_used();
            return page * kPageSize;
        }
    }
    ++diagnostics_.failed_allocations;
    return 0;
}

uint64_t PhysicalMemoryManager::allocate_contiguous(uint64_t page_count) {
    hk::sync::LockGuard guard(pmm_lock);
    ++diagnostics_.allocate_contiguous_calls;
    if (page_count == 0) {
        ++diagnostics_.failed_allocations;
        return 0;
    }
    uint64_t run_start = 0;
    uint64_t run = 0;
    for (uint64_t page = 0x100; page < total_pages_; ++page) {
        if (!is_used(page)) {
            if (run == 0) run_start = page;
            ++run;
            if (run == page_count) {
                for (uint64_t p = run_start; p < run_start + page_count; ++p) set_used(p, true);
                free_pages_ -= page_count;
                stats_.free_pages = free_pages_;
                stats_.used_pages = total_pages_ - free_pages_;
                scan_hint_ = run_start + page_count;
                diagnostics_.last_allocated_page = run_start;
                update_peak_used();
                return run_start * kPageSize;
            }
        } else {
            run = 0;
        }
    }
    ++diagnostics_.failed_allocations;
    return 0;
}

void PhysicalMemoryManager::free_page(uint64_t physical) {
    ++diagnostics_.free_page_calls;
    free_contiguous(physical, 1);
}

void PhysicalMemoryManager::free_contiguous(uint64_t physical, uint64_t page_count) {
    hk::sync::LockGuard guard(pmm_lock);
    ++diagnostics_.free_contiguous_calls;
    uint64_t page = physical / kPageSize;
    if (physical == 0 || (physical & (kPageSize - 1)) || page < 0x100 || page_count == 0) {
        ++diagnostics_.invalid_frees;
        return;
    }
    bool freed = false;
    for (uint64_t i = 0; i < page_count; ++i) {
        if (page + i >= total_pages_ || !is_used(page + i)) continue;
        set_used(page + i, false);
        ++free_pages_;
        freed = true;
    }
    if (!freed) ++diagnostics_.invalid_frees;
    stats_.free_pages = free_pages_;
    stats_.used_pages = total_pages_ - free_pages_;
    if (page < scan_hint_) scan_hint_ = page;
}

void PhysicalMemoryManager::mark_range_used(uint64_t base, uint64_t length) {
    uint64_t first = align_down(base) / kPageSize;
    uint64_t last = align_up(base + length) / kPageSize;
    if (last > total_pages_) last = total_pages_;
    for (uint64_t page = first; page < last; ++page) {
        if (!is_used(page)) {
            set_used(page, true);
            if (free_pages_ > 0) --free_pages_;
        }
    }
    stats_.free_pages = free_pages_;
    stats_.used_pages = total_pages_ - free_pages_;
}

void PhysicalMemoryManager::mark_range_free(uint64_t base, uint64_t length) {
    uint64_t first = align_up(base) / kPageSize;
    uint64_t last = align_down(base + length) / kPageSize;
    if (last > total_pages_) last = total_pages_;
    for (uint64_t page = first; page < last; ++page) {
        if (page < 0x100) continue;
        if (is_used(page)) {
            set_used(page, false);
            ++free_pages_;
        }
    }
    stats_.free_pages = free_pages_;
    stats_.used_pages = total_pages_ - free_pages_;
}

} // namespace hk::mm
