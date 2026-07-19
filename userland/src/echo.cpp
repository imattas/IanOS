#include "hybrid/user.hpp"

extern "C" [[noreturn]] void _start() {
    auto count = hybrid::user::syscall(hybrid::SyscallNumber::GetArgumentCount);
    if (count.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[echo] ", "error ", count.error);
        hybrid::user::exit(1);
    }

    bool wrote = false;
    for (uint64_t i = 1; i < count.value && i < 8; ++i) {
        hybrid::ArgumentInfo argument;
        auto* bytes = reinterpret_cast<unsigned char*>(&argument);
        for (uint64_t b = 0; b < sizeof(argument); ++b) bytes[b] = 0;
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, i, reinterpret_cast<uint64_t>(&argument));
        if (result.error != hybrid::kSyscallErrorNone) continue;
        if (wrote) hybrid::user::write_text(" ");
        hybrid::user::write_text(argument.value);
        wrote = true;
    }
    hybrid::user::write_text("\n");
    hybrid::user::exit(0);
}
