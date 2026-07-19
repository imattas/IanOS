#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

bool text_equals(const char* left, const char* right) {
    if (!left || !right) return false;
    uint64_t i = 0;
    for (; left[i] != 0 && right[i] != 0; ++i) {
        if (left[i] != right[i]) return false;
    }
    return left[i] == 0 && right[i] == 0;
}

void copy_text(char* out, uint64_t capacity, const char* text) {
    if (!out || capacity == 0) return;
    uint64_t i = 0;
    if (text) {
        for (; i + 1 < capacity && text[i] != 0; ++i) out[i] = text[i];
    }
    out[i] = 0;
}

bool has_slash(const char* text) {
    if (!text) return false;
    for (uint64_t i = 0; text[i] != 0; ++i) {
        if (text[i] == '/') return true;
    }
    return false;
}

bool path_exists(const char* path) {
    if (!path || path[0] == 0) return false;
    hybrid::VfsStatInfo info{};
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::VfsStatInfo,
                                        reinterpret_cast<uint64_t>(path),
                                        hybrid::user::strlen(path) + 1,
                                        reinterpret_cast<uint64_t>(&info));
    return result.error == hybrid::kSyscallErrorNone && info.type == hybrid::VfsNodeType::MemoryFile;
}

bool environment_value(const char* key, char* out, uint64_t capacity) {
    if (!key || !out || capacity == 0) return false;
    out[0] = 0;
    auto count = hybrid::user::syscall(hybrid::SyscallNumber::GetEnvironmentCount);
    if (count.error != hybrid::kSyscallErrorNone) return false;
    for (uint64_t i = 0; i < count.value && i < 8; ++i) {
        hybrid::EnvironmentInfo env{};
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetEnvironment, i, reinterpret_cast<uint64_t>(&env));
        if (result.error != hybrid::kSyscallErrorNone || !text_equals(env.key, key)) continue;
        copy_text(out, capacity, env.value);
        return out[0] != 0;
    }
    return false;
}

bool resolve_command(const char* name, char* out, uint64_t capacity) {
    if (!name || name[0] == 0) return false;
    if (has_slash(name)) {
        copy_text(out, capacity, name);
        return path_exists(out);
    }

    char path_env[80];
    if (!environment_value("PATH", path_env, sizeof(path_env))) copy_text(path_env, sizeof(path_env), "/bin");

    uint64_t cursor = 0;
    while (path_env[cursor] != 0) {
        while (path_env[cursor] == ':') ++cursor;
        if (path_env[cursor] == 0) break;

        char candidate[64];
        uint64_t candidate_cursor = 0;
        while (path_env[cursor] != 0 && path_env[cursor] != ':') {
            hybrid::user::append_char(candidate, sizeof(candidate), candidate_cursor, path_env[cursor++]);
        }
        if (candidate_cursor == 0) continue;
        if (candidate[candidate_cursor - 1] != '/') hybrid::user::append_char(candidate, sizeof(candidate), candidate_cursor, '/');
        hybrid::user::append_text(candidate, sizeof(candidate), candidate_cursor, name);
        hybrid::user::append_text(candidate, sizeof(candidate), candidate_cursor, ".elf");
        if (path_exists(candidate)) {
            copy_text(out, capacity, candidate);
            return true;
        }
    }
    return false;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo arg{};
    if (!get_arg(1, arg)) {
        hybrid::user::write_line("[which] usage which <command>");
        hybrid::user::exit(1);
    }

    char resolved[64];
    if (!resolve_command(arg.value, resolved, sizeof(resolved))) {
        hybrid::user::write_text_line("[which] ", "missing ", arg.value);
        hybrid::user::exit(1);
    }

    hybrid::user::write_text_line("[which] ", "path ", resolved);
    hybrid::user::exit(0);
}
