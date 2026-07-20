#include "hybrid/user.hpp"

namespace {

char g_chunk[192];

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

bool parse_pid(const char* text, uint64_t& pid) {
    pid = 0;
    if (text[0] == 0) return false;
    for (uint64_t i = 0; text[i] != 0; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        pid = pid * 10 + static_cast<uint64_t>(text[i] - '0');
    }
    return pid != 0;
}

void append_decimal(char* buffer, uint64_t capacity, uint64_t& cursor, uint64_t value) {
    char digits[24];
    uint64_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && count < sizeof(digits));
    while (count > 0) hybrid::user::append_char(buffer, capacity, cursor, digits[--count]);
}

void build_pid_maps_path(uint64_t pid, char (&path)[32]) {
    uint64_t cursor = 0;
    hybrid::user::append_text(path, sizeof(path), cursor, "/proc/");
    append_decimal(path, sizeof(path), cursor, pid);
    hybrid::user::append_text(path, sizeof(path), cursor, "/maps");
}

bool stream_file(const char* path, uint64_t& bytes) {
    bytes = 0;
    auto opened = hybrid::user::syscall(hybrid::SyscallNumber::VfsOpen,
                                        reinterpret_cast<uint64_t>(path),
                                        hybrid::user::strlen(path) + 1);
    if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
        hybrid::user::write_hex_line("[maps] ", "open error ", opened.error);
        return false;
    }

    hybrid::user::write_text_line("[maps] ", "path ", path);
    for (;;) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::VfsReadHandle,
                                          opened.value,
                                          reinterpret_cast<uint64_t>(g_chunk),
                                          sizeof(g_chunk));
        if (read.error != hybrid::kSyscallErrorNone) {
            if (read.error == hybrid::kSyscallErrorNotFound && bytes != 0) break;
            hybrid::user::write_hex_line("[maps] ", "read error ", read.error);
            hybrid::user::syscall(hybrid::SyscallNumber::VfsClose, opened.value);
            return false;
        }
        if (read.value == 0) break;
        bytes += read.value;
        hybrid::user::syscall(hybrid::SyscallNumber::Write,
                              hybrid::kStdoutFd,
                              reinterpret_cast<uint64_t>(g_chunk),
                              read.value);
    }

    hybrid::user::syscall(hybrid::SyscallNumber::VfsClose, opened.value);
    return bytes != 0;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    static const char self_path[] = "/proc/self/maps";
    const char* path = self_path;
    char pid_path[32]{};

    hybrid::ArgumentInfo arg;
    if (get_arg(1, arg)) {
        uint64_t pid = 0;
        if (!parse_pid(arg.value, pid)) {
            hybrid::user::write_line("[maps] usage maps [pid]");
            hybrid::user::exit(2);
        }
        build_pid_maps_path(pid, pid_path);
        path = pid_path;
    }

    uint64_t bytes = 0;
    if (!stream_file(path, bytes)) hybrid::user::exit(1);
    hybrid::user::write_hex_line("[maps] ", "bytes ", bytes);
    hybrid::user::exit(0);
}
