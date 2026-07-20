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
    return result.error == hybrid::kSyscallErrorNone && result.value == 1;
}

bool path_starts_with(const char* path, const char* prefix) {
    if (!path || !prefix || path[0] != '/' || prefix[0] != '/') return false;
    uint64_t i = 0;
    while (prefix[i] != 0) {
        if (path[i] != prefix[i]) return false;
        ++i;
    }
    if (i == 1 && prefix[0] == '/') return true;
    return path[i] == 0 || path[i] == '/';
}

const char* type_text(hybrid::VfsNodeType type) {
    switch (type) {
    case hybrid::VfsNodeType::Directory: return "dir";
    case hybrid::VfsNodeType::CharacterDevice: return "char";
    case hybrid::VfsNodeType::VirtualFile: return "virt";
    case hybrid::VfsNodeType::MemoryFile: return "file";
    default: return "node";
    }
}

void write_entry(const hybrid::VfsNodeInfo& node) {
    char line[160];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[find] ");
    hybrid::user::append_text(line, sizeof(line), cursor, type_text(node.type));
    hybrid::user::append_char(line, sizeof(line), cursor, ' ');
    hybrid::user::append_text(line, sizeof(line), cursor, node.path);
    hybrid::user::write_line(line);
}

void write_count(uint64_t count) {
    hybrid::user::write_hex_line("[find] ", "matches ", count);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo arg{};
    const char* root = "/";
    if (get_arg(1, arg)) root = arg.value;

    hybrid::VfsStatInfo stat{};
    if (!stat_path(root, stat)) {
        hybrid::user::write_text_line("[find] ", "missing ", root);
        hybrid::user::exit(1);
    }

    auto count = hybrid::user::syscall(hybrid::SyscallNumber::GetVfsNodeCount);
    if (count.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[find] ", "count error ", count.error);
        hybrid::user::exit(2);
    }

    uint64_t matches = 0;
    uint64_t printed = 0;
    constexpr uint64_t kOutputLimit = 64;
    for (uint64_t i = 0; i < count.value; ++i) {
        hybrid::VfsNodeInfo node{};
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetVfsNodeInfo, i, reinterpret_cast<uint64_t>(&node));
        if (result.error != hybrid::kSyscallErrorNone || !path_starts_with(node.path, stat.path)) continue;
        if (printed < kOutputLimit) {
            write_entry(node);
            ++printed;
        }
        ++matches;
    }
    if (matches > printed) hybrid::user::write_hex_line("[find] ", "truncated ", matches - printed);
    write_count(matches);
    hybrid::user::exit(matches);
}
