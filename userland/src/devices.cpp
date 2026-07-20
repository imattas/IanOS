#include "hybrid/user.hpp"

namespace {

char g_chunk[128];

bool stream_devices(uint64_t& bytes) {
    bytes = 0;
    static const char path[] = "/proc/devices";
    auto opened = hybrid::user::syscall(hybrid::SyscallNumber::VfsOpen,
                                        reinterpret_cast<uint64_t>(path),
                                        sizeof(path));
    if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
        hybrid::user::write_hex_line("[devices] ", "open error ", opened.error);
        return false;
    }

    hybrid::user::write_text("[devices] ");
    for (;;) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::VfsReadHandle,
                                          opened.value,
                                          reinterpret_cast<uint64_t>(g_chunk),
                                          sizeof(g_chunk));
        if (read.error != hybrid::kSyscallErrorNone) {
            if (read.error == hybrid::kSyscallErrorNotFound && bytes != 0) break;
            hybrid::user::write_hex_line("[devices] ", "read error ", read.error);
            hybrid::user::syscall(hybrid::SyscallNumber::VfsClose, opened.value);
            return false;
        }
        if (read.value == 0) break;
        bytes += read.value;
        hybrid::user::syscall(hybrid::SyscallNumber::Write,
                              hybrid::kStdoutFd,
                              reinterpret_cast<uint64_t>(g_chunk),
                              read.value);
    }

    hybrid::user::syscall(hybrid::SyscallNumber::VfsClose, opened.value);
    return bytes != 0;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    uint64_t bytes = 0;
    if (!stream_devices(bytes)) hybrid::user::exit(1);
    hybrid::user::write_hex_line("[devices] ", "bytes ", bytes);
    hybrid::user::exit(0);
}
