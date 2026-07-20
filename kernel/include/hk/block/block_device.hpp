#pragma once
#include <stddef.h>
#include <stdint.h>

namespace hk::block {

struct BlockStats {
    bool initialized;
    uint64_t sector_count;
    uint64_t sector_reads;
    uint64_t multi_sector_reads;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t cache_evictions;
    uint64_t invalid_reads;
    uint64_t null_buffer_rejects;
    uint64_t zero_count_rejects;
    uint64_t oversized_request_rejects;
    uint64_t backend_read_failures;
    uint64_t cache_fills;
    uint64_t cached_entries;
    uint64_t largest_request_sectors;
    uint64_t last_lba;
};

class BlockDevice {
public:
    bool initialize_ahci();
    bool read_sector(uint64_t lba, void* out_512);
    bool read_sectors(uint64_t start_lba, uint32_t sector_count, void* out);
    BlockStats stats() const { return stats_; }
private:
    struct CacheEntry {
        bool valid;
        uint64_t lba;
        unsigned char data[512];
    };

    static constexpr uint32_t kCacheEntries = 16;
    CacheEntry cache_[kCacheEntries]{};
    uint32_t next_victim_ = 0;
    BlockStats stats_{};
    void refresh_cached_entry_count();
};

BlockDevice& boot_disk();
bool self_test();

} // namespace hk::block
