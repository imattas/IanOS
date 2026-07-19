#include "hybrid/user.hpp"

namespace {

bool read_hostname(char* out, uint64_t capacity) {
    if (!out || capacity == 0) return false;
    out[0] = 0;
    constexpr const char* path = "/etc/hostname";
    auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open,
                                        reinterpret_cast<uint64_t>(path),
                                        hybrid::user::strlen(path) + 1);
    if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) return false;
    uint64_t cursor = 0;
    for (;;) {
        char byte = 0;
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::Read,
                                          opened.value,
                                          reinterpret_cast<uint64_t>(&byte),
                                          1);
        if (read.error != hybrid::kSyscallErrorNone || read.value == 0) break;
        if (byte == '\r' || byte == '\n') break;
        if (cursor + 1 < capacity) {
            out[cursor++] = byte;
            out[cursor] = 0;
        }
    }
    hybrid::user::syscall(hybrid::SyscallNumber::Close, opened.value);
    return cursor != 0;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    char name[64];
    if (!read_hostname(name, sizeof(name))) {
        hybrid::user::write_error("[hostname] read failed\n");
        hybrid::user::exit(1);
    }
    hybrid::user::write_line(name);
    hybrid::user::exit(0);
}
