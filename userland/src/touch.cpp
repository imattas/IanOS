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
    hybrid::ArgumentInfo path;
    if (!get_arg(1, path)) {
        hybrid::user::write_line("[touch] missing path");
        hybrid::user::exit(1);
    }
    auto created = hybrid::user::syscall(hybrid::SyscallNumber::CreateFile, reinterpret_cast<uint64_t>(path.value), hybrid::user::strlen(path.value) + 1);
    if (created.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[touch] ", "error ", created.error);
        hybrid::user::exit(2);
    }
    hybrid::user::write_text_line("[touch] ", "created ", path.value);
    hybrid::user::exit(0);
}
