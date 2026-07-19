#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

bool streq(const char* left, const char* right) {
    uint64_t i = 0;
    while (left[i] != 0 && right[i] != 0) {
        if (left[i] != right[i]) return false;
        ++i;
    }
    return left[i] == 0 && right[i] == 0;
}

void copy_line(char (&out)[64], const char* text, uint64_t length) {
    uint64_t i = 0;
    for (; i + 1 < sizeof(out) && i < length; ++i) out[i] = text[i];
    out[i] = 0;
    for (++i; i < sizeof(out); ++i) out[i] = 0;
}

void emit_unique(const char* line, bool& have_last, char (&last)[64], uint64_t& count) {
    if (have_last && streq(line, last)) return;
    hybrid::user::write_text("[uniq] ");
    hybrid::user::write_line(line);
    copy_line(last, line, hybrid::user::strlen(line));
    have_last = true;
    ++count;
}

}

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo path;
    uint64_t fd = hybrid::kStdinFd;
    bool close_when_done = false;
    bool has_path = get_arg(1, path);
    if (has_path) {
        auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open, reinterpret_cast<uint64_t>(path.value), hybrid::user::strlen(path.value) + 1);
        if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
            hybrid::user::write_hex_line("[uniq] ", "open error ", opened.error);
            hybrid::user::exit(2);
        }
        fd = opened.value;
        close_when_done = true;
    }

    hybrid::user::write_text_line("[uniq] ", "path ", has_path ? path.value : "<stdin>");
    char line[64];
    uint64_t line_len = 0;
    char last[64];
    for (uint64_t i = 0; i < sizeof(last); ++i) last[i] = 0;
    bool have_last = false;
    uint64_t count = 0;
    char buffer[32];
    for (;;) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::Read, fd, reinterpret_cast<uint64_t>(buffer), sizeof(buffer));
        if (read.error != hybrid::kSyscallErrorNone || read.value == 0) break;
        for (uint64_t i = 0; i < read.value; ++i) {
            char c = buffer[i];
            if (c == '\r') continue;
            if (c == '\n') {
                line[line_len] = 0;
                emit_unique(line, have_last, last, count);
                line_len = 0;
            } else if (line_len + 1 < sizeof(line)) {
                line[line_len++] = c;
            }
        }
    }
    if (line_len != 0) {
        line[line_len] = 0;
        emit_unique(line, have_last, last, count);
    }
    if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);
    hybrid::user::exit(count);
}
