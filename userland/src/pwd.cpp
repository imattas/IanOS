#include "hybrid/user.hpp"

extern "C" [[noreturn]] void _start() {
    hybrid::PathInfo cwd;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetCurrentDirectory, reinterpret_cast<uint64_t>(&cwd));
    if (result.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[pwd] ", "error ", result.error);
        hybrid::user::exit(1);
    }
    hybrid::user::write_text_line("[pwd] ", "", cwd.path);
    hybrid::user::exit(0);
}
