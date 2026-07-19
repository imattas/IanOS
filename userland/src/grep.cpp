#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

bool contains(const char* line, uint64_t length, const char* pattern) {
    uint64_t pattern_len = hybrid::user::strlen(pattern);
    if (pattern_len == 0 || pattern_len > length) return false;
    for (uint64_t i = 0; i + pattern_len <= length; ++i) {
        bool match = true;
        for (uint64_t j = 0; j < pattern_len; ++j) {
            if (line[i + j] != pattern[j]) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

void emit_line(const char* line, uint64_t length) {
    hybrid::user::write_text("[grep] ");
    hybrid::user::syscall(hybrid::SyscallNumber::Write, hybrid::kStdoutFd, reinterpret_cast<uint64_t>(line), length);
    hybrid::user::write_text("\n");
}

void process_line(const char* line, uint64_t length, const char* pattern, uint64_t& matches) {
    if (contains(line, length, pattern)) {
        emit_line(line, length);
        ++matches;
    }
}

hybrid::SyscallResult read_blocking(uint64_t fd, char* buffer, uint64_t size) {
    bool reported_wait = false;
    for (;;) {
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::Read, fd, reinterpret_cast<uint64_t>(buffer), size);
        if (result.error != hybrid::kSyscallErrorWouldBlock) return result;
        if (!reported_wait) {
            hybrid::user::write_line("[grep] pipe read wouldblock");
            reported_wait = true;
        }
        hybrid::user::syscall(hybrid::SyscallNumber::Yield);
    }
}

}

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo pattern;
    hybrid::ArgumentInfo path;
    if (!get_arg(1, pattern)) {
        hybrid::user::write_line("[grep] usage: grep <pattern> [path]");
        hybrid::user::exit(1);
    }

    uint64_t fd = hybrid::kStdinFd;
    bool close_when_done = false;
    bool has_path = get_arg(2, path);
    if (has_path) {
        auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open, reinterpret_cast<uint64_t>(path.value), hybrid::user::strlen(path.value) + 1);
        if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
            hybrid::user::write_hex_line("[grep] ", "open error ", opened.error);
            hybrid::user::exit(2);
        }
        fd = opened.value;
        close_when_done = true;
    }

    char line[96];
    uint64_t line_len = 0;
    uint64_t matches = 0;
    char buffer[32];
    for (;;) {
        auto read = read_blocking(fd, buffer, sizeof(buffer));
        if (read.error != hybrid::kSyscallErrorNone || read.value == 0) break;
        for (uint64_t i = 0; i < read.value; ++i) {
            char c = buffer[i];
            if (c == '\r') continue;
            if (c == '\n') {
                process_line(line, line_len, pattern.value, matches);
                line_len = 0;
            } else if (line_len + 1 < sizeof(line)) {
                line[line_len++] = c;
            }
        }
    }
    if (line_len != 0) process_line(line, line_len, pattern.value, matches);
    if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);
    hybrid::user::write_text_line("[grep] ", "path ", has_path ? path.value : "<stdin>");
    hybrid::user::write_text_line("[grep] ", "pattern ", pattern.value);
    hybrid::user::write_hex_line("[grep] ", "matches ", matches);
    hybrid::user::exit(matches == 0 ? 1 : 0);
}
