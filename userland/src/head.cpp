#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

}

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo path;
    uint64_t fd = hybrid::kStdinFd;
    bool close_when_done = false;
    if (get_arg(1, path)) {
        auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open, reinterpret_cast<uint64_t>(path.value), hybrid::user::strlen(path.value) + 1);
        if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
            hybrid::user::write_hex_line("[head] ", "open error ", opened.error);
            hybrid::user::exit(2);
        }
        fd = opened.value;
        close_when_done = true;
    }

    hybrid::user::write_text_line("[head] ", "path ", close_when_done ? path.value : "<stdin>");
    char c = 0;
    uint64_t lines = 0;
    bool emitted = false;
    while (lines < 3) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::Read, fd, reinterpret_cast<uint64_t>(&c), 1);
        if (read.error != hybrid::kSyscallErrorNone || read.value == 0) break;
        hybrid::user::syscall(hybrid::SyscallNumber::Write, hybrid::kStdoutFd, reinterpret_cast<uint64_t>(&c), 1);
        emitted = true;
        if (c == '\n') ++lines;
    }
    if (emitted && c != '\n') hybrid::user::write_text("\n");
    if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);
    hybrid::user::exit(lines);
}
