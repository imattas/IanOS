#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

void dirname_part(const char* path, char (&out)[64]) {
    for (uint64_t i = 0; i < sizeof(out); ++i) out[i] = 0;
    uint64_t len = hybrid::user::strlen(path);
    while (len > 1 && path[len - 1] == '/') --len;
    uint64_t slash = len;
    while (slash > 0 && path[slash - 1] != '/') --slash;
    if (slash == 0) {
        out[0] = '.';
        return;
    }
    if (slash == 1) {
        out[0] = '/';
        return;
    }
    uint64_t end = slash - 1;
    while (end > 1 && path[end - 1] == '/') --end;
    for (uint64_t i = 0; i < end && i + 1 < sizeof(out); ++i) out[i] = path[i];
}

}

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo path;
    if (!get_arg(1, path)) {
        hybrid::user::write_line("[dirname] missing path");
        hybrid::user::exit(1);
    }
    char dir[64];
    dirname_part(path.value, dir);
    hybrid::user::write_text("[dirname] ");
    hybrid::user::write_line(dir);
    hybrid::user::exit(0);
}
