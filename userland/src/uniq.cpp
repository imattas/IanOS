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

bool eof_result(const hybrid::SyscallResult& result) {
    return result.value == 0 && (result.error == hybrid::kSyscallErrorNone || result.error == hybrid::kSyscallErrorNotFound);
}

void emit_plain(const char* line, uint64_t& emitted) {
    hybrid::user::write_text("[uniq] ");
    hybrid::user::write_line(line);
    ++emitted;
}

void emit_counted(const char* line, uint64_t repeat_count, uint64_t& emitted) {
    hybrid::user::write_text("[uniq] count ");
    hybrid::user::write_hex_line("", "", repeat_count);
    hybrid::user::write_text("[uniq] ");
    hybrid::user::write_line(line);
    ++emitted;
}

void flush_pending(bool count_mode, bool& have_last, char (&last)[64], uint64_t& repeat_count, uint64_t& emitted) {
    if (!have_last) return;
    if (count_mode) emit_counted(last, repeat_count, emitted);
    else emit_plain(last, emitted);
    have_last = false;
    repeat_count = 0;
}

void handle_line(const char* line, bool count_mode, bool& have_last, char (&last)[64], uint64_t& repeat_count, uint64_t& emitted) {
    if (have_last && streq(line, last)) {
        ++repeat_count;
        return;
    }
    flush_pending(count_mode, have_last, last, repeat_count, emitted);
    copy_line(last, line, hybrid::user::strlen(line));
    have_last = true;
    repeat_count = 1;
}

}

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo path;
    hybrid::ArgumentInfo first;
    bool count_mode = false;
    uint64_t path_index = 1;
    if (get_arg(1, first) && streq(first.value, "-c")) {
        count_mode = true;
        path_index = 2;
    }
    uint64_t fd = hybrid::kStdinFd;
    bool close_when_done = false;
    bool has_path = get_arg(path_index, path);
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
    uint64_t repeat_count = 0;
    uint64_t emitted = 0;
    char buffer[32];
    for (;;) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::Read, fd, reinterpret_cast<uint64_t>(buffer), sizeof(buffer));
        if (eof_result(read)) break;
        if (read.error != hybrid::kSyscallErrorNone) {
            if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);
            hybrid::user::write_hex_line("[uniq] ", "read error ", read.error);
            hybrid::user::exit(3);
        }
        for (uint64_t i = 0; i < read.value; ++i) {
            char c = buffer[i];
            if (c == '\r') continue;
            if (c == '\n') {
                line[line_len] = 0;
                handle_line(line, count_mode, have_last, last, repeat_count, emitted);
                line_len = 0;
            } else if (line_len + 1 < sizeof(line)) {
                line[line_len++] = c;
            }
        }
    }
    if (line_len != 0) {
        line[line_len] = 0;
        handle_line(line, count_mode, have_last, last, repeat_count, emitted);
    }
    flush_pending(count_mode, have_last, last, repeat_count, emitted);
    hybrid::user::write_hex_line("[uniq] ", "lines ", emitted);
    if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);
    hybrid::user::exit(emitted);
}
