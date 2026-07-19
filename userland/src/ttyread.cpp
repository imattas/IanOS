#include "hybrid/user.hpp"

namespace {

bool set_mode(hybrid::TerminalInputMode mode) {
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::TerminalControl,
                                        static_cast<uint64_t>(hybrid::TerminalControlCommand::SetInputMode),
                                        static_cast<uint64_t>(mode));
    return result.error == hybrid::kSyscallErrorNone && result.value == 1;
}

bool inject(const char* text) {
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::TerminalControl,
                                        static_cast<uint64_t>(hybrid::TerminalControlCommand::InjectInput),
                                        reinterpret_cast<uint64_t>(text),
                                        hybrid::user::strlen(text));
    return result.error == hybrid::kSyscallErrorNone && result.value == hybrid::user::strlen(text);
}

}

extern "C" [[noreturn]] void _start() {
    static const char payload[] = "canX\bonical tty line\n";
    char buffer[32]{};
    bool ok = set_mode(hybrid::TerminalInputMode::Canonical);
    if (ok) {
        hybrid::user::write_line("[ttyread] mode canonical");
        ok = inject(payload);
    }
    if (ok) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::Read,
                                          hybrid::kStdinFd,
                                          reinterpret_cast<uint64_t>(buffer),
                                          sizeof(buffer));
        ok = read.error == hybrid::kSyscallErrorNone && read.value == 19 &&
             buffer[0] == 'c' && buffer[1] == 'a' && buffer[2] == 'n' &&
             buffer[3] == 'o' && buffer[4] == 'n' &&
             buffer[17] == 'e' && buffer[18] == '\n';
        if (ok) {
            hybrid::user::write_text_line("[ttyread] ", "line ", buffer);
            hybrid::user::write_hex_line("[ttyread] ", "bytes ", read.value);
        } else {
            hybrid::user::write_hex_line("[ttyread] ", "read error ", read.error);
            hybrid::user::write_hex_line("[ttyread] ", "read bytes ", read.value);
        }
    }
    if (set_mode(hybrid::TerminalInputMode::Raw)) {
        hybrid::user::write_line("[ttyread] mode raw");
    } else {
        ok = false;
    }
    if (ok) hybrid::user::write_line("[ttyread] canonical stdin ok");
    hybrid::user::exit(ok ? 0 : 1);
}
