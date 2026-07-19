#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

bool streq(const char* left, const char* right) {
    if (!left || !right) return false;
    uint64_t i = 0;
    while (left[i] != 0 && right[i] != 0) {
        if (left[i] != right[i]) return false;
        ++i;
    }
    return left[i] == right[i];
}

bool parse_u64(const char* text, uint64_t& out) {
    if (!text || text[0] == 0) return false;
    uint64_t cursor = 0;
    uint64_t value = 0;
    for (; text[cursor] != 0; ++cursor) {
        if (text[cursor] < '0' || text[cursor] > '9') return false;
        value = value * 10 + static_cast<uint64_t>(text[cursor] - '0');
    }
    out = value;
    return true;
}

bool parse_n_option(const char* text, uint64_t& lines) {
    if (!text || text[0] != '-' || text[1] != 'n' || text[2] == 0) return false;
    return parse_u64(text + 2, lines);
}

}

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo path;
    hybrid::ArgumentInfo first_arg;
    uint64_t limit = 3;
    uint64_t path_index = 1;
    if (get_arg(1, first_arg)) {
        if (streq(first_arg.value, "-n")) {
            hybrid::ArgumentInfo count_arg;
            if (!get_arg(2, count_arg) || !parse_u64(count_arg.value, limit)) {
                hybrid::user::write_line("[head] usage head [-n count] [path]");
                hybrid::user::exit(1);
            }
            path_index = 3;
        } else if (parse_n_option(first_arg.value, limit)) {
            path_index = 2;
        }
    }

    uint64_t fd = hybrid::kStdinFd;
    bool close_when_done = false;
    if (get_arg(path_index, path)) {
        auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open, reinterpret_cast<uint64_t>(path.value), hybrid::user::strlen(path.value) + 1);
        if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
            hybrid::user::write_hex_line("[head] ", "open error ", opened.error);
            hybrid::user::exit(2);
        }
        fd = opened.value;
        close_when_done = true;
    }

    hybrid::user::write_text_line("[head] ", "path ", close_when_done ? path.value : "<stdin>");
    char c = 0;
    uint64_t lines = 0;
    bool emitted = false;
    while (lines < limit) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::Read, fd, reinterpret_cast<uint64_t>(&c), 1);
        if (read.value == 0 && (read.error == hybrid::kSyscallErrorNone || read.error == hybrid::kSyscallErrorNotFound)) break;
        if (read.error != hybrid::kSyscallErrorNone) {
            if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);
            hybrid::user::write_hex_line("[head] ", "read error ", read.error);
            hybrid::user::exit(3);
        }
        hybrid::user::syscall(hybrid::SyscallNumber::Write, hybrid::kStdoutFd, reinterpret_cast<uint64_t>(&c), 1);
        emitted = true;
        if (c == '\n') ++lines;
    }
    if (emitted && c != '\n') hybrid::user::write_text("\n");
    if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);
    hybrid::user::write_hex_line("[head] ", "lines ", lines);
    hybrid::user::exit(lines);
}
