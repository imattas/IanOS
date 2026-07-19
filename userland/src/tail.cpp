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
            hybrid::user::write_hex_line("[tail] ", "open error ", opened.error);
            hybrid::user::exit(2);
        }
        fd = opened.value;
        close_when_done = true;
    }

    char data[192];
    uint64_t size = 0;
    char buffer[32];
    for (;;) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::Read, fd, reinterpret_cast<uint64_t>(buffer), sizeof(buffer));
        if (read.error != hybrid::kSyscallErrorNone || read.value == 0) break;
        for (uint64_t i = 0; i < read.value; ++i) {
            if (size < sizeof(data)) {
                data[size++] = buffer[i];
            } else {
                for (uint64_t j = 1; j < sizeof(data); ++j) data[j - 1] = data[j];
                data[sizeof(data) - 1] = buffer[i];
            }
        }
    }
    if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);

    uint64_t start = 0;
    uint64_t lines_seen = 0;
    uint64_t cursor = size;
    while (cursor > 0 && lines_seen < 3) {
        --cursor;
        if (data[cursor] == '\n' && cursor + 1 < size) {
            ++lines_seen;
            if (lines_seen == 3) {
                start = cursor + 1;
                break;
            }
        }
    }
    hybrid::user::write_text_line("[tail] ", "path ", close_when_done ? path.value : "<stdin>");
    if (size > start) hybrid::user::syscall(hybrid::SyscallNumber::Write, hybrid::kStdoutFd, reinterpret_cast<uint64_t>(data + start), size - start);
    if (size == 0 || data[size - 1] != '\n') hybrid::user::write_text("\n");
    hybrid::user::exit(size);
}
