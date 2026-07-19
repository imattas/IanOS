#include "hybrid/user.hpp"

namespace {

constexpr uint64_t kMaxLines = 16;
constexpr uint64_t kMaxLineLength = 64;

struct Lines {
    char values[kMaxLines][kMaxLineLength];
    uint64_t count;
};

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

int compare_text(const char* left, const char* right) {
    uint64_t i = 0;
    while (left[i] != 0 && right[i] != 0) {
        if (left[i] < right[i]) return -1;
        if (left[i] > right[i]) return 1;
        ++i;
    }
    if (left[i] == 0 && right[i] == 0) return 0;
    return left[i] == 0 ? -1 : 1;
}

void copy_line(char* out, const char* in, uint64_t length) {
    uint64_t i = 0;
    for (; i + 1 < kMaxLineLength && i < length; ++i) out[i] = in[i];
    out[i] = 0;
}

void add_line(Lines& lines, const char* text, uint64_t length) {
    if (lines.count >= kMaxLines) return;
    copy_line(lines.values[lines.count++], text, length);
}

void read_lines(uint64_t fd, Lines& lines) {
    char current[kMaxLineLength];
    uint64_t current_len = 0;
    char buffer[32];
    for (;;) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::Read, fd, reinterpret_cast<uint64_t>(buffer), sizeof(buffer));
        if (read.error != hybrid::kSyscallErrorNone || read.value == 0) break;
        for (uint64_t i = 0; i < read.value; ++i) {
            char c = buffer[i];
            if (c == '\r') continue;
            if (c == '\n') {
                add_line(lines, current, current_len);
                current_len = 0;
            } else if (current_len + 1 < sizeof(current)) {
                current[current_len++] = c;
            }
        }
    }
    if (current_len != 0) add_line(lines, current, current_len);
}

void sort_lines(Lines& lines) {
    for (uint64_t i = 0; i < lines.count; ++i) {
        for (uint64_t j = i + 1; j < lines.count; ++j) {
            if (compare_text(lines.values[j], lines.values[i]) >= 0) continue;
            char tmp[kMaxLineLength];
            copy_line(tmp, lines.values[i], hybrid::user::strlen(lines.values[i]));
            copy_line(lines.values[i], lines.values[j], hybrid::user::strlen(lines.values[j]));
            copy_line(lines.values[j], tmp, hybrid::user::strlen(tmp));
        }
    }
}

void write_lines(const Lines& lines) {
    for (uint64_t i = 0; i < lines.count; ++i) {
        hybrid::user::write_text("[sort] ");
        hybrid::user::write_line(lines.values[i]);
    }
}

void clear_lines(Lines& lines) {
    auto* bytes = reinterpret_cast<unsigned char*>(&lines);
    for (uint64_t i = 0; i < sizeof(lines); ++i) bytes[i] = 0;
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
            hybrid::user::write_hex_line("[sort] ", "open error ", opened.error);
            hybrid::user::exit(2);
        }
        fd = opened.value;
        close_when_done = true;
    }

    Lines lines;
    clear_lines(lines);
    read_lines(fd, lines);
    if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);
    sort_lines(lines);
    hybrid::user::write_text_line("[sort] ", "path ", has_path ? path.value : "<stdin>");
    write_lines(lines);
    hybrid::user::exit(lines.count);
}
