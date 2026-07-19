#include "hybrid/user.hpp"

namespace {

constexpr uint64_t kMaxDepth = 3;
constexpr uint64_t kMaxEntries = 192;

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

void clear(void* ptr, uint64_t size) {
    auto* bytes = reinterpret_cast<unsigned char*>(ptr);
    for (uint64_t i = 0; i < size; ++i) bytes[i] = 0;
}

const char* type_name(hybrid::VfsNodeType type) {
    switch (type) {
    case hybrid::VfsNodeType::Directory: return "dir";
    case hybrid::VfsNodeType::MemoryFile: return "file";
    case hybrid::VfsNodeType::CharacterDevice: return "char";
    case hybrid::VfsNodeType::VirtualFile: return "virt";
    default: return "node";
    }
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

bool read_entry(const char* path, uint64_t index, hybrid::VfsDirectoryEntryInfo& out) {
    clear(&out, sizeof(out));
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::ReadDirectory,
                                        reinterpret_cast<uint64_t>(path),
                                        hybrid::user::strlen(path) + 1,
                                        index,
                                        reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone && result.value == 1;
}

void write_entry(const char* path, hybrid::VfsNodeType type, uint64_t depth) {
    char line[160];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[tree] ");
    hybrid::user::append_hex(line, sizeof(line), cursor, depth);
    hybrid::user::append_char(line, sizeof(line), cursor, ' ');
    hybrid::user::append_text(line, sizeof(line), cursor, type_name(type));
    hybrid::user::append_char(line, sizeof(line), cursor, ' ');
    hybrid::user::append_text(line, sizeof(line), cursor, path);
    hybrid::user::write_line(line);
}

void walk(const char* path, hybrid::VfsNodeType type, uint64_t depth, uint64_t& visited, bool& truncated) {
    if (visited >= kMaxEntries) {
        truncated = true;
        return;
    }
    write_entry(path, type, depth);
    ++visited;
    if (type != hybrid::VfsNodeType::Directory || depth >= kMaxDepth) return;

    for (uint64_t i = 0; i < kMaxEntries; ++i) {
        hybrid::VfsDirectoryEntryInfo entry;
        if (!read_entry(path, i, entry)) break;
        walk(entry.path, entry.type, depth + 1, visited, truncated);
        if (truncated) return;
    }
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo arg;
    const char* root = "/";
    if (get_arg(1, arg)) root = arg.value;

    hybrid::VfsStatInfo stat;
    uint64_t error = hybrid::kSyscallErrorNone;
    if (!stat_path(root, stat, error)) {
        hybrid::user::write_text_line("[tree] ", "missing ", root);
        hybrid::user::write_hex_line("[tree] ", "error ", error);
        hybrid::user::exit(1);
    }

    hybrid::user::write_text_line("[tree] ", "root ", stat.path);
    uint64_t visited = 0;
    bool truncated = false;
    walk(stat.path, stat.type, 0, visited, truncated);
    hybrid::user::write_hex_line("[tree] ", "entries ", visited);
    hybrid::user::write_hex_line("[tree] ", "truncated ", truncated ? 1 : 0);
    hybrid::user::exit(truncated ? 2 : 0);
}
