#include "hybrid/user.hpp"

namespace {

void write_prefixed_line(const char* line, uint64_t length) {
    hybrid::user::write_text("[diskstats] ");
    uint64_t offset = 0;
    while (offset < length) {
        auto written = hybrid::user::syscall(hybrid::SyscallNumber::Write,
                                             hybrid::kStdoutFd,
                                             reinterpret_cast<uint64_t>(line + offset),
                                             length - offset);
        if (written.error == hybrid::kSyscallErrorWouldBlock) {
            hybrid::user::syscall(hybrid::SyscallNumber::Yield);
            continue;
        }
        if (written.error != hybrid::kSyscallErrorNone || written.value == 0) break;
        offset += written.value;
    }
    if (length == 0 || line[length - 1] != '\n') hybrid::user::write_text("\n");
}

int main_result() {
    auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open,
                                        reinterpret_cast<uint64_t>("/proc/diskstats"),
                                        sizeof("/proc/diskstats"));
    if (opened.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[diskstats] ", "open error ", opened.error);
        return 1;
    }

    char buffer[256];
    char line[256];
    uint64_t line_len = 0;
    while (true) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::Read,
                                          opened.value,
                                          reinterpret_cast<uint64_t>(buffer),
                                          sizeof(buffer));
        if (read.error == hybrid::kSyscallErrorNotFound && line_len == 0) break;
        if (read.error != hybrid::kSyscallErrorNone) {
            hybrid::user::write_hex_line("[diskstats] ", "read error ", read.error);
            return 2;
        }
        if (read.value == 0) break;
        for (uint64_t i = 0; i < read.value; ++i) {
            if (line_len + 1 < sizeof(line)) line[line_len++] = buffer[i];
            if (buffer[i] == '\n') {
                write_prefixed_line(line, line_len);
                line_len = 0;
            }
        }
    }
    if (line_len != 0) write_prefixed_line(line, line_len);
    return 0;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::user::exit(static_cast<uint64_t>(main_result()));
}
