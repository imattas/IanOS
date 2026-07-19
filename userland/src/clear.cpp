#include "hybrid/user.hpp"

extern "C" [[noreturn]] void _start() {
    hybrid::user::write_text("\f");
    hybrid::user::syscall(hybrid::SyscallNumber::TerminalControl,
                          static_cast<uint64_t>(hybrid::TerminalControlCommand::ScrollToBottom));
    hybrid::user::exit(0);
}
