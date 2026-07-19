#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

void clear(char* buffer, uint64_t capacity) {
    for (uint64_t i = 0; i < capacity; ++i) buffer[i] = 0;
}

bool append_checked(char* buffer, uint64_t capacity, uint64_t& cursor, char value) {
    if (cursor + 1 >= capacity) return false;
    buffer[cursor++] = value;
    buffer[cursor] = 0;
    return true;
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
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::VfsStatInfo,
                                        reinterpret_cast<uint64_t>(path),
                                        hybrid::user::strlen(path) + 1,
                                        reinterpret_cast<uint64_t>(&out));
    error = result.error;
    return result.error == hybrid::kSyscallErrorNone;
}

void write_component(const char* path, const hybrid::VfsStatInfo& info, uint64_t depth) {
    char line[160];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[namei] ");
    hybrid::user::append_hex(line, sizeof(line), cursor, depth);
    hybrid::user::append_char(line, sizeof(line), cursor, ' ');
    hybrid::user::append_text(line, sizeof(line), cursor, type_name(info.type));
    hybrid::user::append_char(line, sizeof(line), cursor, ' ');
    hybrid::user::append_text(line, sizeof(line), cursor, path);
    hybrid::user::write_line(line);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo path;
    if (!get_arg(1, path)) {
        hybrid::user::write_line("[namei] usage namei <absolute-path>");
        hybrid::user::exit(1);
    }
    if (path.value[0] != '/') {
        hybrid::user::write_line("[namei] absolute path required");
        hybrid::user::exit(1);
    }

    hybrid::user::write_text_line("[namei] ", "path ", path.value);

    char current[128];
    clear(current, sizeof(current));
    uint64_t cursor = 0;
    append_checked(current, sizeof(current), cursor, '/');

    hybrid::VfsStatInfo info;
    uint64_t error = hybrid::kSyscallErrorNone;
    uint64_t components = 0;
    if (!stat_path(current, info, error)) {
        hybrid::user::write_hex_line("[namei] ", "error ", error);
        hybrid::user::exit(2);
    }
    write_component(current, info, components++);

    uint64_t i = 1;
    while (path.value[i] != 0) {
        while (path.value[i] == '/') ++i;
        if (path.value[i] == 0) break;
        if (cursor > 1 && !append_checked(current, sizeof(current), cursor, '/')) {
            hybrid::user::write_line("[namei] path too long");
            hybrid::user::exit(3);
        }
        while (path.value[i] != 0 && path.value[i] != '/') {
            if (!append_checked(current, sizeof(current), cursor, path.value[i])) {
                hybrid::user::write_line("[namei] path too long");
                hybrid::user::exit(3);
            }
            ++i;
        }
        if (!stat_path(current, info, error)) {
            hybrid::user::write_text_line("[namei] ", "missing ", current);
            hybrid::user::write_hex_line("[namei] ", "error ", error);
            hybrid::user::exit(2);
        }
        write_component(current, info, components++);
    }

    hybrid::user::write_hex_line("[namei] ", "components ", components);
    hybrid::user::exit(0);
}
