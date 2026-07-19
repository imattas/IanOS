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

const char* basename(const char* path) {
    const char* base = path;
    for (uint64_t i = 0; path[i] != 0; ++i) {
        if (path[i] == '/') base = path + i + 1;
    }
    return base;
}

bool basename_without_elf_matches(const char* name, const char* wanted) {
    const char* base = basename(name);
    uint64_t i = 0;
    while (base[i] != 0 && wanted[i] != 0) {
        if (base[i] != wanted[i]) return false;
        ++i;
    }
    if (wanted[i] != 0) return false;
    return base[i] == 0 || (base[i] == '.' && base[i + 1] == 'e' && base[i + 2] == 'l' && base[i + 3] == 'f' && base[i + 4] == 0);
}

bool process_name_matches(const char* name, const char* wanted) {
    return streq(name, wanted) || streq(basename(name), wanted) || basename_without_elf_matches(name, wanted);
}

void clear_process(hybrid::ProcessInfo& process) {
    auto* bytes = reinterpret_cast<unsigned char*>(&process);
    for (uint64_t i = 0; i < sizeof(process); ++i) bytes[i] = 0;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    uint64_t signal = static_cast<uint64_t>(hybrid::ProcessTerminationReason::SigTerm);
    uint64_t name_index = 1;
    hybrid::ArgumentInfo first;
    if (!get_arg(1, first)) {
        hybrid::user::write_line("[killall] usage killall [TERM|KILL|-9] name");
        hybrid::user::exit(1);
    }

    uint64_t parsed_signal = signal;
    if (parse_signal(first.value, parsed_signal)) {
        signal = parsed_signal;
        name_index = 2;
    }

    hybrid::ArgumentInfo wanted;
    if (!get_arg(name_index, wanted)) {
        hybrid::user::write_line("[killall] missing name");
        hybrid::user::exit(1);
    }

    hybrid::CurrentIdsInfo ids;
    auto* id_bytes = reinterpret_cast<unsigned char*>(&ids);
    for (uint64_t i = 0; i < sizeof(ids); ++i) id_bytes[i] = 0;
    hybrid::user::syscall(hybrid::SyscallNumber::GetCurrentIds, reinterpret_cast<uint64_t>(&ids));

    auto count = hybrid::user::syscall(hybrid::SyscallNumber::GetProcessCount);
    if (count.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[killall] ", "count error ", count.error);
        hybrid::user::exit(2);
    }

    uint64_t killed = 0;
    for (uint64_t i = 0; i < count.value; ++i) {
        hybrid::ProcessInfo process;
        clear_process(process);
        auto info = hybrid::user::syscall(hybrid::SyscallNumber::GetProcessInfo, i, reinterpret_cast<uint64_t>(&process));
        if (info.error != hybrid::kSyscallErrorNone) continue;
        if (process.pid == 0 || process.pid == ids.pid || process.state == 4) continue;
        if (!process_name_matches(process.name, wanted.value)) continue;

        hybrid::user::write_hex_line("[killall] ", "match pid ", process.pid);
        hybrid::user::write_text_line("[killall] ", "name ", process.name);
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::Kill, process.pid, signal);
        if (result.error == hybrid::kSyscallErrorNone && result.value != 0) ++killed;
        else hybrid::user::write_hex_line("[killall] ", "kill error ", result.error);
    }

    hybrid::user::write_text_line("[killall] ", "reason ", signal_name(signal));
    hybrid::user::write_hex_line("[killall] ", "killed ", killed);
    hybrid::user::exit(killed == 0 ? 1 : 0);
}
