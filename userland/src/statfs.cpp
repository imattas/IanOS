#include "hybrid/user.hpp"

namespace {

void clear(void* ptr, uint64_t size) {
    auto* bytes = reinterpret_cast<unsigned char*>(ptr);
    for (uint64_t i = 0; i < size; ++i) bytes[i] = 0;
}

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    clear(&out, sizeof(out));
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

bool stat_path(const char* path, hybrid::VfsStatInfo& out, uint64_t& error) {
    clear(&out, sizeof(out));
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::VfsStatInfo,
                                        reinterpret_cast<uint64_t>(path),
                                        hybrid::user::strlen(path) + 1,
                                        reinterpret_cast<uint64_t>(&out));
    error = result.error;
    return result.error == hybrid::kSyscallErrorNone && result.value == 1;
}

bool path_has_prefix(const char* path, const char* prefix) {
    if (!path || !prefix || path[0] != '/' || prefix[0] != '/') return false;
    if (prefix[0] == '/' && prefix[1] == 0) return true;
    uint64_t i = 0;
    while (prefix[i] != 0) {
        if (path[i] != prefix[i]) return false;
        ++i;
    }
    return path[i] == 0 || path[i] == '/';
}

const char* flag_text(uint32_t flags) {
    if ((flags & hybrid::MountDiskBacked) != 0 && (flags & hybrid::MountReadOnly) != 0) return "ro,disk";
    if ((flags & hybrid::MountMemoryBacked) != 0 && (flags & hybrid::MountWritable) != 0) return "rw,mem";
    if ((flags & hybrid::MountDiskBacked) != 0) return "disk";
    if ((flags & hybrid::MountMemoryBacked) != 0) return "mem";
    return "none";
}

bool find_mount(const char* path, hybrid::MountInfo& best) {
    clear(&best, sizeof(best));
    auto count = hybrid::user::syscall(hybrid::SyscallNumber::GetMountCount);
    if (count.error != hybrid::kSyscallErrorNone) return false;

    uint64_t best_len = 0;
    bool found = false;
    for (uint64_t i = 0; i < count.value; ++i) {
        hybrid::MountInfo info;
        clear(&info, sizeof(info));
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetMountInfo, i, reinterpret_cast<uint64_t>(&info));
        if (result.error != hybrid::kSyscallErrorNone) continue;
        if (!path_has_prefix(path, info.path)) continue;
        uint64_t len = hybrid::user::strlen(info.path);
        if (!found || len > best_len) {
            best = info;
            best_len = len;
            found = true;
        }
    }
    return found;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo arg;
    const char* requested = "/";
    if (get_arg(1, arg)) requested = arg.value;

    hybrid::VfsStatInfo stat;
    uint64_t error = hybrid::kSyscallErrorNone;
    if (!stat_path(requested, stat, error)) {
        hybrid::user::write_text_line("[statfs] ", "missing ", requested);
        hybrid::user::write_hex_line("[statfs] ", "error ", error);
        hybrid::user::exit(1);
    }

    hybrid::MountInfo mount;
    if (!find_mount(stat.path, mount)) {
        hybrid::user::write_text_line("[statfs] ", "unmounted ", stat.path);
        hybrid::user::exit(2);
    }

    hybrid::user::write_text_line("[statfs] ", "path ", stat.path);
    hybrid::user::write_text_line("[statfs] ", "mount ", mount.path);
    hybrid::user::write_text_line("[statfs] ", "type ", mount.fs_type);
    hybrid::user::write_text_line("[statfs] ", "source ", mount.source);
    hybrid::user::write_text_line("[statfs] ", "flags ", flag_text(mount.flags));
    hybrid::user::write_hex_line("[statfs] ", "nodes ", mount.node_count);
    hybrid::user::write_hex_line("[statfs] ", "bytes ", mount.total_bytes);
    hybrid::user::exit(0);
}
