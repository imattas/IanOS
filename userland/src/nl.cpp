#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
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

void append_decimal(char* out, uint64_t capacity, uint64_t& cursor, uint64_t value) {
    char digits[24];
    uint64_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && count < sizeof(digits));
    while (count > 0) hybrid::user::append_char(out, capacity, cursor, digits[--count]);
}

void emit_line(uint64_t line_number, const char* line, uint64_t length) {
    char prefix[48];
    uint64_t cursor = 0;
    hybrid::user::append_text(prefix, sizeof(prefix), cursor, "[nl] ");
    append_decimal(prefix, sizeof(prefix), cursor, line_number);
    hybrid::user::append_text(prefix, sizeof(prefix), cursor, "\t");
    hybrid::user::write_text(prefix);
    if (length != 0) {
        hybrid::user::syscall(hybrid::SyscallNumber::Write, hybrid::kStdoutFd, reinterpret_cast<uint64_t>(line), length);
    }
    hybrid::user::write_text("\n");
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo path{};
    uint64_t fd = hybrid::kStdinFd;
    bool close_when_done = false;
    const bool has_path = get_arg(1, path);
    if (has_path) {
        auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open,
                                            reinterpret_cast<uint64_t>(path.value),
                                            hybrid::user::strlen(path.value) + 1);
        if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
            hybrid::user::write_hex_line("[nl] ", "open error ", opened.error);
            hybrid::user::exit(2);
        }
        fd = opened.value;
        close_when_done = true;
    }

    hybrid::user::write_text_line("[nl] ", "path ", has_path ? path.value : "<stdin>");
    char read_buffer[32];
    char line[128];
    uint64_t line_length = 0;
    uint64_t line_number = 1;
    uint64_t emitted = 0;

    for (;;) {
        auto read = read_blocking(fd, read_buffer, sizeof(read_buffer));
        if (eof_result(read)) break;
        if (read.error != hybrid::kSyscallErrorNone) {
            if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);
            hybrid::user::write_hex_line("[nl] ", "read error ", read.error);
            hybrid::user::exit(3);
        }
        if (read.value == 0) break;
        for (uint64_t i = 0; i < read.value; ++i) {
            const char c = read_buffer[i];
            if (c == '\r') continue;
            if (c == '\n') {
                emit_line(line_number++, line, line_length);
                line_length = 0;
                ++emitted;
            } else if (line_length + 1 < sizeof(line)) {
                line[line_length++] = c;
            }
        }
    }
    if (line_length != 0) {
        emit_line(line_number++, line, line_length);
        ++emitted;
    }
    if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);
    hybrid::user::write_hex_line("[nl] ", "lines ", emitted);
    hybrid::user::exit(emitted);
}
