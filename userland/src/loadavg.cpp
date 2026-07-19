#include "hybrid/user.hpp"

namespace {

char g_chunk[64];

bool read_loadavg(uint64_t& total) {
    total = 0;
    const char* path = "/proc/loadavg";
    auto opened = hybrid::user::syscall(hybrid::SyscallNumber::VfsOpen, reinterpret_cast<uint64_t>(path), hybrid::user::strlen(path) + 1);
    if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
        hybrid::user::write_hex_line("[loadavg] ", "open error ", opened.error);
        return false;
    }
    hybrid::user::write_text("[loadavg] ");
    for (;;) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::VfsReadHandle, opened.value, reinterpret_cast<uint64_t>(g_chunk), sizeof(g_chunk));
        if (read.error != hybrid::kSyscallErrorNone) {
            if (read.error == hybrid::kSyscallErrorNotFound && total != 0) break;
            hybrid::user::write_hex_line("[loadavg] ", "read error ", read.error);
            hybrid::user::syscall(hybrid::SyscallNumber::VfsClose, opened.value);
            return false;
        }
        if (read.value == 0) break;
        total += read.value;
        hybrid::user::syscall(hybrid::SyscallNumber::Write, hybrid::kStdoutFd, reinterpret_cast<uint64_t>(g_chunk), read.value);
    }
    hybrid::user::syscall(hybrid::SyscallNumber::VfsClose, opened.value);
    return total != 0;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    uint64_t bytes = 0;
    if (!read_loadavg(bytes)) hybrid::user::exit(1);
    hybrid::user::write_hex_line("[loadavg] ", "bytes ", bytes);
    hybrid::user::exit(0);
}
