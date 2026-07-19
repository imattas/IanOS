#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

}

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo existing;
    hybrid::ArgumentInfo linked;
    if (!get_arg(1, existing) || !get_arg(2, linked)) {
        hybrid::user::write_line("[ln] usage: ln <existing> <new>");
        hybrid::user::exit(1);
    }

    auto result = hybrid::user::syscall(hybrid::SyscallNumber::Link,
                                        reinterpret_cast<uint64_t>(existing.value),
                                        hybrid::user::strlen(existing.value) + 1,
                                        reinterpret_cast<uint64_t>(linked.value),
                                        hybrid::user::strlen(linked.value) + 1);
    if (result.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[ln] ", "error ", result.error);
        hybrid::user::exit(2);
    }
    hybrid::user::write_text_line("[ln] ", "from ", existing.value);
    hybrid::user::write_text_line("[ln] ", "to ", linked.value);
    hybrid::user::exit(0);
}
