#include "hybrid/user.hpp"

namespace {

bool all_zero(const unsigned char* data, uint64_t size) {
    for (uint64_t i = 0; i < size; ++i) {
        if (data[i] != 0) return false;
    }
    return true;
}

}

extern "C" [[noreturn]] void _start() {
    static const char zero_path[] = "/dev/zero";
    static const char null_path[] = "/dev/null";

    auto zero_fd = hybrid::user::syscall(hybrid::SyscallNumber::Open,
                                         reinterpret_cast<uint64_t>(zero_path),
                                         sizeof(zero_path));
    if (zero_fd.error != hybrid::kSyscallErrorNone || zero_fd.value < 3) {
        hybrid::user::write_hex_line("[devio] ", "zero open error ", zero_fd.error);
        hybrid::user::exit(2);
    }

    unsigned char bytes[32];
    for (uint64_t i = 0; i < sizeof(bytes); ++i) bytes[i] = 0xa5;
    auto read = hybrid::user::syscall(hybrid::SyscallNumber::Read,
                                      zero_fd.value,
                                      reinterpret_cast<uint64_t>(bytes),
                                      sizeof(bytes));
    hybrid::user::syscall(hybrid::SyscallNumber::Close, zero_fd.value);
    if (read.error != hybrid::kSyscallErrorNone || read.value != sizeof(bytes) || !all_zero(bytes, sizeof(bytes))) {
        hybrid::user::write_hex_line("[devio] ", "zero read error ", read.error);
        hybrid::user::write_hex_line("[devio] ", "zero read bytes ", read.value);
        hybrid::user::exit(3);
    }
    hybrid::user::write_hex_line("[devio] ", "zero bytes ", read.value);

    auto null_fd = hybrid::user::syscall(hybrid::SyscallNumber::Open,
                                         reinterpret_cast<uint64_t>(null_path),
                                         sizeof(null_path));
    if (null_fd.error != hybrid::kSyscallErrorNone || null_fd.value < 3) {
        hybrid::user::write_hex_line("[devio] ", "null open error ", null_fd.error);
        hybrid::user::exit(4);
    }
    static const char payload[] = "discard this device payload";
    auto wrote = hybrid::user::syscall(hybrid::SyscallNumber::WriteFile,
                                       null_fd.value,
                                       reinterpret_cast<uint64_t>(payload),
                                       sizeof(payload) - 1);
    hybrid::user::syscall(hybrid::SyscallNumber::Close, null_fd.value);
    if (wrote.error != hybrid::kSyscallErrorNone || wrote.value != sizeof(payload) - 1) {
        hybrid::user::write_hex_line("[devio] ", "null write error ", wrote.error);
        hybrid::user::write_hex_line("[devio] ", "null write bytes ", wrote.value);
        hybrid::user::exit(5);
    }
    hybrid::user::write_hex_line("[devio] ", "null bytes ", wrote.value);
    hybrid::user::write_line("[devio] char devices ok");
    hybrid::user::exit(0);
}
