#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

bool streq(const char* left, const char* right) {
    uint64_t i = 0;
    while (left[i] != 0 && right[i] != 0) {
        if (left[i] != right[i]) return false;
        ++i;
    }
    return left[i] == 0 && right[i] == 0;
}

uint64_t parse_number(const char* text, bool& ok) {
    ok = false;
    if (!text || text[0] == 0) return 0;
    uint64_t value = 0;
    uint64_t index = 0;
    uint64_t base = 10;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        index = 2;
    }
    for (; text[index] != 0; ++index) {
        char c = text[index];
        uint64_t digit = 0;
        if (c >= '0' && c <= '9') digit = static_cast<uint64_t>(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f') digit = 10 + static_cast<uint64_t>(c - 'a');
        else if (base == 16 && c >= 'A' && c <= 'F') digit = 10 + static_cast<uint64_t>(c - 'A');
        else return 0;
        if (digit >= base) return 0;
        value = value * base + digit;
        ok = true;
    }
    return value;
}

bool parse_signal(const char* text, uint64_t& signal) {
    if (!text || text[0] == 0) return false;
    if (streq(text, "TERM") || streq(text, "SIGTERM") || streq(text, "-TERM") || streq(text, "-SIGTERM") || streq(text, "-15")) {
        signal = static_cast<uint64_t>(hybrid::ProcessTerminationReason::SigTerm);
        return true;
    }
    if (streq(text, "KILL") || streq(text, "SIGKILL") || streq(text, "-KILL") || streq(text, "-SIGKILL") || streq(text, "-9")) {
        signal = static_cast<uint64_t>(hybrid::ProcessTerminationReason::SigKill);
        return true;
    }
    return false;
}

const char* signal_name(uint64_t signal) {
    return signal == static_cast<uint64_t>(hybrid::ProcessTerminationReason::SigKill) ? "sigkill" : "sigterm";
}

} // namespace

extern "C" [[noreturn]] void _start() {
    uint64_t argc = hybrid::user::syscall(hybrid::SyscallNumber::GetArgumentCount).value;
    bool group = false;
    uint64_t signal = static_cast<uint64_t>(hybrid::ProcessTerminationReason::SigTerm);
    uint64_t target_index = 1;

    hybrid::ArgumentInfo arg1;
    if (!get_arg(1, arg1)) {
        hybrid::user::write_line("[kill] usage kill [TERM|KILL|-9] pid");
        hybrid::user::exit(1);
    }

    if (streq(arg1.value, "--pgid")) {
        group = true;
        target_index = 2;
    } else {
        uint64_t parsed_signal = signal;
        if (parse_signal(arg1.value, parsed_signal)) {
            signal = parsed_signal;
            target_index = 2;
        }
    }

    if (group) {
        hybrid::ArgumentInfo maybe_signal;
        if (get_arg(2, maybe_signal)) {
            uint64_t parsed_signal = signal;
            if (parse_signal(maybe_signal.value, parsed_signal)) {
                signal = parsed_signal;
                target_index = 3;
            }
        }
    }

    if (target_index >= argc) {
        hybrid::user::write_line("[kill] missing target");
        hybrid::user::exit(1);
    }

    hybrid::ArgumentInfo target_arg;
    if (!get_arg(target_index, target_arg)) {
        hybrid::user::write_line("[kill] missing target");
        hybrid::user::exit(1);
    }
    bool ok = false;
    uint64_t target = parse_number(target_arg.value, ok);
    if (!ok || target == 0) {
        hybrid::user::write_line("[kill] invalid target");
        hybrid::user::exit(1);
    }

    auto result = group
        ? hybrid::user::syscall(hybrid::SyscallNumber::KillProcessGroup, target, signal)
        : hybrid::user::syscall(hybrid::SyscallNumber::Kill, target, signal);
    if (result.error != hybrid::kSyscallErrorNone || result.value == 0) {
        hybrid::user::write_hex_line("[kill] ", "error ", result.error);
        hybrid::user::exit(2);
    }

    hybrid::user::write_text_line("[kill] ", group ? "target " : "target ", group ? "pgid" : "pid");
    hybrid::user::write_hex_line("[kill] ", group ? "pgid " : "pid ", target);
    hybrid::user::write_text_line("[kill] ", "reason ", signal_name(signal));
    hybrid::user::write_hex_line("[kill] ", "killed ", result.value);
    hybrid::user::exit(0);
}
