#include "hk/block/block_device.hpp"
#include "hk/drivers/ahci.hpp"
#include "hk/lib/string.hpp"
#include "hk/log.hpp"

namespace hk::block {

BlockDevice& boot_disk() {
    static BlockDevice device;
    return device;
}

bool BlockDevice::initialize_ahci() {
    stats_ = {};
    next_victim_ = 0;
    for (auto& entry : cache_) entry = CacheEntry{};
    const auto& controller = hk::drivers::ahci::driver().controller();
    stats_.initialized = controller.identify_success && controller.read_lba0_success;
    hk::log_hex(hk::LogLevel::Info, "Block boot disk initialized", stats_.initialized ? 1 : 0);
    return stats_.initialized;
}

void BlockDevice::refresh_cached_entry_count() {
    uint64_t count = 0;
    for (const auto& entry : cache_) {
        if (entry.valid) ++count;
    }
    stats_.cached_entries = count;
}

bool BlockDevice::read_sector(uint64_t lba, void* out_512) {
    if (!stats_.initialized) {
        ++stats_.invalid_reads;
        return false;
    }
    if (!out_512) {
        ++stats_.invalid_reads;
        ++stats_.null_buffer_rejects;
        return false;
    }
    ++stats_.sector_reads;
    stats_.last_lba = lba;
    for (auto& entry : cache_) {
        if (entry.valid && entry.lba == lba) {
            ++stats_.cache_hits;
            memcpy(out_512, entry.data, sizeof(entry.data));
            return true;
        }
    }

    CacheEntry& victim = cache_[next_victim_++ % kCacheEntries];
    if (victim.valid) ++stats_.cache_evictions;
    if (!hk::drivers::ahci::driver().read_sector(lba, victim.data)) {
        ++stats_.backend_read_failures;
        return false;
    }
    victim.valid = true;
    victim.lba = lba;
    ++stats_.cache_fills;
    ++stats_.cache_misses;
    refresh_cached_entry_count();
    memcpy(out_512, victim.data, sizeof(victim.data));
    return true;
}

bool BlockDevice::read_sectors(uint64_t start_lba, uint32_t sector_count, void* out) {
    if (!stats_.initialized) {
        ++stats_.invalid_reads;
        return false;
    }
    if (!out) {
        ++stats_.invalid_reads;
        ++stats_.null_buffer_rejects;
        return false;
    }
    if (sector_count == 0) {
        ++stats_.invalid_reads;
        ++stats_.zero_count_rejects;
        return false;
    }
    if (sector_count > 64) {
        ++stats_.invalid_reads;
        ++stats_.oversized_request_rejects;
        return false;
    }
    ++stats_.multi_sector_reads;
    if (sector_count > stats_.largest_request_sectors) stats_.largest_request_sectors = sector_count;
    auto* bytes = static_cast<unsigned char*>(out);
    for (uint32_t i = 0; i < sector_count; ++i) {
        if (!read_sector(start_lba + i, bytes + static_cast<uint64_t>(i) * 512u)) return false;
    }
    return true;
}

bool self_test() {
    unsigned char first[512]{};
    unsigned char second[512]{};
    unsigned char pair[1024]{};
    auto& disk = boot_disk();
    if (!disk.stats().initialized) return false;
    uint64_t invalid_before = disk.stats().invalid_reads;
    uint64_t null_before = disk.stats().null_buffer_rejects;
    uint64_t zero_before = disk.stats().zero_count_rejects;
    uint64_t oversized_before = disk.stats().oversized_request_rejects;
    if (disk.read_sector(0, nullptr)) return false;
    if (disk.read_sectors(0, 1, nullptr)) return false;
    if (disk.read_sectors(0, 0, pair)) return false;
    if (disk.read_sectors(0, 65, pair)) return false;
    if (disk.stats().invalid_reads < invalid_before + 4) return false;
    if (disk.stats().null_buffer_rejects < null_before + 2) return false;
    if (disk.stats().zero_count_rejects < zero_before + 1) return false;
    if (disk.stats().oversized_request_rejects < oversized_before + 1) return false;
    if (!disk.read_sector(0, first)) return false;
    if (!disk.read_sector(0, second)) return false;
    if (!disk.read_sectors(0, 2, pair)) return false;
    if (first[510] != 0x55 || first[511] != 0xaa) return false;
    if (second[510] != 0x55 || second[511] != 0xaa) return false;
    if (pair[510] != 0x55 || pair[511] != 0xaa) return false;
    unsigned char sector[512]{};
    for (uint32_t lba = 1; lba <= 17; ++lba) {
        if (!disk.read_sector(lba, sector)) return false;
    }
    if (disk.stats().cache_hits == 0 || disk.stats().cache_misses == 0) return false;
    if (disk.stats().cache_evictions == 0 || disk.stats().cached_entries == 0) return false;
    if (disk.stats().cache_fills < disk.stats().cache_misses) return false;
    if (disk.stats().backend_read_failures != 0) return false;
    if (disk.stats().multi_sector_reads == 0 || disk.stats().largest_request_sectors < 2) return false;
    hk::log_hex(hk::LogLevel::Info, "Block cache reads", disk.stats().sector_reads);
    hk::log_hex(hk::LogLevel::Info, "Block cache multi reads", disk.stats().multi_sector_reads);
    hk::log_hex(hk::LogLevel::Info, "Block cache hits", disk.stats().cache_hits);
    hk::log_hex(hk::LogLevel::Info, "Block cache misses", disk.stats().cache_misses);
    hk::log_hex(hk::LogLevel::Info, "Block cache evictions", disk.stats().cache_evictions);
    hk::log_hex(hk::LogLevel::Info, "Block cache invalid reads", disk.stats().invalid_reads);
    hk::log_hex(hk::LogLevel::Info, "Block cache null rejects", disk.stats().null_buffer_rejects);
    hk::log_hex(hk::LogLevel::Info, "Block cache zero rejects", disk.stats().zero_count_rejects);
    hk::log_hex(hk::LogLevel::Info, "Block cache oversized rejects", disk.stats().oversized_request_rejects);
    hk::log_hex(hk::LogLevel::Info, "Block cache backend failures", disk.stats().backend_read_failures);
    hk::log_hex(hk::LogLevel::Info, "Block cache fills", disk.stats().cache_fills);
    hk::log_hex(hk::LogLevel::Info, "Block cache cached entries", disk.stats().cached_entries);
    hk::log_hex(hk::LogLevel::Info, "Block cache largest request", disk.stats().largest_request_sectors);
    hk::log_hex(hk::LogLevel::Info, "Block cache last LBA", disk.stats().last_lba);
    return true;
}

} // namespace hk::block
