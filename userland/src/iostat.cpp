#include "hybrid/user.hpp"

namespace {

uint64_t hit_percent_x100(const hybrid::BlockDeviceInfo& info) {
    uint64_t total = info.cache_hits + info.cache_misses;
    if (total == 0) return 0;
    return (info.cache_hits * 10000u) / total;
}

void write_percent_line(const char* prefix, const char* label, uint64_t value_x100) {
    char line[128];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, prefix);
    hybrid::user::append_text(line, sizeof(line), cursor, label);
    hybrid::user::append_hex(line, sizeof(line), cursor, value_x100 / 100u);
    hybrid::user::append_text(line, sizeof(line), cursor, ".");
    uint64_t fraction = value_x100 % 100u;
    hybrid::user::append_char(line, sizeof(line), cursor, static_cast<char>('0' + ((fraction / 10u) % 10u)));
    hybrid::user::append_char(line, sizeof(line), cursor, static_cast<char>('0' + (fraction % 10u)));
    hybrid::user::write_line(line);
}

int main_result() {
    hybrid::BlockDeviceInfo info;
    auto* bytes = reinterpret_cast<unsigned char*>(&info);
    for (uint64_t i = 0; i < sizeof(info); ++i) bytes[i] = 0;

    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetBlockDeviceInfo, reinterpret_cast<uint64_t>(&info));
    if (result.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[iostat] ", "block error ", result.error);
        return 1;
    }

    hybrid::user::write_hex_line("[iostat] ", "initialized ", info.initialized);
    hybrid::user::write_hex_line("[iostat] ", "sector size ", info.sector_size);
    hybrid::user::write_hex_line("[iostat] ", "sector reads ", info.sector_reads);
    hybrid::user::write_hex_line("[iostat] ", "read bytes ", info.sector_reads * info.sector_size);
    hybrid::user::write_hex_line("[iostat] ", "cache hits ", info.cache_hits);
    hybrid::user::write_hex_line("[iostat] ", "cache misses ", info.cache_misses);
    hybrid::user::write_hex_line("[iostat] ", "cache fills ", info.cache_fills);
    hybrid::user::write_hex_line("[iostat] ", "cache evictions ", info.cache_evictions);
    hybrid::user::write_hex_line("[iostat] ", "cached entries ", info.cached_entries);
    hybrid::user::write_hex_line("[iostat] ", "largest request sectors ", info.largest_request_sectors);
    hybrid::user::write_hex_line("[iostat] ", "last lba ", info.last_lba);
    hybrid::user::write_hex_line("[iostat] ", "invalid reads ", info.invalid_reads);
    hybrid::user::write_hex_line("[iostat] ", "backend failures ", info.backend_read_failures);
    write_percent_line("[iostat] ", "cache hit percent ", hit_percent_x100(info));
    return info.initialized != 0 && info.sector_size != 0 ? 0 : 2;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::user::exit(static_cast<uint64_t>(main_result()));
}
