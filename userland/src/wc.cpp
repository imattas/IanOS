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

struct Counts {
    uint64_t bytes;
    uint64_t lines;
    uint64_t words;
};

bool count_stream(uint64_t fd, Counts& counts) {
    bool in_word = false;
    char buffer[32];
    for (;;) {
        auto read = read_blocking(fd, buffer, sizeof(buffer));
        if (read.value == 0 && (read.error == hybrid::kSyscallErrorNone || read.error == hybrid::kSyscallErrorNotFound)) break;
        if (read.error != hybrid::kSyscallErrorNone) return false;
        counts.bytes += read.value;
        for (uint64_t i = 0; i < read.value; ++i) {
            if (buffer[i] == '\n') ++counts.lines;
            if (is_space(buffer[i])) {
                in_word = false;
            } else if (!in_word) {
                in_word = true;
                ++counts.words;
            }
        }
    }
    return true;
}

void print_counts(const char* label, const Counts& counts) {
    hybrid::user::write_text_line("[wc] ", "path ", label);
    hybrid::user::write_hex_line("[wc] ", "bytes ", counts.bytes);
    hybrid::user::write_hex_line("[wc] ", "lines ", counts.lines);
    hybrid::user::write_hex_line("[wc] ", "words ", counts.words);
}

bool count_path(const char* path, Counts& counts) {
    auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open, reinterpret_cast<uint64_t>(path), hybrid::user::strlen(path) + 1);
    if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
        hybrid::user::write_hex_line("[wc] ", "open error ", opened.error);
        return false;
    }
    const bool ok = count_stream(opened.value, counts);
    hybrid::user::syscall(hybrid::SyscallNumber::Close, opened.value);
    return ok;
}

}

extern "C" [[noreturn]] void _start() {
    auto argc = hybrid::user::syscall(hybrid::SyscallNumber::GetArgumentCount);
    if (argc.error != hybrid::kSyscallErrorNone || argc.value < 2) {
        Counts counts{};
        if (!count_stream(hybrid::kStdinFd, counts)) {
            hybrid::user::write_line("[wc] read error");
            hybrid::user::exit(3);
        }
        print_counts("<stdin>", counts);
        hybrid::user::exit(counts.bytes);
    }

    Counts total{};
    for (uint64_t index = 1; index < argc.value; ++index) {
        hybrid::ArgumentInfo path;
        if (!get_arg(index, path)) {
            hybrid::user::write_line("[wc] missing path");
            hybrid::user::exit(2);
        }
        Counts counts{};
        if (!count_path(path.value, counts)) {
            hybrid::user::exit(2);
        }
        print_counts(path.value, counts);
        total.bytes += counts.bytes;
        total.lines += counts.lines;
        total.words += counts.words;
    }
    if (argc.value > 2) print_counts("total", total);
    hybrid::user::exit(total.bytes);
}
