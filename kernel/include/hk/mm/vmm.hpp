#pragma once
#include <stdint.h>
namespace hk::mm {

enum PageFlags : uint64_t {
    PagePresent = 1ull << 0,
    PageWrite = 1ull << 1,
    PageUser = 1ull << 2,
    PageWriteThrough = 1ull << 3,
    PageCacheDisable = 1ull << 4,
    PageGlobal = 1ull << 8,
    PageNoExecute = 1ull << 63,
};

struct MappingResult {
    bool ok;
    const char* error;
};

struct VirtualMemoryDiagnostics {
    uint64_t map_page_calls;
    uint64_t map_range_calls;
    uint64_t unmap_page_calls;
    uint64_t mapped_pages;
    uint64_t unmapped_pages;
    uint64_t failed_maps;
    uint64_t failed_unmaps;
    uint64_t duplicate_map_rejects;
    uint64_t unaligned_map_rejects;
    uint64_t absent_unmap_rejects;
    uint64_t remote_shootdowns_requested;
    uint64_t last_mapped_virt;
    uint64_t last_unmapped_virt;
};

class VirtualMemoryManager {
public:
    void initialize_identity();
    MappingResult map_page(uint64_t virt, uint64_t phys, uint64_t flags);
    MappingResult map_range(uint64_t virt, uint64_t phys, uint64_t length, uint64_t flags);
    MappingResult unmap_page(uint64_t virt);
    uint64_t translate(uint64_t virt) const;
    uint64_t active_pml4() const;
    void load_cr3(uint64_t physical);
    void invalidate_local_page(uint64_t virt);
    VirtualMemoryDiagnostics diagnostics() const { return diagnostics_; }
private:
    uint64_t root_ = 0;
    VirtualMemoryDiagnostics diagnostics_{};
    uint64_t* table_from_phys(uint64_t physical) const;
    uint64_t* ensure_table(uint64_t* table, uint16_t index);
};
VirtualMemoryManager& vmm();
}
