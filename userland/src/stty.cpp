#include "hybrid/user.hpp"

namespace {

bool streq(const char* a, const char* b) {
    if (!a || !b) return false;
    uint64_t i = 0;
    while (a[i] != 0 && b[i] != 0) {
        if (a[i] != b[i]) return false;
        ++i;
    }
    return a[i] == 0 && b[i] == 0;
}

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

const char* mode_name(uint64_t mode) {
    if (mode == static_cast<uint64_t>(hybrid::TerminalInputMode::Canonical)) return "canonical";
    return "raw";
}

bool print_mode() {
    auto mode = hybrid::user::syscall(hybrid::SyscallNumber::TerminalControl,
                                      static_cast<uint64_t>(hybrid::TerminalControlCommand::GetInputMode));
    if (mode.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[stty] ", "mode error ", mode.error);
        return false;
    }
    hybrid::user::write_text_line("[stty] ", "mode ", mode_name(mode.value));
    return true;
}

bool set_mode(hybrid::TerminalInputMode mode) {
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::TerminalControl,
                                        static_cast<uint64_t>(hybrid::TerminalControlCommand::SetInputMode),
                                        static_cast<uint64_t>(mode));
    if (result.error != hybrid::kSyscallErrorNone || result.value != 1) {
        hybrid::user::write_hex_line("[stty] ", "set error ", result.error);
        return false;
    }
    hybrid::user::write_text_line("[stty] ", "set ", mode_name(static_cast<uint64_t>(mode)));
    return print_mode();
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo arg;
    if (!get_arg(1, arg)) {
        hybrid::user::exit(print_mode() ? 0 : 1);
    }
    if (streq(arg.value, "raw")) {
        hybrid::user::exit(set_mode(hybrid::TerminalInputMode::Raw) ? 0 : 2);
    }
    if (streq(arg.value, "canonical") || streq(arg.value, "cooked")) {
        hybrid::user::exit(set_mode(hybrid::TerminalInputMode::Canonical) ? 0 : 3);
    }
    hybrid::user::write_line("[stty] usage stty [raw|canonical]");
    hybrid::user::exit(1);
}
