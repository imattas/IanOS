#include "hybrid/user.hpp"

namespace {

constexpr uint64_t kMaxLines = 16;
constexpr uint64_t kMaxLineLength = 96;

struct Lines {
    char values[kMaxLines][kMaxLineLength];
    uint64_t lengths[kMaxLines];
    uint64_t count;
    uint64_t bytes;
};

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

void clear_lines(Lines& lines) {
    auto* bytes = reinterpret_cast<unsigned char*>(&lines);
    for (uint64_t i = 0; i < sizeof(lines); ++i) bytes[i] = 0;
}

bool eof_result(const hybrid::SyscallResult& result) {
    return result.value == 0 && (result.error == hybrid::kSyscallErrorNone || result.error == hybrid::kSyscallErrorNotFound);
}

hybrid::SyscallResult read_blocking(uint64_t fd, char* buffer, uint64_t size) {
    for (;;) {
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::Read, fd, reinterpret_cast<uint64_t>(buffer), size);
        if (result.error != hybrid::kSyscallErrorWouldBlock) return result;
        hybrid::user::syscall(hybrid::SyscallNumber::Yield);
    }
}

void add_line(Lines& lines, const char* text, uint64_t length) {
    if (lines.count >= kMaxLines) return;
    uint64_t out_len = 0;
    for (; out_len + 1 < kMaxLineLength && out_len < length; ++out_len) {
        lines.values[lines.count][out_len] = text[out_len];
    }
    lines.values[lines.count][out_len] = 0;
    lines.lengths[lines.count] = out_len;
    ++lines.count;
}

bool read_lines(uint64_t fd, Lines& lines) {
    char read_buffer[32];
    char current[kMaxLineLength];
    uint64_t current_len = 0;
    for (;;) {
        auto read = read_blocking(fd, read_buffer, sizeof(read_buffer));
        if (eof_result(read)) break;
        if (read.error != hybrid::kSyscallErrorNone) {
            hybrid::user::write_hex_line("[tac] ", "read error ", read.error);
            return false;
        }
        if (read.value == 0) break;
        lines.bytes += read.value;
        for (uint64_t i = 0; i < read.value; ++i) {
            const char c = read_buffer[i];
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
    return true;
}

void write_lines_reverse(const Lines& lines) {
    uint64_t index = lines.count;
    while (index != 0) {
        --index;
        hybrid::user::write_text("[tac] ");
        hybrid::user::write_text(lines.values[index]);
        hybrid::user::write_text("\n");
    }
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo path;
    const bool has_path = get_arg(1, path);
    uint64_t fd = hybrid::kStdinFd;
    bool close_when_done = false;
    if (has_path) {
        auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open,
                                            reinterpret_cast<uint64_t>(path.value),
                                            hybrid::user::strlen(path.value) + 1);
        if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
            hybrid::user::write_hex_line("[tac] ", "open error ", opened.error);
            hybrid::user::exit(2);
        }
        fd = opened.value;
        close_when_done = true;
    }

    Lines lines;
    clear_lines(lines);
    const bool ok = read_lines(fd, lines);
    if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);
    if (!ok) hybrid::user::exit(3);

    hybrid::user::write_text_line("[tac] ", "path ", has_path ? path.value : "<stdin>");
    write_lines_reverse(lines);
    hybrid::user::write_hex_line("[tac] ", "lines ", lines.count);
    hybrid::user::write_hex_line("[tac] ", "bytes ", lines.bytes);
    hybrid::user::exit(lines.count);
}
