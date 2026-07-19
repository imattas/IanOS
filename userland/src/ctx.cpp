#include "hybrid/user.hpp"

extern "C" [[noreturn]] void _start() {
    hybrid::CurrentUserContextInfo context;
    auto* bytes = reinterpret_cast<unsigned char*>(&context);
    for (uint64_t i = 0; i < sizeof(context); ++i) bytes[i] = 0;

    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetCurrentUserContext, reinterpret_cast<uint64_t>(&context));
    if (result.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[ctx] ", "error ", result.error);
        hybrid::user::exit(1);
    }

    hybrid::user::write_hex_line("[ctx] ", "pid ", context.pid);
    hybrid::user::write_hex_line("[ctx] ", "tid ", context.tid);
    hybrid::user::write_hex_line("[ctx] ", "pstate ", context.process_state);
    hybrid::user::write_hex_line("[ctx] ", "tstate ", context.thread_state);
    hybrid::user::write_hex_line("[ctx] ", "rip ", context.entry);
    hybrid::user::write_hex_line("[ctx] ", "rsp ", context.user_stack_pointer);
    hybrid::user::write_hex_line("[ctx] ", "cr3 ", context.address_space_root);

    bool valid = context.pid != 0 && context.tid != 0 && context.address_space_root != 0;
    hybrid::user::exit(valid ? 0 : 1);
}
