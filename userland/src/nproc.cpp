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

}

extern "C" [[noreturn]] void _start() {
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetCpuCount);
    if (result.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[nproc] ", "error ", result.error);
        hybrid::user::exit(1);
    }

    char plain[32];
    uint64_t plain_cursor = 0;
    append_decimal(plain, sizeof(plain), plain_cursor, result.value);
    hybrid::user::write_line(plain);

    char line[96];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[nproc] online ");
    append_decimal(line, sizeof(line), cursor, result.value);
    hybrid::user::write_line(line);
    hybrid::user::write_hex_line("[nproc] ", "online hex ", result.value);
    hybrid::user::exit(0);
}
