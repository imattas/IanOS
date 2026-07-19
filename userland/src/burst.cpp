#include "hybrid/user.hpp"

namespace {

void write_all(const char* data, uint64_t size) {
    uint64_t written = 0;
    uint64_t blocks = 0;
    while (written < size) {
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::Write, hybrid::kStdoutFd, reinterpret_cast<uint64_t>(data + written), size - written);
        if (result.error == hybrid::kSyscallErrorWouldBlock) {
            ++blocks;
            hybrid::user::syscall(hybrid::SyscallNumber::Yield);
            continue;
        }
        if (result.error != hybrid::kSyscallErrorNone || result.value == 0) {
            char error[128];
            uint64_t cursor = 0;
            hybrid::user::append_text(error, sizeof(error), cursor, "[burst] write error ");
            hybrid::user::append_hex(error, sizeof(error), cursor, result.error);
            hybrid::user::append_text(error, sizeof(error), cursor, "\n");
            hybrid::user::write_error(error);
            hybrid::user::exit(2);
        }
        written += result.value;
    }
    char line[128];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[burst] bytes ");
    hybrid::user::append_hex(line, sizeof(line), cursor, written);
    hybrid::user::append_text(line, sizeof(line), cursor, "\n[burst] wouldblock polls ");
    hybrid::user::append_hex(line, sizeof(line), cursor, blocks);
    hybrid::user::append_text(line, sizeof(line), cursor, "\n");
    hybrid::user::write_error(line);
}

}

extern "C" [[noreturn]] void _start() {
    char data[4096];
    for (uint64_t i = 0; i < sizeof(data); ++i) data[i] = (i % 63 == 62) ? '\n' : static_cast<char>('a' + (i % 26));
    write_all(data, sizeof(data));
    hybrid::user::exit(0);
}
