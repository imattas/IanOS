#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

bool parse_size(const char* text, uint64_t& out) {
    if (!text || text[0] == 0) return false;
    uint64_t base = 10;
    uint64_t cursor = 0;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        cursor = 2;
        if (text[cursor] == 0) return false;
    }
    uint64_t value = 0;
    for (; text[cursor] != 0; ++cursor) {
        char c = text[cursor];
        uint64_t digit = 0;
        if (c >= '0' && c <= '9') digit = static_cast<uint64_t>(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f') digit = static_cast<uint64_t>(10 + c - 'a');
        else if (base == 16 && c >= 'A' && c <= 'F') digit = static_cast<uint64_t>(10 + c - 'A');
        else return false;
        if (digit >= base) return false;
        value = value * base + digit;
    }
    out = value;
    return true;
}

}

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo path;
    hybrid::ArgumentInfo size_arg;
    if (!get_arg(1, path) || !get_arg(2, size_arg)) {
        hybrid::user::write_line("[truncate] usage: truncate <path> <size>");
        hybrid::user::exit(1);
    }
    uint64_t size = 0;
    if (!parse_size(size_arg.value, size)) {
        hybrid::user::write_text_line("[truncate] ", "bad size ", size_arg.value);
        hybrid::user::exit(2);
    }
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::Truncate,
                                        reinterpret_cast<uint64_t>(path.value),
                                        hybrid::user::strlen(path.value) + 1,
                                        size);
    if (result.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[truncate] ", "error ", result.error);
        hybrid::user::exit(3);
    }
    hybrid::user::write_text_line("[truncate] ", "path ", path.value);
    hybrid::user::write_hex_line("[truncate] ", "size ", size);
    hybrid::user::exit(0);
}
