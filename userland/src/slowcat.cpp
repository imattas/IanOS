#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

void write_bytes(const char* bytes, uint64_t length) {
    uint64_t written = 0;
    while (written < length) {
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::Write, hybrid::kStdoutFd, reinterpret_cast<uint64_t>(bytes + written), length - written);
        if (result.error == hybrid::kSyscallErrorWouldBlock) {
            hybrid::user::syscall(hybrid::SyscallNumber::Yield);
            continue;
        }
        if (result.error != hybrid::kSyscallErrorNone || result.value == 0) break;
        written += result.value;
    }
}

}

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo path;
    if (!get_arg(1, path)) {
        hybrid::user::write_line("[slowcat] missing path");
        hybrid::user::exit(1);
    }
    char path_line[96];
    uint64_t path_cursor = 0;
    hybrid::user::append_text(path_line, sizeof(path_line), path_cursor, "[slowcat] path ");
    hybrid::user::append_text(path_line, sizeof(path_line), path_cursor, path.value);
    hybrid::user::append_char(path_line, sizeof(path_line), path_cursor, '\n');
    hybrid::user::write_error(path_line);
    for (uint64_t i = 0; i < 4; ++i) hybrid::user::syscall(hybrid::SyscallNumber::Yield);

    auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open, reinterpret_cast<uint64_t>(path.value), hybrid::user::strlen(path.value) + 1);
    if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
        hybrid::user::write_hex_line("[slowcat] ", "open error ", opened.error);
        hybrid::user::exit(2);
    }
    char buffer[32];
    uint64_t total = 0;
    for (;;) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::Read, opened.value, reinterpret_cast<uint64_t>(buffer), sizeof(buffer));
        if (read.error != hybrid::kSyscallErrorNone || read.value == 0) break;
        total += read.value;
        write_bytes(buffer, read.value);
    }
    hybrid::user::syscall(hybrid::SyscallNumber::Close, opened.value);
    hybrid::user::write_hex_line("[slowcat] ", "bytes ", total);
    hybrid::user::exit(total);
}
