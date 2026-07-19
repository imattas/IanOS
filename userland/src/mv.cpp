#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

bool copy_file(const char* source, const char* destination, uint64_t& total) {
    total = 0;
    auto input = hybrid::user::syscall(hybrid::SyscallNumber::Open, reinterpret_cast<uint64_t>(source), hybrid::user::strlen(source) + 1);
    if (input.error != hybrid::kSyscallErrorNone || input.value < 3) return false;

    auto created = hybrid::user::syscall(hybrid::SyscallNumber::CreateFile, reinterpret_cast<uint64_t>(destination), hybrid::user::strlen(destination) + 1);
    if (created.error != hybrid::kSyscallErrorNone) {
        hybrid::user::syscall(hybrid::SyscallNumber::Close, input.value);
        return false;
    }
    auto output = hybrid::user::syscall(hybrid::SyscallNumber::Open, reinterpret_cast<uint64_t>(destination), hybrid::user::strlen(destination) + 1);
    if (output.error != hybrid::kSyscallErrorNone || output.value < 3) {
        hybrid::user::syscall(hybrid::SyscallNumber::Close, input.value);
        return false;
    }

    char buffer[32];
    for (;;) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::Read, input.value, reinterpret_cast<uint64_t>(buffer), sizeof(buffer));
        if (read.error != hybrid::kSyscallErrorNone || read.value == 0) break;
        auto wrote = hybrid::user::syscall(hybrid::SyscallNumber::WriteFile, output.value, reinterpret_cast<uint64_t>(buffer), read.value);
        if (wrote.error != hybrid::kSyscallErrorNone || wrote.value != read.value) {
            hybrid::user::syscall(hybrid::SyscallNumber::Close, input.value);
            hybrid::user::syscall(hybrid::SyscallNumber::Close, output.value);
            return false;
        }
        total += wrote.value;
    }

    hybrid::user::syscall(hybrid::SyscallNumber::Close, input.value);
    hybrid::user::syscall(hybrid::SyscallNumber::Close, output.value);
    return true;
}

}

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo source;
    hybrid::ArgumentInfo destination;
    if (!get_arg(1, source) || !get_arg(2, destination)) {
        hybrid::user::write_line("[mv] usage: mv <source> <destination>");
        hybrid::user::exit(1);
    }

    auto renamed = hybrid::user::syscall(hybrid::SyscallNumber::Rename,
                                         reinterpret_cast<uint64_t>(source.value),
                                         hybrid::user::strlen(source.value) + 1,
                                         reinterpret_cast<uint64_t>(destination.value),
                                         hybrid::user::strlen(destination.value) + 1);
    if (renamed.error == hybrid::kSyscallErrorNone) {
        hybrid::user::write_text_line("[mv] ", "from ", source.value);
        hybrid::user::write_text_line("[mv] ", "to ", destination.value);
        hybrid::user::write_line("[mv] mode rename");
        hybrid::user::exit(0);
    }

    uint64_t total = 0;
    if (!copy_file(source.value, destination.value, total)) {
        hybrid::user::write_line("[mv] copy failed");
        hybrid::user::exit(2);
    }
    auto deleted = hybrid::user::syscall(hybrid::SyscallNumber::DeleteFile, reinterpret_cast<uint64_t>(source.value), hybrid::user::strlen(source.value) + 1);
    if (deleted.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[mv] ", "delete error ", deleted.error);
        hybrid::user::exit(3);
    }
    hybrid::user::write_text_line("[mv] ", "from ", source.value);
    hybrid::user::write_text_line("[mv] ", "to ", destination.value);
    hybrid::user::write_hex_line("[mv] ", "bytes ", total);
    hybrid::user::exit(0);
}
