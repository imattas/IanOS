#include "hybrid/user.hpp"

namespace {

void write_all(uint32_t fd, const char* text) {
    uint64_t length = hybrid::user::strlen(text);
    uint64_t written = 0;
    while (written < length) {
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::WriteFile, fd, reinterpret_cast<uint64_t>(text + written), length - written);
        if (result.error != hybrid::kSyscallErrorNone || result.value == 0) hybrid::user::exit(5);
        written += result.value;
    }
}

}

extern "C" [[noreturn]] void _start() {
    static const char path[] = "/tmp/dup.txt";
    hybrid::user::syscall(hybrid::SyscallNumber::DeleteFile, reinterpret_cast<uint64_t>(path), sizeof(path));
    auto created = hybrid::user::syscall(hybrid::SyscallNumber::CreateFile, reinterpret_cast<uint64_t>(path), sizeof(path));
    if (created.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[duptest] ", "create error ", created.error);
        hybrid::user::exit(1);
    }
    auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open, reinterpret_cast<uint64_t>(path), sizeof(path));
    if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
        hybrid::user::write_hex_line("[duptest] ", "open error ", opened.error);
        hybrid::user::exit(2);
    }
    auto dup = hybrid::user::syscall(hybrid::SyscallNumber::Dup, opened.value);
    if (dup.error != hybrid::kSyscallErrorNone || dup.value == 0) {
        hybrid::user::write_hex_line("[duptest] ", "dup error ", dup.error);
        hybrid::user::exit(3);
    }
    hybrid::user::write_hex_line("[duptest] ", "dup fd ", dup.value);
    write_all(static_cast<uint32_t>(dup.value), "dup-write\n");
    auto dup2 = hybrid::user::syscall(hybrid::SyscallNumber::Dup2, opened.value, hybrid::kStdoutFd);
    if (dup2.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[duptest] ", "dup2 error ", dup2.error);
        hybrid::user::exit(4);
    }
    hybrid::user::write_error("[duptest] dup2 stdout ok\n");
    hybrid::user::write_text("dup2-stdout\n");
    hybrid::user::syscall(hybrid::SyscallNumber::Close, dup.value);
    hybrid::user::syscall(hybrid::SyscallNumber::Close, opened.value);
    hybrid::user::exit(0);
}
