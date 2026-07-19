#include "hybrid/user.hpp"

namespace {

uint64_t used_percent_x100(const hybrid::MemoryStatsInfo& info) {
    if (info.total_pages == 0) return 0;
    return (info.used_pages * 10000u) / info.total_pages;
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
    hybrid::MemoryStatsInfo info;
    auto* bytes = reinterpret_cast<unsigned char*>(&info);
    for (uint64_t i = 0; i < sizeof(info); ++i) bytes[i] = 0;

    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetMemoryStats, reinterpret_cast<uint64_t>(&info));
    if (result.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[lsmem] ", "memory error ", result.error);
        return 1;
    }

    hybrid::user::write_hex_line("[lsmem] ", "range start ", 0);
    hybrid::user::write_hex_line("[lsmem] ", "range end ", info.highest_physical);
    hybrid::user::write_hex_line("[lsmem] ", "total pages ", info.total_pages);
    hybrid::user::write_hex_line("[lsmem] ", "used pages ", info.used_pages);
    hybrid::user::write_hex_line("[lsmem] ", "free pages ", info.free_pages);
    hybrid::user::write_hex_line("[lsmem] ", "usable bytes ", info.usable_bytes);
    hybrid::user::write_hex_line("[lsmem] ", "reserved bytes ", info.reserved_bytes);
    hybrid::user::write_hex_line("[lsmem] ", "highest physical ", info.highest_physical);
    write_percent_line("[lsmem] ", "used percent ", used_percent_x100(info));
    return info.total_pages != 0 && info.highest_physical != 0 ? 0 : 2;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::user::exit(static_cast<uint64_t>(main_result()));
}
