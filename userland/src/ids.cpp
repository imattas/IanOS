#include "hybrid/user.hpp"

extern "C" [[noreturn]] void _start() {
    hybrid::CurrentIdsInfo ids;
    auto* bytes = reinterpret_cast<unsigned char*>(&ids);
    for (uint64_t i = 0; i < sizeof(ids); ++i) bytes[i] = 0;

    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetCurrentIds, reinterpret_cast<uint64_t>(&ids));
    if (result.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[ids] ", "error ", result.error);
        hybrid::user::exit(1);
    }

    hybrid::user::write_hex_line("[ids] ", "pid ", ids.pid);
    hybrid::user::write_hex_line("[ids] ", "tid ", ids.tid);
    hybrid::user::write_hex_line("[ids] ", "ppid ", ids.parent_pid);
    hybrid::user::write_hex_line("[ids] ", "pgid ", ids.process_group_id);
    hybrid::user::write_hex_line("[ids] ", "kthread ", ids.kernel_thread_id);
    hybrid::user::write_hex_line("[ids] ", "cpu ", ids.cpu_id);
    hybrid::user::exit((ids.pid == 0 || ids.tid == 0) ? 1 : 0);
}
