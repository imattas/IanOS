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

void write_match(const hybrid::ProcessInfo& process) {
    char line[160];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[pgrep] pid ");
    hybrid::user::append_hex(line, sizeof(line), cursor, process.pid);
    hybrid::user::append_text(line, sizeof(line), cursor, " pgid ");
    hybrid::user::append_hex(line, sizeof(line), cursor, process.process_group_id);
    hybrid::user::append_text(line, sizeof(line), cursor, " name ");
    hybrid::user::append_text(line, sizeof(line), cursor, process.name);
    hybrid::user::write_line(line);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo wanted;
    if (!get_arg(1, wanted)) {
        hybrid::user::write_line("[pgrep] usage pgrep name");
        hybrid::user::exit(1);
    }

    hybrid::CurrentIdsInfo ids;
    auto* id_bytes = reinterpret_cast<unsigned char*>(&ids);
    for (uint64_t i = 0; i < sizeof(ids); ++i) id_bytes[i] = 0;
    hybrid::user::syscall(hybrid::SyscallNumber::GetCurrentIds, reinterpret_cast<uint64_t>(&ids));

    auto count = hybrid::user::syscall(hybrid::SyscallNumber::GetProcessCount);
    if (count.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[pgrep] ", "count error ", count.error);
        hybrid::user::exit(2);
    }

    uint64_t matches = 0;
    for (uint64_t i = 0; i < count.value; ++i) {
        hybrid::ProcessInfo process;
        clear_process(process);
        auto info = hybrid::user::syscall(hybrid::SyscallNumber::GetProcessInfo, i, reinterpret_cast<uint64_t>(&process));
        if (info.error != hybrid::kSyscallErrorNone) continue;
        if (process.pid == 0 || process.pid == ids.pid || process.state == 4) continue;
        if (!process_name_matches(process.name, wanted.value)) continue;
        write_match(process);
        ++matches;
    }

    hybrid::user::write_hex_line("[pgrep] ", "matches ", matches);
    hybrid::user::exit(matches == 0 ? 1 : 0);
}
