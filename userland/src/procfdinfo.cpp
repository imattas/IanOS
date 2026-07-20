#include "hybrid/user.hpp"

namespace {

char g_chunk[128];

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

bool parse_number(const char* text, uint64_t& value) {
    value = 0;
    if (!text || text[0] == 0) return false;
    for (uint64_t i = 0; text[i] != 0; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + static_cast<uint64_t>(text[i] - '0');
    }
    return true;
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

bool stream_file(const char* path, uint64_t& bytes) {
    bytes = 0;
    auto opened = hybrid::user::syscall(hybrid::SyscallNumber::VfsOpen,
                                        reinterpret_cast<uint64_t>(path),
                                        hybrid::user::strlen(path) + 1);
    if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
        hybrid::user::write_hex_line("[procfdinfo] ", "open error ", opened.error);
        return false;
    }

    hybrid::user::write_text_line("[procfdinfo] ", "path ", path);
    for (;;) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::VfsReadHandle,
                                          opened.value,
                                          reinterpret_cast<uint64_t>(g_chunk),
                                          sizeof(g_chunk));
        if (read.error != hybrid::kSyscallErrorNone) {
            if (read.error == hybrid::kSyscallErrorNotFound && bytes != 0) break;
            hybrid::user::write_hex_line("[procfdinfo] ", "read error ", read.error);
            hybrid::user::syscall(hybrid::SyscallNumber::VfsClose, opened.value);
            return false;
        }
        if (read.value == 0) break;
        bytes += read.value;
        hybrid::user::write_text("[procfdinfo] ");
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
    char path[64]{};
    uint64_t cursor = 0;
    hybrid::ArgumentInfo pid_arg{};
    hybrid::ArgumentInfo fd_arg{};

    if (!get_arg(1, pid_arg)) {
        hybrid::user::append_text(path, sizeof(path), cursor, "/proc/self/fdinfo");
    } else {
        uint64_t pid = 0;
        if (!parse_number(pid_arg.value, pid) || pid == 0) {
            hybrid::user::write_line("[procfdinfo] usage procfdinfo [pid [fd]]");
            hybrid::user::exit(2);
        }
        hybrid::user::append_text(path, sizeof(path), cursor, "/proc/");
        append_decimal(path, sizeof(path), cursor, pid);
        hybrid::user::append_text(path, sizeof(path), cursor, "/fdinfo");
        if (get_arg(2, fd_arg)) {
            uint64_t fd = 0;
            if (!parse_number(fd_arg.value, fd)) {
                hybrid::user::write_line("[procfdinfo] usage procfdinfo [pid [fd]]");
                hybrid::user::exit(2);
            }
            hybrid::user::append_char(path, sizeof(path), cursor, '/');
            append_decimal(path, sizeof(path), cursor, fd);
        }
    }

    uint64_t bytes = 0;
    if (!stream_file(path, bytes)) hybrid::user::exit(1);
    hybrid::user::write_hex_line("[procfdinfo] ", "bytes ", bytes);
    hybrid::user::exit(0);
}
