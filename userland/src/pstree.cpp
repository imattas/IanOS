#include "hybrid/user.hpp"

namespace {

constexpr uint64_t kMaxProcesses = 16;

const char* state_name(uint32_t state) {
    switch (state) {
    case 0: return "empty";
    case 1: return "new";
    case 2: return "run";
    case 3: return "stop";
    case 4: return "exit";
    default: return "?";
    }
}

void clear_process(hybrid::ProcessInfo& process) {
    auto* bytes = reinterpret_cast<unsigned char*>(&process);
    for (uint64_t i = 0; i < sizeof(process); ++i) bytes[i] = 0;
}

uint64_t find_by_pid(const hybrid::ProcessInfo* processes, uint64_t count, uint64_t pid) {
    for (uint64_t i = 0; i < count; ++i) {
        if (processes[i].state != 0 && processes[i].pid == pid) return i;
    }
    return count;
}

uint64_t process_depth(const hybrid::ProcessInfo* processes, uint64_t count, uint64_t index) {
    uint64_t depth = 0;
    uint64_t current = index;
    for (uint64_t guard = 0; guard < count; ++guard) {
        uint64_t parent_pid = processes[current].parent_pid;
        if (parent_pid == 0) return depth;
        uint64_t parent = find_by_pid(processes, count, parent_pid);
        if (parent >= count || parent == current) return depth;
        current = parent;
        ++depth;
    }
    return depth;
}

uint64_t child_count(const hybrid::ProcessInfo* processes, uint64_t count, uint64_t pid) {
    uint64_t children = 0;
    for (uint64_t i = 0; i < count; ++i) {
        if (processes[i].state != 0 && processes[i].parent_pid == pid) ++children;
    }
    return children;
}

void write_row(const hybrid::ProcessInfo& process, uint64_t depth, uint64_t children) {
    char line[256];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[pstree] proc depth=");
    hybrid::user::append_hex(line, sizeof(line), cursor, depth);
    hybrid::user::append_text(line, sizeof(line), cursor, " pid=");
    hybrid::user::append_hex(line, sizeof(line), cursor, process.pid);
    hybrid::user::append_text(line, sizeof(line), cursor, " ppid=");
    hybrid::user::append_hex(line, sizeof(line), cursor, process.parent_pid);
    hybrid::user::append_text(line, sizeof(line), cursor, " children=");
    hybrid::user::append_hex(line, sizeof(line), cursor, children);
    hybrid::user::append_text(line, sizeof(line), cursor, " state=");
    hybrid::user::append_text(line, sizeof(line), cursor, state_name(process.state));
    hybrid::user::append_text(line, sizeof(line), cursor, " name=");
    hybrid::user::append_text(line, sizeof(line), cursor, process.name);
    hybrid::user::write_line(line);
}

int main_result() {
    auto count_result = hybrid::user::syscall(hybrid::SyscallNumber::GetProcessCount);
    if (count_result.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[pstree] ", "count error ", count_result.error);
        return 1;
    }

    hybrid::ProcessInfo processes[kMaxProcesses];
    for (uint64_t i = 0; i < kMaxProcesses; ++i) clear_process(processes[i]);

    uint64_t requested = count_result.value < kMaxProcesses ? count_result.value : kMaxProcesses;
    uint64_t loaded = 0;
    for (uint64_t i = 0; i < requested; ++i) {
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetProcessInfo, i, reinterpret_cast<uint64_t>(&processes[loaded]));
        if (result.error == hybrid::kSyscallErrorNone && processes[loaded].state != 0) ++loaded;
    }

    hybrid::user::write_hex_line("[pstree] ", "processes ", count_result.value);
    hybrid::user::write_hex_line("[pstree] ", "loaded ", loaded);
    for (uint64_t pass = 0; pass < kMaxProcesses; ++pass) {
        bool wrote = false;
        for (uint64_t i = 0; i < loaded; ++i) {
            uint64_t depth = process_depth(processes, loaded, i);
            if (depth != pass) continue;
            write_row(processes[i], depth, child_count(processes, loaded, processes[i].pid));
            wrote = true;
        }
        if (!wrote && pass > loaded) break;
    }
    return 0;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::user::exit(static_cast<uint64_t>(main_result()));
}
