#pragma once
#include <stddef.h>
#include <stdint.h>
#include "hybrid/boot_info.hpp"
namespace hk::mm {
constexpr uint64_t kPageSize = 4096;
constexpr uint64_t align_down(uint64_t value, uint64_t align = kPageSize) { return value & ~(align - 1); }
constexpr uint64_t align_up(uint64_t value, uint64_t align = kPageSize) { return (value + align - 1) & ~(align - 1); }

struct MemoryStats {
    uint64_t total_pages;
    uint64_t free_pages;
    uint64_t used_pages;
    uint64_t usable_bytes;
    uint64_t reserved_bytes;
    uint64_t highest_physical;
};

struct PhysicalMemoryDiagnostics {
    uint64_t allocate_page_calls;
    uint64_t allocate_contiguous_calls;
    uint64_t free_page_calls;
    uint64_t free_contiguous_calls;
    uint64_t failed_allocations;
    uint64_t invalid_frees;
    uint64_t peak_used_pages;
    uint64_t last_allocated_page;
};

class PhysicalMemoryManager {
public:
    void initialize(const hybrid::BootInfo& boot);
    uint64_t allocate_page();
    uint64_t allocate_contiguous(uint64_t page_count);
    void free_page(uint64_t physical);
    void free_contiguous(uint64_t physical, uint64_t page_count);
    void mark_range_used(uint64_t base, uint64_t length);
    void mark_range_free(uint64_t base, uint64_t length);
    uint64_t total_pages() const { return total_pages_; }
    uint64_t free_pages() const { return free_pages_; }
    MemoryStats stats() const { return stats_; }
    PhysicalMemoryDiagnostics diagnostics() const { return diagnostics_; }
    uint64_t free_run_count(uint64_t order) const;
private:
    static constexpr uint64_t kMaxPages = 1024 * 1024;
    uint8_t bitmap_[kMaxPages / 8]{};
    uint64_t total_pages_ = 0;
    uint64_t free_pages_ = 0;
    uint64_t scan_hint_ = 0;
    MemoryStats stats_{};
    PhysicalMemoryDiagnostics diagnostics_{};
    void set_used(uint64_t page, bool used);
    bool is_used(uint64_t page) const;
    void update_peak_used();
};
PhysicalMemoryManager& pmm();
}
