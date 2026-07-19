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
    uint64_t index = 0;
    while (left[index] != 0 && right[index] != 0) {
        if (left[index] != right[index]) return false;
        ++index;
    }
    return left[index] == right[index];
}

bool parse_u64(const char* text, uint64_t& out) {
    if (!text || text[0] == 0) return false;
    uint64_t value = 0;
    for (uint64_t i = 0; text[i] != 0; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + static_cast<uint64_t>(text[i] - '0');
    }
    if (value == 0 || value > 120) return false;
    out = value;
    return true;
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

bool parse_options(uint64_t& width, hybrid::ArgumentInfo& path, bool& has_path) {
    width = 80;
    has_path = false;
    for (uint64_t index = 1;;) {
        hybrid::ArgumentInfo arg{};
        if (!get_arg(index, arg)) break;
        if (streq(arg.value, "-w")) {
            hybrid::ArgumentInfo value{};
            if (!get_arg(index + 1, value) || !parse_u64(value.value, width)) return false;
            index += 2;
            continue;
        }
        path = arg;
        has_path = true;
        ++index;
    }
    return true;
}

void emit_fragment(const char* line, uint64_t offset, uint64_t length, uint64_t& emitted) {
    hybrid::user::write_text("[fold] ");
    if (length != 0) {
        hybrid::user::syscall(hybrid::SyscallNumber::Write, hybrid::kStdoutFd, reinterpret_cast<uint64_t>(line + offset), length);
    }
    hybrid::user::write_text("\n");
    ++emitted;
}

void fold_line(const char* line, uint64_t length, uint64_t width, uint64_t& emitted) {
    if (length == 0) {
        emit_fragment(line, 0, 0, emitted);
        return;
    }
    uint64_t offset = 0;
    while (offset < length) {
        uint64_t count = length - offset;
        if (count > width) count = width;
        emit_fragment(line, offset, count, emitted);
        offset += count;
    }
}

} // namespace

extern "C" [[noreturn]] void _start() {
    uint64_t width = 80;
    hybrid::ArgumentInfo path{};
    bool has_path = false;
    if (!parse_options(width, path, has_path)) {
        hybrid::user::write_line("[fold] usage fold [-w width] [path]");
        hybrid::user::exit(1);
    }

    uint64_t fd = hybrid::kStdinFd;
    bool close_when_done = false;
    if (has_path) {
        auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open,
                                            reinterpret_cast<uint64_t>(path.value),
                                            hybrid::user::strlen(path.value) + 1);
        if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
            hybrid::user::write_hex_line("[fold] ", "open error ", opened.error);
            hybrid::user::exit(2);
        }
        fd = opened.value;
        close_when_done = true;
    }

    hybrid::user::write_text_line("[fold] ", "path ", has_path ? path.value : "<stdin>");
    hybrid::user::write_hex_line("[fold] ", "width ", width);

    char read_buffer[32];
    char line[192];
    uint64_t line_length = 0;
    uint64_t emitted = 0;
    uint64_t bytes = 0;

    for (;;) {
        auto read = read_blocking(fd, read_buffer, sizeof(read_buffer));
        if (eof_result(read)) break;
        if (read.error != hybrid::kSyscallErrorNone) {
            if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);
            hybrid::user::write_hex_line("[fold] ", "read error ", read.error);
            hybrid::user::exit(3);
        }
        if (read.value == 0) break;
        bytes += read.value;
        for (uint64_t i = 0; i < read.value; ++i) {
            const char c = read_buffer[i];
            if (c == '\r') continue;
            if (c == '\n') {
                fold_line(line, line_length, width, emitted);
                line_length = 0;
            } else if (line_length + 1 < sizeof(line)) {
                line[line_length++] = c;
            }
        }
    }
    if (line_length != 0) fold_line(line, line_length, width, emitted);
    if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);

    hybrid::user::write_hex_line("[fold] ", "lines ", emitted);
    hybrid::user::write_hex_line("[fold] ", "bytes ", bytes);
    hybrid::user::exit(emitted);
}
