#include "hybrid/user.hpp"

namespace {

constexpr uint64_t kCommandLineCapacity = 128;

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

void copy_text(char* out, uint64_t capacity, const char* text) {
    if (!out || capacity == 0) return;
    uint64_t i = 0;
    if (text) {
        for (; i + 1 < capacity && text[i] != 0; ++i) out[i] = text[i];
    }
    out[i] = 0;
}

bool stat_path(const char* path) {
    hybrid::VfsStatInfo info{};
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::VfsStatInfo,
                                        reinterpret_cast<uint64_t>(path),
                                        hybrid::user::strlen(path) + 1,
                                        reinterpret_cast<uint64_t>(&info));
    return result.error == hybrid::kSyscallErrorNone;
}

bool resolve_command(const char* name, char* out, uint64_t capacity) {
    if (!name || name[0] == 0) return false;
    if (name[0] == '/') {
        copy_text(out, capacity, name);
        return stat_path(out);
    }

    char candidate[64];
    uint64_t cursor = 0;
    hybrid::user::append_text(candidate, sizeof(candidate), cursor, "/bin/");
    hybrid::user::append_text(candidate, sizeof(candidate), cursor, name);
    if (stat_path(candidate)) {
        copy_text(out, capacity, candidate);
        return true;
    }

    cursor = 0;
    hybrid::user::append_text(candidate, sizeof(candidate), cursor, "/bin/");
    hybrid::user::append_text(candidate, sizeof(candidate), cursor, name);
    hybrid::user::append_text(candidate, sizeof(candidate), cursor, ".elf");
    if (!stat_path(candidate)) return false;
    copy_text(out, capacity, candidate);
    return true;
}

bool append_quoted_arg(char* line, uint64_t capacity, uint64_t& cursor, const char* text) {
    if (!text || text[0] == 0) return true;
    if (cursor + 2 >= capacity) return false;
    hybrid::user::append_char(line, capacity, cursor, ' ');
    for (uint64_t i = 0; text[i] != 0; ++i) {
        char c = text[i];
        if (c == ' ' || c == '\t' || c == '\'' || c == '"' || c == '\\') {
            if (cursor + 2 >= capacity) return false;
            hybrid::user::append_char(line, capacity, cursor, '\\');
        }
        hybrid::user::append_char(line, capacity, cursor, c);
    }
    return true;
}

bool append_stdin_words(char* line, uint64_t capacity, uint64_t& cursor, uint64_t& words) {
    char token[32];
    uint64_t token_len = 0;
    char buffer[32];
    for (;;) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::Read,
                                          hybrid::kStdinFd,
                                          reinterpret_cast<uint64_t>(buffer),
                                          sizeof(buffer));
        if (read.error == hybrid::kSyscallErrorWouldBlock) {
            hybrid::user::syscall(hybrid::SyscallNumber::Yield);
            continue;
        }
        if (read.value == 0 && (read.error == hybrid::kSyscallErrorNone || read.error == hybrid::kSyscallErrorNotFound)) break;
        if (read.error != hybrid::kSyscallErrorNone) {
            hybrid::user::write_hex_line("[xargs] ", "read error ", read.error);
            return false;
        }
        for (uint64_t i = 0; i < read.value; ++i) {
            char c = buffer[i];
            bool separator = c == ' ' || c == '\t' || c == '\r' || c == '\n';
            if (!separator && token_len + 1 < sizeof(token)) {
                token[token_len++] = c;
                token[token_len] = 0;
                continue;
            }
            if (token_len == 0) continue;
            if (!append_quoted_arg(line, capacity, cursor, token)) return false;
            ++words;
            token_len = 0;
            token[0] = 0;
        }
    }
    if (token_len != 0) {
        if (!append_quoted_arg(line, capacity, cursor, token)) return false;
        ++words;
    }
    return true;
}

uint64_t wait_reap(uint64_t pid) {
    uint64_t code = 0;
    for (;;) {
        auto waited = hybrid::user::syscall(hybrid::SyscallNumber::Wait, pid, reinterpret_cast<uint64_t>(&code));
        if (waited.error == hybrid::kSyscallErrorNone) break;
        hybrid::user::syscall(hybrid::SyscallNumber::Yield);
    }
    hybrid::user::syscall(hybrid::SyscallNumber::ReapProcess, pid);
    return code;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo command{};
    if (!get_arg(1, command)) {
        hybrid::user::write_line("[xargs] usage: xargs <command> [args...]");
        hybrid::user::exit(1);
    }

    char path[64];
    if (!resolve_command(command.value, path, sizeof(path))) {
        hybrid::user::write_text_line("[xargs] ", "not found ", command.value);
        hybrid::user::exit(127);
    }

    char line[kCommandLineCapacity];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, path);
    for (uint64_t i = 2;; ++i) {
        hybrid::ArgumentInfo arg{};
        if (!get_arg(i, arg)) break;
        if (!append_quoted_arg(line, sizeof(line), cursor, arg.value)) {
            hybrid::user::write_line("[xargs] command too long");
            hybrid::user::exit(1);
        }
    }

    uint64_t words = 0;
    if (!append_stdin_words(line, sizeof(line), cursor, words)) {
        hybrid::user::write_line("[xargs] command too long");
        hybrid::user::exit(1);
    }
    if (words == 0) {
        hybrid::user::write_line("[xargs] no input");
        hybrid::user::exit(0);
    }

    uint64_t pid = 0;
    auto spawned = hybrid::user::syscall(hybrid::SyscallNumber::Spawn,
                                         reinterpret_cast<uint64_t>(line),
                                         hybrid::user::strlen(line) + 1,
                                         reinterpret_cast<uint64_t>(&pid),
                                         0);
    if (spawned.error != hybrid::kSyscallErrorNone || pid == 0) {
        hybrid::user::write_hex_line("[xargs] ", "spawn error ", spawned.error);
        hybrid::user::exit(126);
    }

    hybrid::user::write_text_line("[xargs] ", "command ", path);
    hybrid::user::write_hex_line("[xargs] ", "words ", words);
    hybrid::user::write_hex_line("[xargs] ", "pid ", pid);
    uint64_t code = wait_reap(pid);
    hybrid::user::write_hex_line("[xargs] ", "exit ", code);
    hybrid::user::exit(code);
}
