#include "hybrid/user.hpp"

extern "C" [[noreturn]] void _start() {
    hybrid::user::write_line("[uyield] child start");
    hybrid::user::syscall(hybrid::SyscallNumber::Yield);
    hybrid::user::write_line("[uyield] child resumed");
    hybrid::user::exit(42);
}
