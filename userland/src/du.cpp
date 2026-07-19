#include "hybrid/user.hpp"

namespace {

void clear_bytes(void* value, uint64_t size) {
    auto* bytes = reinterpret_cast<unsigned char*>(value);
    for (uint64_t i = 0; i < size; ++i) bytes[i] = 0;
}

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    clear_bytes(&out, sizeof(out));
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

bool stat_path(const char* path, hybrid::VfsStatInfo& out) {
    clear_bytes(&out, sizeof(out));
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::VfsStatInfo,
                                        reinterpret_cast<uint64_t>(path),
                                        hybrid::user::strlen(path) + 1,
                                        reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

bool path_starts_with(const char* path, const char* prefix) {
    if (!path || !prefix || path[0] != '/' || prefix[0] != '/') return false;
    uint64_t i = 0;
    while (prefix[i] != 0) {
        if (path[i] != prefix[i]) return false;
        ++i;
    }
    if (prefix[i - 1] == '/') return true;
    return path[i] == 0 || path[i] == '/';
}

void write_summary(const hybrid::VfsStatInfo& root, uint64_t files, uint64_t dirs, uint64_t bytes, uint64_t disk_bytes, uint64_t memory_bytes) {
    hybrid::user::write_text_line("[du] ", "path ", root.path);
    hybrid::user::write_hex_line("[du] ", "files ", files);
    hybrid::user::write_hex_line("[du] ", "dirs ", dirs);
    hybrid::user::write_hex_line("[du] ", "bytes ", bytes);
    hybrid::user::write_hex_line("[du] ", "disk bytes ", disk_bytes);
    hybrid::user::write_hex_line("[du] ", "memory bytes ", memory_bytes);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo arg;
    const char* requested = "/";
    if (get_arg(1, arg)) requested = arg.value;

    hybrid::VfsStatInfo root;
    if (!stat_path(requested, root)) {
        hybrid::user::write_text_line("[du] ", "missing ", requested);
        hybrid::user::exit(1);
    }

    auto count = hybrid::user::syscall(hybrid::SyscallNumber::GetVfsNodeCount);
    if (count.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[du] ", "count error ", count.error);
        hybrid::user::exit(2);
    }

    uint64_t files = 0;
    uint64_t dirs = 0;
    uint64_t bytes = 0;
    uint64_t disk_bytes = 0;
    uint64_t memory_bytes = 0;
    for (uint64_t i = 0; i < count.value && i < 256; ++i) {
        hybrid::VfsNodeInfo node;
        clear_bytes(&node, sizeof(node));
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetVfsNodeInfo, i, reinterpret_cast<uint64_t>(&node));
        if (result.error != hybrid::kSyscallErrorNone || !path_starts_with(node.path, root.path)) continue;
        if (node.type == hybrid::VfsNodeType::Directory) {
            ++dirs;
            continue;
        }
        if (node.type != hybrid::VfsNodeType::MemoryFile) continue;
        ++files;
        bytes += node.size;
        if ((node.flags & hybrid::VfsNodeDiskBacked) != 0) disk_bytes += node.size;
        else memory_bytes += node.size;
    }

    write_summary(root, files, dirs, bytes, disk_bytes, memory_bytes);
    hybrid::user::exit(files + dirs);
}
