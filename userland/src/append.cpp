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
    hybrid::ArgumentInfo text;
    if (!get_arg(1, path) || !get_arg(2, text)) {
        hybrid::user::write_line("[append] usage: append <path> <text>");
        hybrid::user::exit(1);
    }
    auto created = hybrid::user::syscall(hybrid::SyscallNumber::CreateFile, reinterpret_cast<uint64_t>(path.value), hybrid::user::strlen(path.value) + 1);
    (void)created;
    auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open, reinterpret_cast<uint64_t>(path.value), hybrid::user::strlen(path.value) + 1);
    if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
        hybrid::user::write_hex_line("[append] ", "open error ", opened.error);
        hybrid::user::exit(2);
    }
    auto size = hybrid::user::syscall(hybrid::SyscallNumber::VfsStat, reinterpret_cast<uint64_t>(path.value), hybrid::user::strlen(path.value) + 1);
    if (size.error == hybrid::kSyscallErrorNone) {
        hybrid::user::syscall(hybrid::SyscallNumber::Seek, opened.value, size.value);
    }
    auto wrote = hybrid::user::syscall(hybrid::SyscallNumber::WriteFile, opened.value, reinterpret_cast<uint64_t>(text.value), hybrid::user::strlen(text.value));
    static const char newline[] = "\n";
    hybrid::user::syscall(hybrid::SyscallNumber::WriteFile, opened.value, reinterpret_cast<uint64_t>(newline), 1);
    hybrid::user::syscall(hybrid::SyscallNumber::Close, opened.value);
    if (wrote.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[append] ", "write error ", wrote.error);
        hybrid::user::exit(3);
    }
    hybrid::user::write_text_line("[append] ", "path ", path.value);
    hybrid::user::write_hex_line("[append] ", "bytes ", wrote.value);
    hybrid::user::exit(wrote.value);
}
