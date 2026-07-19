#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

bool parse_i64(const char* text, int64_t& out) {
    if (!text || text[0] == 0) return false;
    bool neg = false;
    uint64_t index = 0;
    if (text[0] == '-') {
        neg = true;
        index = 1;
    }
    if (text[index] == 0) return false;
    int64_t value = 0;
    for (; text[index] != 0; ++index) {
        if (text[index] < '0' || text[index] > '9') return false;
        value = value * 10 + static_cast<int64_t>(text[index] - '0');
    }
    out = neg ? -value : value;
    return true;
}

void write_char(char c) {
    hybrid::user::syscall(hybrid::SyscallNumber::Write, hybrid::kStdoutFd, reinterpret_cast<uint64_t>(&c), 1);
}

void write_u64(uint64_t value, uint64_t base, bool sign, bool negative) {
    char digits[32];
    uint64_t count = 0;
    do {
        const uint64_t digit = value % base;
        digits[count++] = digit < 10 ? static_cast<char>('0' + digit) : static_cast<char>('a' + digit - 10);
        value /= base;
    } while (value != 0 && count < sizeof(digits));
    if (sign && negative) write_char('-');
    while (count != 0) write_char(digits[--count]);
}

char escape_char(char c) {
    switch (c) {
    case 'n': return '\n';
    case 'r': return '\r';
    case 't': return '\t';
    case '0': return '\0';
    case '\\': return '\\';
    default: return c;
    }
}

void emit_format(const char* format) {
    uint64_t next_arg = 2;
    for (uint64_t i = 0; format[i] != 0; ++i) {
        if (format[i] == '\\') {
            if (format[i + 1] != 0) {
                write_char(escape_char(format[++i]));
            }
            continue;
        }
        if (format[i] != '%') {
            write_char(format[i]);
            continue;
        }
        const char spec = format[++i];
        if (spec == 0) break;
        if (spec == '%') {
            write_char('%');
            continue;
        }

        hybrid::ArgumentInfo arg{};
        const bool has_arg = get_arg(next_arg, arg);
        if (has_arg) ++next_arg;
        const char* value = has_arg ? arg.value : "";

        if (spec == 's') {
            hybrid::user::write_text(value);
        } else if (spec == 'c') {
            write_char(value[0]);
        } else if (spec == 'd' || spec == 'i') {
            int64_t parsed = 0;
            const bool ok = parse_i64(value, parsed);
            const bool neg = ok && parsed < 0;
            const uint64_t magnitude = neg ? static_cast<uint64_t>(-parsed) : static_cast<uint64_t>(parsed);
            write_u64(magnitude, 10, true, neg);
        } else if (spec == 'x') {
            int64_t parsed = 0;
            parse_i64(value, parsed);
            write_u64(static_cast<uint64_t>(parsed), 16, false, false);
        } else {
            write_char('%');
            write_char(spec);
        }
    }
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo format{};
    if (!get_arg(1, format)) {
        hybrid::user::write_line("[printf] usage printf <format> [args...]");
        hybrid::user::exit(1);
    }
    emit_format(format.value);
    hybrid::user::exit(0);
}
