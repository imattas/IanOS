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

bool append_text_checked(char* buffer, uint64_t capacity, uint64_t& cursor, const char* text) {
    for (uint64_t i = 0; text[i] != 0; ++i) {
        if (!append_checked(buffer, capacity, cursor, text[i])) return false;
    }
    return true;
}

void pop_component(char* path, uint64_t& cursor) {
    if (cursor <= 1) {
        cursor = 1;
        path[0] = '/';
        path[1] = 0;
        return;
    }
    uint64_t slash = cursor;
    while (slash > 1 && path[slash - 1] != '/') --slash;
    cursor = slash <= 1 ? 1 : slash - 1;
    if (cursor <= 1) {
        cursor = 1;
        path[0] = '/';
    }
    path[cursor] = 0;
}

bool push_component(char* path, uint64_t capacity, uint64_t& cursor, const char* begin, uint64_t length) {
    if (length == 0) return true;
    if (length == 1 && begin[0] == '.') return true;
    if (length == 2 && begin[0] == '.' && begin[1] == '.') {
        pop_component(path, cursor);
        return true;
    }
    if (cursor > 1 && !append_checked(path, capacity, cursor, '/')) return false;
    for (uint64_t i = 0; i < length; ++i) {
        if (!append_checked(path, capacity, cursor, begin[i])) return false;
    }
    return true;
}

bool normalize_absolute(const char* input, char* out, uint64_t capacity) {
    clear(out, capacity);
    if (capacity < 2 || !input || input[0] != '/') return false;
    uint64_t cursor = 0;
    if (!append_checked(out, capacity, cursor, '/')) return false;
    uint64_t i = 1;
    while (input[i] != 0) {
        while (input[i] == '/') ++i;
        const char* start = input + i;
        uint64_t length = 0;
        while (input[i] != 0 && input[i] != '/') {
            ++i;
            ++length;
        }
        if (!push_component(out, capacity, cursor, start, length)) return false;
    }
    if (cursor == 0) append_checked(out, capacity, cursor, '/');
    return true;
}

bool make_absolute(const char* input, char* out, uint64_t capacity) {
    if (!input || input[0] == 0) return false;
    char combined[128];
    clear(combined, sizeof(combined));
    uint64_t cursor = 0;
    if (input[0] == '/') {
        if (!append_text_checked(combined, sizeof(combined), cursor, input)) return false;
    } else {
        hybrid::PathInfo cwd{};
        auto cwd_result = hybrid::user::syscall(hybrid::SyscallNumber::GetCurrentDirectory, reinterpret_cast<uint64_t>(&cwd));
        if (cwd_result.error != hybrid::kSyscallErrorNone) return false;
        if (!append_text_checked(combined, sizeof(combined), cursor, cwd.path)) return false;
        if (cursor > 1 && !append_checked(combined, sizeof(combined), cursor, '/')) return false;
        if (!append_text_checked(combined, sizeof(combined), cursor, input)) return false;
    }
    return normalize_absolute(combined, out, capacity);
}

bool resolve_link_target(const char* path, char* out, uint64_t capacity) {
    char target[128];
    clear(target, sizeof(target));
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::ReadLink,
                                        reinterpret_cast<uint64_t>(path),
                                        hybrid::user::strlen(path) + 1,
                                        reinterpret_cast<uint64_t>(target),
                                        sizeof(target));
    if (result.error != hybrid::kSyscallErrorNone || result.value == 0) return false;
    return make_absolute(target, out, capacity);
}

bool stat_path(const char* path, hybrid::VfsStatInfo& info, hybrid::SyscallResult& result) {
    clear(reinterpret_cast<char*>(&info), sizeof(info));
    result = hybrid::user::syscall(hybrid::SyscallNumber::VfsStatInfo,
                                   reinterpret_cast<uint64_t>(path),
                                   hybrid::user::strlen(path) + 1,
                                   reinterpret_cast<uint64_t>(&info));
    return result.error == hybrid::kSyscallErrorNone;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo path{};
    if (!get_arg(1, path)) {
        hybrid::user::write_line("[realpath] usage realpath <path>");
        hybrid::user::exit(1);
    }

    char resolved[128];
    bool used_link = resolve_link_target(path.value, resolved, sizeof(resolved));
    if (!used_link && !make_absolute(path.value, resolved, sizeof(resolved))) {
        hybrid::user::write_line("[realpath] path too long");
        hybrid::user::exit(2);
    }

    hybrid::VfsStatInfo info{};
    hybrid::SyscallResult stat_result{};
    if (!stat_path(resolved, info, stat_result)) {
        hybrid::user::write_hex_line("[realpath] ", "error ", stat_result.error);
        hybrid::user::exit(3);
    }

    hybrid::user::write_text_line("[realpath] ", "input ", path.value);
    if (used_link) hybrid::user::write_text_line("[realpath] ", "link-target ", resolved);
    hybrid::user::write_text_line("[realpath] ", "path ", info.path);
    hybrid::user::exit(0);
}
