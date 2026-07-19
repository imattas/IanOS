#include "hybrid/user.hpp"

namespace {

const char* process_state_name(uint32_t state) {
    switch (state) {
    case 0: return "empty";
    case 1: return "new";
    case 2: return "run";
    case 3: return "stop";
    case 4: return "exit";
    default: return "?";
    }
}

const char* thread_state_name(uint32_t state) {
    switch (state) {
    case 0: return "empty";
    case 1: return "new";
    case 2: return "ready";
    case 3: return "run";
    case 4: return "block";
    case 5: return "exit";
    default: return "?";
    }
}

void clear_process(hybrid::ProcessInfo& process) {
    auto* bytes = reinterpret_cast<unsigned char*>(&process);
    for (uint64_t i = 0; i < sizeof(process); ++i) bytes[i] = 0;
}

void clear_thread(hybrid::UserThreadInfo& thread) {
    auto* bytes = reinterpret_cast<unsigned char*>(&thread);
    for (uint64_t i = 0; i < sizeof(thread); ++i) bytes[i] = 0;
}

void write_process_row(const hybrid::ProcessInfo& process) {
    char line[320];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[top] proc pid=");
    hybrid::user::append_hex(line, sizeof(line), cursor, process.pid);
    hybrid::user::append_text(line, sizeof(line), cursor, " ppid=");
    hybrid::user::append_hex(line, sizeof(line), cursor, process.parent_pid);
    hybrid::user::append_text(line, sizeof(line), cursor, " state=");
    hybrid::user::append_text(line, sizeof(line), cursor, process_state_name(process.state));
    hybrid::user::append_text(line, sizeof(line), cursor, " ticks=");
    hybrid::user::append_hex(line, sizeof(line), cursor, process.run_ticks);
    hybrid::user::append_text(line, sizeof(line), cursor, " switches=");
    hybrid::user::append_hex(line, sizeof(line), cursor, process.switch_count);
    hybrid::user::append_text(line, sizeof(line), cursor, " preempts=");
    hybrid::user::append_hex(line, sizeof(line), cursor, process.preempt_count);
    hybrid::user::append_text(line, sizeof(line), cursor, " pages=");
    hybrid::user::append_hex(line, sizeof(line), cursor, process.owned_page_count);
    hybrid::user::append_text(line, sizeof(line), cursor, " fds=");
    hybrid::user::append_hex(line, sizeof(line), cursor, process.open_file_count);
    hybrid::user::append_text(line, sizeof(line), cursor, " name=");
    hybrid::user::append_text(line, sizeof(line), cursor, process.name);
    hybrid::user::write_line(line);
}

void write_thread_row(const hybrid::UserThreadInfo& thread) {
    char line[256];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[top] thread tid=");
    hybrid::user::append_hex(line, sizeof(line), cursor, thread.tid);
    hybrid::user::append_text(line, sizeof(line), cursor, " pid=");
    hybrid::user::append_hex(line, sizeof(line), cursor, thread.pid);
    hybrid::user::append_text(line, sizeof(line), cursor, " state=");
    hybrid::user::append_text(line, sizeof(line), cursor, thread_state_name(thread.state));
    hybrid::user::append_text(line, sizeof(line), cursor, " ticks=");
    hybrid::user::append_hex(line, sizeof(line), cursor, thread.run_ticks);
    hybrid::user::append_text(line, sizeof(line), cursor, " switches=");
    hybrid::user::append_hex(line, sizeof(line), cursor, thread.switch_count);
    hybrid::user::append_text(line, sizeof(line), cursor, " preempts=");
    hybrid::user::append_hex(line, sizeof(line), cursor, thread.preempt_count);
    hybrid::user::write_line(line);
}

uint64_t used_percent_x100(const hybrid::MemoryStatsInfo& memory) {
    if (memory.total_pages == 0) return 0;
    return (memory.used_pages * 10000u) / memory.total_pages;
}

void write_percent_line(const char* prefix, const char* label, uint64_t value_x100) {
    char line[128];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, prefix);
    hybrid::user::append_text(line, sizeof(line), cursor, label);
    hybrid::user::append_hex(line, sizeof(line), cursor, value_x100 / 100u);
    hybrid::user::append_text(line, sizeof(line), cursor, ".");
    uint64_t fraction = value_x100 % 100u;
    hybrid::user::append_char(line, sizeof(line), cursor, static_cast<char>('0' + ((fraction / 10u) % 10u)));
    hybrid::user::append_char(line, sizeof(line), cursor, static_cast<char>('0' + (fraction % 10u)));
    hybrid::user::write_line(line);
}

int main_result() {
    hybrid::MemoryStatsInfo memory;
    hybrid::SchedulerStatsInfo scheduler;
    auto memory_result = hybrid::user::syscall(hybrid::SyscallNumber::GetMemoryStats, reinterpret_cast<uint64_t>(&memory));
    auto scheduler_result = hybrid::user::syscall(hybrid::SyscallNumber::GetSchedulerStats, reinterpret_cast<uint64_t>(&scheduler));
    auto ticks = hybrid::user::syscall(hybrid::SyscallNumber::GetTicks);
    auto processes = hybrid::user::syscall(hybrid::SyscallNumber::GetProcessCount);
    auto threads = hybrid::user::syscall(hybrid::SyscallNumber::GetUserThreadCount);
    if (memory_result.error != hybrid::kSyscallErrorNone || scheduler_result.error != hybrid::kSyscallErrorNone ||
        ticks.error != hybrid::kSyscallErrorNone || processes.error != hybrid::kSyscallErrorNone ||
        threads.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_line("[top] snapshot error");
        return 1;
    }

    hybrid::user::write_hex_line("[top] ", "ticks ", ticks.value);
    hybrid::user::write_hex_line("[top] ", "processes ", processes.value);
    hybrid::user::write_hex_line("[top] ", "threads ", scheduler.thread_count);
    hybrid::user::write_hex_line("[top] ", "ready ", scheduler.ready_count);
    hybrid::user::write_hex_line("[top] ", "sleeping ", scheduler.sleeping_count);
    hybrid::user::write_hex_line("[top] ", "switches ", scheduler.switch_count);
    hybrid::user::write_hex_line("[top] ", "online cpus ", scheduler.online_cpu_count);
    hybrid::user::write_hex_line("[top] ", "mem total pages ", memory.total_pages);
    hybrid::user::write_hex_line("[top] ", "mem used pages ", memory.used_pages);
    hybrid::user::write_hex_line("[top] ", "mem free pages ", memory.free_pages);
    write_percent_line("[top] ", "mem used percent ", used_percent_x100(memory));

    uint64_t process_limit = processes.value < 8 ? processes.value : 8;
    for (uint64_t i = 0; i < process_limit; ++i) {
        hybrid::ProcessInfo process;
        clear_process(process);
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetProcessInfo, i, reinterpret_cast<uint64_t>(&process));
        if (result.error == hybrid::kSyscallErrorNone && process.state != 0) write_process_row(process);
    }

    uint64_t thread_limit = threads.value < 8 ? threads.value : 8;
    for (uint64_t i = 0; i < thread_limit; ++i) {
        hybrid::UserThreadInfo thread;
        clear_thread(thread);
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetUserThreadInfo, i, reinterpret_cast<uint64_t>(&thread));
        if (result.error == hybrid::kSyscallErrorNone && thread.state != 0) write_thread_row(thread);
    }
    return 0;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::user::exit(static_cast<uint64_t>(main_result()));
}
