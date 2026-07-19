#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

bool parse_u64(const char* text, uint64_t& out) {
    if (!text || text[0] == 0) return false;
    uint64_t value = 0;
    for (uint64_t i = 0; text[i] != 0; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + static_cast<uint64_t>(text[i] - '0');
    }
    out = value;
    return true;
}

void append_decimal(char* out, uint64_t capacity, uint64_t& cursor, uint64_t value) {
    char digits[24];
    uint64_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && count < sizeof(digits));
    while (count > 0) hybrid::user::append_char(out, capacity, cursor, digits[--count]);
}

void emit_number(uint64_t value) {
    char line[64];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[seq] ");
    append_decimal(line, sizeof(line), cursor, value);
    hybrid::user::write_line(line);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo first_arg{};
    hybrid::ArgumentInfo last_arg{};
    if (!get_arg(1, first_arg)) {
        hybrid::user::write_line("[seq] usage seq [first] <last>");
        hybrid::user::exit(1);
    }

    uint64_t first = 1;
    uint64_t last = 0;
    if (get_arg(2, last_arg)) {
        if (!parse_u64(first_arg.value, first) || !parse_u64(last_arg.value, last)) {
            hybrid::user::write_line("[seq] bad number");
            hybrid::user::exit(1);
        }
    } else if (!parse_u64(first_arg.value, last)) {
        hybrid::user::write_line("[seq] bad number");
        hybrid::user::exit(1);
    }

    uint64_t count = 0;
    if (first <= last) {
        for (uint64_t value = first;; ++value) {
            emit_number(value);
            ++count;
            if (value == last) break;
        }
    }
    hybrid::user::write_hex_line("[seq] ", "count ", count);
    hybrid::user::exit(count);
}
