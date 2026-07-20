#include "hybrid/user.hpp"

namespace {

char g_chunk[96];

bool read_proc_uptime(uint64_t& total) {
    total = 0;
    const char* path = "/proc/uptime";
    auto opened = hybrid::user::syscall(hybrid::SyscallNumber::VfsOpen, reinterpret_cast<uint64_t>(path), hybrid::user::strlen(path) + 1);
    if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
        hybrid::user::write_hex_line("[uptime] ", "open error ", opened.error);
        return false;
    }

    for (;;) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::VfsReadHandle, opened.value, reinterpret_cast<uint64_t>(g_chunk), sizeof(g_chunk));
        if (read.error != hybrid::kSyscallErrorNone) {
            if (read.error == hybrid::kSyscallErrorNotFound && total != 0) break;
            hybrid::user::write_hex_line("[uptime] ", "read error ", read.error);
            hybrid::user::syscall(hybrid::SyscallNumber::VfsClose, opened.value);
            return false;
        }
        if (read.value == 0) break;
        total += read.value;
    }

    hybrid::user::syscall(hybrid::SyscallNumber::VfsClose, opened.value);
    return total != 0;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    auto ticks = hybrid::user::syscall(hybrid::SyscallNumber::GetTicks);
    if (ticks.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[uptime] ", "ticks error ", ticks.error);
        hybrid::user::exit(1);
    }
    hybrid::user::write_hex_line("[uptime] ", "ticks ", ticks.value);

    uint64_t bytes = 0;
    if (!read_proc_uptime(bytes)) hybrid::user::exit(2);
    hybrid::user::write_hex_line("[uptime] ", "proc bytes ", bytes);
    hybrid::user::exit(0);
}
