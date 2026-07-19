#include "hybrid/user.hpp"

namespace {

const char* state_name(uint32_t state) {
    switch (state) {
    case 0: return "empty";
    case 1: return "created";
    case 2: return "runnable";
    case 3: return "stopped";
    case 4: return "exited";
    default: return "unknown";
    }
}

const char* reason_name(uint32_t reason) {
    switch (reason) {
    case 0: return "none";
    case 1: return "exit";
    case 9: return "sigkill";
    case 15: return "sigterm";
    default: return "unknown";
    }
}

const char* thread_state_name(uint32_t state) {
    switch (state) {
    case 0: return "empty";
    case 1: return "created";
    case 2: return "runnable";
    case 3: return "running";
    case 4: return "blocked";
    case 5: return "exited";
    default: return "unknown";
    }
}

const char* block_reason_name(uint32_t reason) {
    switch (reason) {
    case 0: return "none";
    case 1: return "pipe-read";
    case 2: return "pipe-write";
    case 3: return "process-wait";
    case 4: return "sleep";
    default: return "unknown";
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

void write_process(const hybrid::ProcessInfo& process) {
    char line[320];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[ps] pid=");
    hybrid::user::append_hex(line, sizeof(line), cursor, process.pid);
    hybrid::user::append_text(line, sizeof(line), cursor, " ppid=");
    hybrid::user::append_hex(line, sizeof(line), cursor, process.parent_pid);
    hybrid::user::append_text(line, sizeof(line), cursor, " pgid=");
    hybrid::user::append_hex(line, sizeof(line), cursor, process.process_group_id);
    hybrid::user::append_text(line, sizeof(line), cursor, " state=");
    hybrid::user::append_text(line, sizeof(line), cursor, state_name(process.state));
    hybrid::user::append_text(line, sizeof(line), cursor, " reason=");
    hybrid::user::append_text(line, sizeof(line), cursor, reason_name(process.termination_reason));
    hybrid::user::append_text(line, sizeof(line), cursor, " code=");
    hybrid::user::append_hex(line, sizeof(line), cursor, process.exit_code);
    hybrid::user::append_text(line, sizeof(line), cursor, " fds=");
    hybrid::user::append_hex(line, sizeof(line), cursor, process.open_file_count);
    hybrid::user::append_text(line, sizeof(line), cursor, " syscalls=");
    hybrid::user::append_hex(line, sizeof(line), cursor, process.syscall_count);
    hybrid::user::append_text(line, sizeof(line), cursor, " last=");
    hybrid::user::append_hex(line, sizeof(line), cursor, process.last_syscall);
    hybrid::user::append_text(line, sizeof(line), cursor, " ticks=");
    hybrid::user::append_hex(line, sizeof(line), cursor, process.run_ticks);
    hybrid::user::append_text(line, sizeof(line), cursor, " switches=");
    hybrid::user::append_hex(line, sizeof(line), cursor, process.switch_count);
    hybrid::user::append_text(line, sizeof(line), cursor, " preempts=");
    hybrid::user::append_hex(line, sizeof(line), cursor, process.preempt_count);
    hybrid::user::append_text(line, sizeof(line), cursor, " name=");
    hybrid::user::append_text(line, sizeof(line), cursor, process.name);
    hybrid::user::write_line(line);
}

void write_thread(const hybrid::UserThreadInfo& thread) {
    char line[320];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[ps] tid=");
    hybrid::user::append_hex(line, sizeof(line), cursor, thread.tid);
    hybrid::user::append_text(line, sizeof(line), cursor, " pid=");
    hybrid::user::append_hex(line, sizeof(line), cursor, thread.pid);
    hybrid::user::append_text(line, sizeof(line), cursor, " tstate=");
    hybrid::user::append_text(line, sizeof(line), cursor, thread_state_name(thread.state));
    hybrid::user::append_text(line, sizeof(line), cursor, " block=");
    hybrid::user::append_text(line, sizeof(line), cursor, block_reason_name(thread.block_reason));
    hybrid::user::append_text(line, sizeof(line), cursor, " pipe=");
    hybrid::user::append_hex(line, sizeof(line), cursor, thread.wait_pipe_id);
    hybrid::user::append_text(line, sizeof(line), cursor, " waitpid=");
    hybrid::user::append_hex(line, sizeof(line), cursor, thread.wait_process_id);
    hybrid::user::append_text(line, sizeof(line), cursor, " syscalls=");
    hybrid::user::append_hex(line, sizeof(line), cursor, thread.syscall_count);
    hybrid::user::append_text(line, sizeof(line), cursor, " last=");
    hybrid::user::append_hex(line, sizeof(line), cursor, thread.last_syscall);
    hybrid::user::append_text(line, sizeof(line), cursor, " ticks=");
    hybrid::user::append_hex(line, sizeof(line), cursor, thread.run_ticks);
    hybrid::user::append_text(line, sizeof(line), cursor, " switches=");
    hybrid::user::append_hex(line, sizeof(line), cursor, thread.switch_count);
    hybrid::user::append_text(line, sizeof(line), cursor, " preempts=");
    hybrid::user::append_hex(line, sizeof(line), cursor, thread.preempt_count);
    hybrid::user::write_line(line);
}

}

extern "C" [[noreturn]] void _start() {
    auto count = hybrid::user::syscall(hybrid::SyscallNumber::GetProcessCount);
    if (count.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[ps] ", "error ", count.error);
        hybrid::user::exit(1);
    }
    hybrid::user::write_hex_line("[ps] ", "count ", count.value);
    for (uint64_t i = 0; i < count.value && i < 16; ++i) {
        hybrid::ProcessInfo process;
        clear_process(process);
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetProcessInfo, i, reinterpret_cast<uint64_t>(&process));
        if (result.error == hybrid::kSyscallErrorNone) write_process(process);
    }
    auto thread_count = hybrid::user::syscall(hybrid::SyscallNumber::GetUserThreadCount);
    if (thread_count.error == hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[ps] ", "threads ", thread_count.value);
        for (uint64_t i = 0; i < thread_count.value && i < 32; ++i) {
            hybrid::UserThreadInfo thread;
            clear_thread(thread);
            auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetUserThreadInfo, i, reinterpret_cast<uint64_t>(&thread));
            if (result.error == hybrid::kSyscallErrorNone) write_thread(thread);
        }
    }
    hybrid::user::exit(count.value);
}
