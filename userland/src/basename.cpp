#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

const char* basename_part(const char* path) {
    uint64_t len = hybrid::user::strlen(path);
    while (len > 1 && path[len - 1] == '/') --len;
    uint64_t start = len;
    while (start > 0 && path[start - 1] != '/') --start;
    return path + start;
}

}

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo path;
    if (!get_arg(1, path)) {
        hybrid::user::write_line("[basename] missing path");
        hybrid::user::exit(1);
    }
    hybrid::user::write_text("[basename] ");
    hybrid::user::write_line(basename_part(path.value));
    hybrid::user::exit(0);
}
