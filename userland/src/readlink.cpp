#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

void append_decimal(char* buffer, uint64_t capacity, uint64_t& cursor, uint64_t value) {
    char digits[21];
    uint64_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && count < sizeof(digits));
    while (count > 0) hybrid::user::append_char(buffer, capacity, cursor, digits[--count]);
}

bool resolve_link(const char* path, char* target, uint64_t capacity, hybrid::SyscallResult& result) {
    for (uint64_t i = 0; i < capacity; ++i) target[i] = 0;
    result = hybrid::user::syscall(hybrid::SyscallNumber::ReadLink,
                                   reinterpret_cast<uint64_t>(path),
                                   hybrid::user::strlen(path) + 1,
                                   reinterpret_cast<uint64_t>(target),
                                   capacity);
    return result.error == hybrid::kSyscallErrorNone;
}

void print_numeric_fd_entries(uint64_t pid) {
    char fd_dir[64];
    uint64_t cursor = 0;
    hybrid::user::append_text(fd_dir, sizeof(fd_dir), cursor, "/proc/");
    append_decimal(fd_dir, sizeof(fd_dir), cursor, pid);
    hybrid::user::append_text(fd_dir, sizeof(fd_dir), cursor, "/fd");
    for (uint64_t index = 0; index < 8; ++index) {
        hybrid::VfsDirectoryEntryInfo entry{};
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::ReadDirectory,
                                            reinterpret_cast<uint64_t>(fd_dir),
                                            hybrid::user::strlen(fd_dir) + 1,
                                            index,
                                            reinterpret_cast<uint64_t>(&entry));
        if (result.error != hybrid::kSyscallErrorNone || result.value != 1) break;
        hybrid::user::write_text_line("[readlink] ", "fd entry ", entry.path);
    }
}

}

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo path;
    if (!get_arg(1, path)) {
        hybrid::user::write_line("[readlink] usage: readlink <path>");
        hybrid::user::exit(1);
    }

    char target[128];
    hybrid::SyscallResult result{};
    if (!resolve_link(path.value, target, sizeof(target), result)) {
        hybrid::user::write_hex_line("[readlink] ", "error ", result.error);
        hybrid::user::exit(2);
    }

    hybrid::user::write_text_line("[readlink] ", "path ", path.value);
    hybrid::user::write_text_line("[readlink] ", "target ", target);
    hybrid::user::write_hex_line("[readlink] ", "bytes ", result.value);

    auto pid = hybrid::user::syscall(hybrid::SyscallNumber::GetCurrentProcessId);
    if (pid.error == hybrid::kSyscallErrorNone && pid.value != 0) {
        char numeric_path[64];
        uint64_t cursor = 0;
        hybrid::user::append_text(numeric_path, sizeof(numeric_path), cursor, "/proc/");
        append_decimal(numeric_path, sizeof(numeric_path), cursor, pid.value);
        hybrid::user::append_text(numeric_path, sizeof(numeric_path), cursor, "/fd/1");
        char numeric_target[128];
        hybrid::SyscallResult numeric_result{};
        if (resolve_link(numeric_path, numeric_target, sizeof(numeric_target), numeric_result)) {
            hybrid::user::write_text_line("[readlink] ", "numeric ", numeric_path);
            hybrid::user::write_text_line("[readlink] ", "numeric target ", numeric_target);
            hybrid::user::write_hex_line("[readlink] ", "numeric bytes ", numeric_result.value);
        } else {
            hybrid::user::write_hex_line("[readlink] ", "numeric error ", numeric_result.error);
        }
        print_numeric_fd_entries(pid.value);
    }
    hybrid::user::exit(result.value);
}
