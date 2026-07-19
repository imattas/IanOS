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

bool read_line(uint64_t fd, char* line, uint64_t capacity, bool& had_data, uint64_t& error) {
    had_data = false;
    error = hybrid::kSyscallErrorNone;
    uint64_t length = 0;
    for (;;) {
        char c = 0;
        auto read = read_blocking(fd, &c, 1);
        if (eof_result(read)) break;
        if (read.error != hybrid::kSyscallErrorNone) {
            error = read.error;
            return false;
        }
        if (read.value == 0) break;
        had_data = true;
        if (c == '\r') continue;
        if (c == '\n') break;
        if (length + 1 < capacity) {
            line[length++] = c;
            line[length] = 0;
        }
    }
    line[length] = 0;
    return had_data;
}

void emit_pair(const char* left, const char* right) {
    char output[192];
    uint64_t cursor = 0;
    hybrid::user::append_text(output, sizeof(output), cursor, "[paste] ");
    hybrid::user::append_text(output, sizeof(output), cursor, left);
    hybrid::user::append_text(output, sizeof(output), cursor, "\t");
    hybrid::user::append_text(output, sizeof(output), cursor, right);
    hybrid::user::write_line(output);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo left_path{};
    hybrid::ArgumentInfo right_path{};
    if (!get_arg(1, left_path) || !get_arg(2, right_path)) {
        hybrid::user::write_line("[paste] usage paste <left> <right>");
        hybrid::user::exit(1);
    }

    auto left_open = hybrid::user::syscall(hybrid::SyscallNumber::Open,
                                           reinterpret_cast<uint64_t>(left_path.value),
                                           hybrid::user::strlen(left_path.value) + 1);
    if (left_open.error != hybrid::kSyscallErrorNone || left_open.value < 3) {
        hybrid::user::write_hex_line("[paste] ", "left open error ", left_open.error);
        hybrid::user::exit(2);
    }
    auto right_open = hybrid::user::syscall(hybrid::SyscallNumber::Open,
                                            reinterpret_cast<uint64_t>(right_path.value),
                                            hybrid::user::strlen(right_path.value) + 1);
    if (right_open.error != hybrid::kSyscallErrorNone || right_open.value < 3) {
        hybrid::user::syscall(hybrid::SyscallNumber::Close, left_open.value);
        hybrid::user::write_hex_line("[paste] ", "right open error ", right_open.error);
        hybrid::user::exit(2);
    }

    hybrid::user::write_text_line("[paste] ", "left ", left_path.value);
    hybrid::user::write_text_line("[paste] ", "right ", right_path.value);

    char left[96];
    char right[96];
    uint64_t lines = 0;
    for (;;) {
        bool left_data = false;
        bool right_data = false;
        uint64_t left_error = hybrid::kSyscallErrorNone;
        uint64_t right_error = hybrid::kSyscallErrorNone;
        read_line(left_open.value, left, sizeof(left), left_data, left_error);
        read_line(right_open.value, right, sizeof(right), right_data, right_error);
        if (left_error != hybrid::kSyscallErrorNone || right_error != hybrid::kSyscallErrorNone) {
            hybrid::user::syscall(hybrid::SyscallNumber::Close, left_open.value);
            hybrid::user::syscall(hybrid::SyscallNumber::Close, right_open.value);
            hybrid::user::write_hex_line("[paste] ", "read error ", left_error != hybrid::kSyscallErrorNone ? left_error : right_error);
            hybrid::user::exit(3);
        }
        if (!left_data && !right_data) break;
        emit_pair(left_data ? left : "", right_data ? right : "");
        ++lines;
    }

    hybrid::user::syscall(hybrid::SyscallNumber::Close, left_open.value);
    hybrid::user::syscall(hybrid::SyscallNumber::Close, right_open.value);
    hybrid::user::write_hex_line("[paste] ", "lines ", lines);
    hybrid::user::exit(lines);
}
