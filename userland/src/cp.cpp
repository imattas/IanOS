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
    hybrid::ArgumentInfo source;
    hybrid::ArgumentInfo destination;
    if (!get_arg(1, source) || !get_arg(2, destination)) {
        hybrid::user::write_line("[cp] usage: cp <source> <destination>");
        hybrid::user::exit(1);
    }

    auto input = hybrid::user::syscall(hybrid::SyscallNumber::Open, reinterpret_cast<uint64_t>(source.value), hybrid::user::strlen(source.value) + 1);
    if (input.error != hybrid::kSyscallErrorNone || input.value < 3) {
        hybrid::user::write_hex_line("[cp] ", "source error ", input.error);
        hybrid::user::exit(2);
    }

    auto created = hybrid::user::syscall(hybrid::SyscallNumber::CreateFile, reinterpret_cast<uint64_t>(destination.value), hybrid::user::strlen(destination.value) + 1);
    if (created.error != hybrid::kSyscallErrorNone) {
        hybrid::user::syscall(hybrid::SyscallNumber::Close, input.value);
        hybrid::user::write_hex_line("[cp] ", "create error ", created.error);
        hybrid::user::exit(3);
    }

    auto output = hybrid::user::syscall(hybrid::SyscallNumber::Open, reinterpret_cast<uint64_t>(destination.value), hybrid::user::strlen(destination.value) + 1);
    if (output.error != hybrid::kSyscallErrorNone || output.value < 3) {
        hybrid::user::syscall(hybrid::SyscallNumber::Close, input.value);
        hybrid::user::write_hex_line("[cp] ", "dest error ", output.error);
        hybrid::user::exit(4);
    }

    uint64_t total = 0;
    char buffer[32];
    for (;;) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::Read, input.value, reinterpret_cast<uint64_t>(buffer), sizeof(buffer));
        if (read.error != hybrid::kSyscallErrorNone || read.value == 0) break;
        auto wrote = hybrid::user::syscall(hybrid::SyscallNumber::WriteFile, output.value, reinterpret_cast<uint64_t>(buffer), read.value);
        if (wrote.error != hybrid::kSyscallErrorNone || wrote.value != read.value) {
            hybrid::user::syscall(hybrid::SyscallNumber::Close, input.value);
            hybrid::user::syscall(hybrid::SyscallNumber::Close, output.value);
            hybrid::user::write_hex_line("[cp] ", "write error ", wrote.error);
            hybrid::user::exit(5);
        }
        total += wrote.value;
    }

    hybrid::user::syscall(hybrid::SyscallNumber::Close, input.value);
    hybrid::user::syscall(hybrid::SyscallNumber::Close, output.value);
    hybrid::user::write_text_line("[cp] ", "from ", source.value);
    hybrid::user::write_text_line("[cp] ", "to ", destination.value);
    hybrid::user::write_hex_line("[cp] ", "bytes ", total);
    hybrid::user::exit(total);
}
