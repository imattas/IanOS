#include "hybrid/user.hpp"

int main_result() {
    hybrid::MemoryStatsInfo memory;
    hybrid::SchedulerStatsInfo scheduler;
    auto memory_result = hybrid::user::syscall(hybrid::SyscallNumber::GetMemoryStats, reinterpret_cast<uint64_t>(&memory));
    auto scheduler_result = hybrid::user::syscall(hybrid::SyscallNumber::GetSchedulerStats, reinterpret_cast<uint64_t>(&scheduler));
    auto ticks = hybrid::user::syscall(hybrid::SyscallNumber::GetTicks);
    if (memory_result.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[vmstat] ", "memory error ", memory_result.error);
        return 1;
    }
    if (scheduler_result.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[vmstat] ", "scheduler error ", scheduler_result.error);
        return 2;
    }
    if (ticks.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[vmstat] ", "ticks error ", ticks.error);
        return 3;
    }

    hybrid::user::write_hex_line("[vmstat] ", "ticks ", ticks.value);
    hybrid::user::write_hex_line("[vmstat] ", "mem total pages ", memory.total_pages);
    hybrid::user::write_hex_line("[vmstat] ", "mem used pages ", memory.used_pages);
    hybrid::user::write_hex_line("[vmstat] ", "mem free pages ", memory.free_pages);
    hybrid::user::write_hex_line("[vmstat] ", "mem usable bytes ", memory.usable_bytes);
    hybrid::user::write_hex_line("[vmstat] ", "sched threads ", scheduler.thread_count);
    hybrid::user::write_hex_line("[vmstat] ", "sched ready ", scheduler.ready_count);
    hybrid::user::write_hex_line("[vmstat] ", "sched sleeping ", scheduler.sleeping_count);
    hybrid::user::write_hex_line("[vmstat] ", "sched switches ", scheduler.switch_count);
    hybrid::user::write_hex_line("[vmstat] ", "sched preempts ", scheduler.preempt_count);
    hybrid::user::write_hex_line("[vmstat] ", "online cpus ", scheduler.online_cpu_count);
    return 0;
}

extern "C" [[noreturn]] void _start() {
    hybrid::user::exit(static_cast<uint64_t>(main_result()));
}
