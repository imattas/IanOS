#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

uint64_t open_path(const char* path) {
    auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open,
                                        reinterpret_cast<uint64_t>(path),
                                        hybrid::user::strlen(path) + 1);
    if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
        hybrid::user::write_hex_line("[cmp] ", "open error ", opened.error);
        hybrid::user::exit(2);
    }
    return opened.value;
}

hybrid::SyscallResult read_next(uint64_t fd, unsigned char* buffer, uint64_t size) {
    for (;;) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::Read, fd, reinterpret_cast<uint64_t>(buffer), size);
        if (read.error != hybrid::kSyscallErrorWouldBlock) return read;
        hybrid::user::syscall(hybrid::SyscallNumber::Yield);
    }
}

void write_files_line(const char* left, const char* right) {
    char line[160];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[cmp] files ");
    hybrid::user::append_text(line, sizeof(line), cursor, left);
    hybrid::user::append_text(line, sizeof(line), cursor, " ");
    hybrid::user::append_text(line, sizeof(line), cursor, right);
    hybrid::user::write_line(line);
}

void write_difference(uint64_t byte_offset, uint8_t left, uint8_t right) {
    char line[128];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[cmp] differ byte ");
    hybrid::user::append_hex(line, sizeof(line), cursor, byte_offset);
    hybrid::user::append_text(line, sizeof(line), cursor, " left ");
    hybrid::user::append_hex(line, sizeof(line), cursor, left);
    hybrid::user::append_text(line, sizeof(line), cursor, " right ");
    hybrid::user::append_hex(line, sizeof(line), cursor, right);
    hybrid::user::write_line(line);
}

void close_pair(uint64_t left_fd, uint64_t right_fd) {
    hybrid::user::syscall(hybrid::SyscallNumber::Close, left_fd);
    hybrid::user::syscall(hybrid::SyscallNumber::Close, right_fd);
}

bool is_eof(const hybrid::SyscallResult& result) {
    return result.value == 0 && (result.error == hybrid::kSyscallErrorNone || result.error == hybrid::kSyscallErrorNotFound);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo left_path{};
    hybrid::ArgumentInfo right_path{};
    if (!get_arg(1, left_path) || !get_arg(2, right_path)) {
        hybrid::user::write_line("[cmp] usage cmp <left> <right>");
        hybrid::user::exit(1);
    }

    const uint64_t left_fd = open_path(left_path.value);
    const uint64_t right_fd = open_path(right_path.value);
    write_files_line(left_path.value, right_path.value);

    unsigned char left[32];
    unsigned char right[32];
    uint64_t compared = 0;
    for (;;) {
        auto left_read = read_next(left_fd, left, sizeof(left));
        auto right_read = read_next(right_fd, right, sizeof(right));
        if (left_read.error != hybrid::kSyscallErrorNone && !is_eof(left_read)) {
            close_pair(left_fd, right_fd);
            hybrid::user::write_hex_line("[cmp] ", "left read error ", left_read.error);
            hybrid::user::exit(3);
        }
        if (right_read.error != hybrid::kSyscallErrorNone && !is_eof(right_read)) {
            close_pair(left_fd, right_fd);
            hybrid::user::write_hex_line("[cmp] ", "right read error ", right_read.error);
            hybrid::user::exit(4);
        }

        const bool left_eof = is_eof(left_read);
        const bool right_eof = is_eof(right_read);
        if (left_eof && right_eof) {
            close_pair(left_fd, right_fd);
            hybrid::user::write_hex_line("[cmp] ", "equal bytes ", compared);
            hybrid::user::exit(0);
        }

        const uint64_t common = left_read.value < right_read.value ? left_read.value : right_read.value;
        for (uint64_t i = 0; i < common; ++i) {
            if (left[i] != right[i]) {
                close_pair(left_fd, right_fd);
                write_difference(compared + i + 1, left[i], right[i]);
                hybrid::user::exit(1);
            }
        }
        compared += common;

        if (left_read.value != right_read.value) {
            close_pair(left_fd, right_fd);
            if (left_read.value < right_read.value) {
                write_difference(compared + 1, 0, right[common]);
            } else {
                write_difference(compared + 1, left[common], 0);
            }
            hybrid::user::exit(1);
        }
    }
}
