#include "hybrid/user.hpp"

extern "C" [[noreturn]] void _start() {
    hybrid::SchedulerStatsInfo info;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetSchedulerStats, reinterpret_cast<uint64_t>(&info));
    if (result.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[schedstat] ", "error ", result.error);
        hybrid::user::exit(1);
    }

    hybrid::user::write_hex_line("[schedstat] ", "threads ", info.thread_count);
    hybrid::user::write_hex_line("[schedstat] ", "ready ", info.ready_count);
    hybrid::user::write_hex_line("[schedstat] ", "sleeping ", info.sleeping_count);
    hybrid::user::write_hex_line("[schedstat] ", "dead ", info.dead_count);
    hybrid::user::write_hex_line("[schedstat] ", "switches ", info.switch_count);
    hybrid::user::write_hex_line("[schedstat] ", "yields ", info.yield_count);
    hybrid::user::write_hex_line("[schedstat] ", "preempts ", info.preempt_count);
    hybrid::user::write_hex_line("[schedstat] ", "current thread ", info.current_thread_id);
    hybrid::user::write_hex_line("[schedstat] ", "current cpu ", info.current_cpu_id);
    hybrid::user::write_hex_line("[schedstat] ", "online cpus ", info.online_cpu_count);
    hybrid::user::exit(0);
}
