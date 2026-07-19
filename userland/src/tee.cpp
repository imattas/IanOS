#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

bool streq(const char* left, const char* right) {
    uint64_t i = 0;
    while (left[i] != 0 && right[i] != 0) {
        if (left[i] != right[i]) return false;
        ++i;
    }
    return left[i] == 0 && right[i] == 0;
}

void write_error_hex(const char* label, uint64_t value) {
    char line[128];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[tee] ");
    hybrid::user::append_text(line, sizeof(line), cursor, label);
    hybrid::user::append_hex(line, sizeof(line), cursor, value);
    hybrid::user::append_text(line, sizeof(line), cursor, "\n");
    hybrid::user::write_error(line);
}

void write_error_text(const char* label, const char* value) {
    char line[128];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[tee] ");
    hybrid::user::append_text(line, sizeof(line), cursor, label);
    hybrid::user::append_text(line, sizeof(line), cursor, value);
    hybrid::user::append_text(line, sizeof(line), cursor, "\n");
    hybrid::user::write_error(line);
}

hybrid::SyscallResult read_blocking(char* buffer, uint64_t size) {
    bool reported_wait = false;
    for (;;) {
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::Read, hybrid::kStdinFd, reinterpret_cast<uint64_t>(buffer), size);
        if (result.error != hybrid::kSyscallErrorWouldBlock) return result;
        if (!reported_wait) {
            hybrid::user::write_error("[tee] pipe read wouldblock\n");
            reported_wait = true;
        }
        hybrid::user::syscall(hybrid::SyscallNumber::Yield);
    }
}

bool write_stdout_blocking(const char* buffer, uint64_t size) {
    uint64_t written = 0;
    while (written < size) {
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::Write, hybrid::kStdoutFd, reinterpret_cast<uint64_t>(buffer + written), size - written);
        if (result.error == hybrid::kSyscallErrorWouldBlock) {
            hybrid::user::syscall(hybrid::SyscallNumber::Yield);
            continue;
        }
        if (result.error != hybrid::kSyscallErrorNone || result.value == 0) return false;
        written += result.value;
    }
    return true;
}

bool write_file_all(uint64_t fd, const char* buffer, uint64_t size) {
    uint64_t written = 0;
    while (written < size) {
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::WriteFile, fd, reinterpret_cast<uint64_t>(buffer + written), size - written);
        if (result.error != hybrid::kSyscallErrorNone || result.value == 0) return false;
        written += result.value;
    }
    return true;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo first;
    hybrid::ArgumentInfo path;
    bool append = false;
    if (!get_arg(1, first)) {
        hybrid::user::write_error("[tee] usage: tee [-a] <path>\n");
        hybrid::user::exit(1);
    }
    if (streq(first.value, "-a")) {
        append = true;
        if (!get_arg(2, path)) {
            hybrid::user::write_error("[tee] usage: tee [-a] <path>\n");
            hybrid::user::exit(1);
        }
    } else {
        path = first;
    }

    if (append) {
        auto exists = hybrid::user::syscall(hybrid::SyscallNumber::VfsStat, reinterpret_cast<uint64_t>(path.value), hybrid::user::strlen(path.value) + 1);
        if (exists.error != hybrid::kSyscallErrorNone) {
            auto created = hybrid::user::syscall(hybrid::SyscallNumber::CreateFile, reinterpret_cast<uint64_t>(path.value), hybrid::user::strlen(path.value) + 1);
            if (created.error != hybrid::kSyscallErrorNone) {
                write_error_hex("create error ", created.error);
                hybrid::user::exit(2);
            }
        }
    } else {
        hybrid::user::syscall(hybrid::SyscallNumber::DeleteFile, reinterpret_cast<uint64_t>(path.value), hybrid::user::strlen(path.value) + 1);
        auto created = hybrid::user::syscall(hybrid::SyscallNumber::CreateFile, reinterpret_cast<uint64_t>(path.value), hybrid::user::strlen(path.value) + 1);
        if (created.error != hybrid::kSyscallErrorNone) {
            write_error_hex("create error ", created.error);
            hybrid::user::exit(2);
        }
    }

    auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open, reinterpret_cast<uint64_t>(path.value), hybrid::user::strlen(path.value) + 1);
    if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
        write_error_hex("open error ", opened.error);
        hybrid::user::exit(3);
    }
    if (append) {
        auto size = hybrid::user::syscall(hybrid::SyscallNumber::VfsStat, reinterpret_cast<uint64_t>(path.value), hybrid::user::strlen(path.value) + 1);
        if (size.error == hybrid::kSyscallErrorNone) hybrid::user::syscall(hybrid::SyscallNumber::Seek, opened.value, size.value);
    }

    uint64_t total = 0;
    char buffer[64];
    for (;;) {
        auto read = read_blocking(buffer, sizeof(buffer));
        if (read.error != hybrid::kSyscallErrorNone || read.value == 0) break;
        if (!write_stdout_blocking(buffer, read.value) || !write_file_all(opened.value, buffer, read.value)) {
            hybrid::user::syscall(hybrid::SyscallNumber::Close, opened.value);
            hybrid::user::exit(4);
        }
        total += read.value;
    }

    hybrid::user::syscall(hybrid::SyscallNumber::Close, opened.value);
    write_error_text("path ", path.value);
    write_error_hex("bytes ", total);
    hybrid::user::exit(total);
}
