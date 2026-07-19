#include "hybrid/user.hpp"

namespace {

void append_decimal(char* buffer, uint64_t capacity, uint64_t& cursor, uint64_t value) {
    char digits[20];
    uint64_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && count < sizeof(digits));
    while (count != 0) hybrid::user::append_char(buffer, capacity, cursor, digits[--count]);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::CurrentIdsInfo ids;
    auto* bytes = reinterpret_cast<unsigned char*>(&ids);
    for (uint64_t i = 0; i < sizeof(ids); ++i) bytes[i] = 0;

    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetCurrentIds, reinterpret_cast<uint64_t>(&ids));
    if (result.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[id] ", "error ", result.error);
        hybrid::user::exit(1);
    }

    char line[160];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "uid=0(root) gid=0(root) euid=0(root) egid=0(root) pid=");
    append_decimal(line, sizeof(line), cursor, ids.pid);
    hybrid::user::append_text(line, sizeof(line), cursor, " tid=");
    append_decimal(line, sizeof(line), cursor, ids.tid);
    hybrid::user::append_text(line, sizeof(line), cursor, " pgid=");
    append_decimal(line, sizeof(line), cursor, ids.process_group_id);
    hybrid::user::write_line(line);
    hybrid::user::exit((ids.pid == 0 || ids.tid == 0) ? 1 : 0);
}
