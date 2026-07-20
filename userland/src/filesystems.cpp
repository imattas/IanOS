#include "hybrid/user.hpp"

namespace {

char g_buffer[128];
char g_line[160];

void write_buffer(uint64_t fd, const char* data, uint64_t length) {
    uint64_t offset = 0;
    while (offset < length) {
        auto written = hybrid::user::syscall(hybrid::SyscallNumber::Write,
                                             fd,
                                             reinterpret_cast<uint64_t>(data + offset),
                                             length - offset);
        if (written.error == hybrid::kSyscallErrorWouldBlock) {
            hybrid::user::syscall(hybrid::SyscallNumber::Yield);
            continue;
        }
        if (written.error != hybrid::kSyscallErrorNone || written.value == 0) break;
        offset += written.value;
    }
}

void emit_line(const char* line, uint64_t length) {
    hybrid::user::write_text("[filesystems] ");
    write_buffer(hybrid::kStdoutFd, line, length);
    if (length == 0 || line[length - 1] != '\n') hybrid::user::write_text("\n");
}

bool stream_filesystems(uint64_t& lines) {
    lines = 0;
    static const char path[] = "/proc/filesystems";
    auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open,
                                        reinterpret_cast<uint64_t>(path),
                                        sizeof(path));
    if (opened.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[filesystems] ", "open error ", opened.error);
        return false;
    }

    uint64_t line_length = 0;
    for (;;) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::Read,
                                          opened.value,
                                          reinterpret_cast<uint64_t>(g_buffer),
                                          sizeof(g_buffer));
        if (read.error == hybrid::kSyscallErrorNotFound && line_length == 0) break;
        if (read.error != hybrid::kSyscallErrorNone) {
            hybrid::user::write_hex_line("[filesystems] ", "read error ", read.error);
            hybrid::user::syscall(hybrid::SyscallNumber::Close, opened.value);
            return false;
        }
        if (read.value == 0) break;
        for (uint64_t i = 0; i < read.value; ++i) {
            if (line_length + 1 < sizeof(g_line)) g_line[line_length++] = g_buffer[i];
            if (g_buffer[i] == '\n') {
                emit_line(g_line, line_length);
                ++lines;
                line_length = 0;
            }
        }
    }
    if (line_length != 0) {
        emit_line(g_line, line_length);
        ++lines;
    }

    hybrid::user::syscall(hybrid::SyscallNumber::Close, opened.value);
    return lines != 0;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    uint64_t lines = 0;
    if (!stream_filesystems(lines)) hybrid::user::exit(1);
    hybrid::user::write_hex_line("[filesystems] ", "lines ", lines);
    hybrid::user::exit(lines >= 4 ? 0 : 2);
}
