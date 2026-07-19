#include "hybrid/user.hpp"

extern "C" [[noreturn]] void _start() {
    hybrid::user::write_line("[loop] start");
    for (;;) {
        hybrid::user::syscall(hybrid::SyscallNumber::Yield);
    }
}
