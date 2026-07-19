#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

bool is_space(char c) {
    return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

hybrid::SyscallResult read_blocking(uint64_t fd, char* buffer, uint64_t size) {
    bool reported_wait = false;
    for (;;) {
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::Read, fd, reinterpret_cast<uint64_t>(buffer), size);
        if (result.error != hybrid::kSyscallErrorWouldBlock) return result;
        if (!reported_wait) {
            hybrid::user::write_line("[wc] pipe read wouldblock");
            reported_wait = true;
        }
        hybrid::user::syscall(hybrid::SyscallNumber::Yield);
    }
}

}

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo path;
    uint64_t fd = hybrid::kStdinFd;
    bool close_when_done = false;
    bool has_path = get_arg(1, path);
    if (has_path) {
        auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open, reinterpret_cast<uint64_t>(path.value), hybrid::user::strlen(path.value) + 1);
        if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
            hybrid::user::write_hex_line("[wc] ", "open error ", opened.error);
            hybrid::user::exit(2);
        }
        fd = opened.value;
        close_when_done = true;
    }
    uint64_t bytes = 0;
    uint64_t lines = 0;
    uint64_t words = 0;
    bool in_word = false;
    char buffer[32];
    for (;;) {
        auto read = read_blocking(fd, buffer, sizeof(buffer));
        if (read.error != hybrid::kSyscallErrorNone || read.value == 0) break;
        bytes += read.value;
        for (uint64_t i = 0; i < read.value; ++i) {
            if (buffer[i] == '\n') ++lines;
            if (is_space(buffer[i])) {
                in_word = false;
            } else if (!in_word) {
                in_word = true;
                ++words;
            }
        }
    }
    if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);
    hybrid::user::write_text_line("[wc] ", "path ", has_path ? path.value : "<stdin>");
    hybrid::user::write_hex_line("[wc] ", "bytes ", bytes);
    hybrid::user::write_hex_line("[wc] ", "lines ", lines);
    hybrid::user::write_hex_line("[wc] ", "words ", words);
    hybrid::user::exit(bytes);
}
