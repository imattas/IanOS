#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

bool streq(const char* a, const char* b) {
    uint64_t i = 0;
    while (a[i] != 0 && b[i] != 0) {
        if (a[i] != b[i]) return false;
        ++i;
    }
    return a[i] == 0 && b[i] == 0;
}

bool stat_path(const char* path, hybrid::VfsStatInfo& info) {
    auto* bytes = reinterpret_cast<unsigned char*>(&info);
    for (uint64_t i = 0; i < sizeof(info); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::VfsStatInfo,
                                        reinterpret_cast<uint64_t>(path),
                                        hybrid::user::strlen(path) + 1,
                                        reinterpret_cast<uint64_t>(&info));
    return result.error == hybrid::kSyscallErrorNone;
}

}

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo op;
    hybrid::ArgumentInfo value;
    hybrid::ArgumentInfo rhs;
    if (!get_arg(1, op)) hybrid::user::exit(1);

    bool ok = false;
    if ((streq(op.value, "-e") || streq(op.value, "-f") || streq(op.value, "-d")) && get_arg(2, value)) {
        hybrid::VfsStatInfo info;
        ok = stat_path(value.value, info);
        if (ok && streq(op.value, "-f")) ok = info.type == hybrid::VfsNodeType::MemoryFile;
        if (ok && streq(op.value, "-d")) ok = info.type == hybrid::VfsNodeType::Directory;
    } else if (get_arg(2, value) && get_arg(3, rhs) && streq(value.value, "=")) {
        ok = streq(op.value, rhs.value);
    } else {
        ok = op.value[0] != 0;
    }

    hybrid::user::exit(ok ? 0 : 1);
}
