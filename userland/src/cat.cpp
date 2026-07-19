#include "hybrid/syscall.hpp"

namespace {

hybrid::SyscallResult syscall(hybrid::SyscallNumber number, uint64_t arg0 = 0, uint64_t arg1 = 0, uint64_t arg2 = 0, uint64_t arg3 = 0) {
    register uint64_t rax asm("rax") = static_cast<uint64_t>(number);
    register uint64_t rdi asm("rdi") = arg0;
    register uint64_t rsi asm("rsi") = arg1;
    register uint64_t rdx asm("rdx") = arg2;
    register uint64_t r10 asm("r10") = arg3;
    asm volatile(
        "int $0x80"
        : "+a"(rax), "+d"(rdx)
        : "D"(rdi), "S"(rsi), "r"(r10)
        : "rcx", "r8", "r9", "r11", "memory");
    return {rax, rdx};
}

uint64_t strlen(const char* text) {
    uint64_t length = 0;
    while (text[length] != 0) ++length;
    return length;
}

void write_text(const char* text) {
    syscall(hybrid::SyscallNumber::Write, hybrid::kStdoutFd, reinterpret_cast<uint64_t>(text), strlen(text));
}

void write_line(const char* text) {
    write_text(text);
    write_text("\n");
}

char hex_digit(uint64_t value) {
    value &= 0xf;
    return value < 10 ? static_cast<char>('0' + value) : static_cast<char>('a' + (value - 10));
}

void append_char(char* buffer, uint64_t capacity, uint64_t& cursor, char value) {
    if (cursor + 1 >= capacity) return;
    buffer[cursor++] = value;
    buffer[cursor] = 0;
}

void append_text(char* buffer, uint64_t capacity, uint64_t& cursor, const char* text) {
    for (uint64_t i = 0; text[i] != 0; ++i) append_char(buffer, capacity, cursor, text[i]);
}

void append_hex(char* buffer, uint64_t capacity, uint64_t& cursor, uint64_t value) {
    append_text(buffer, capacity, cursor, "0x");
    for (int shift = 60; shift >= 0; shift -= 4) append_char(buffer, capacity, cursor, hex_digit(value >> shift));
}

void clear_argument(hybrid::ArgumentInfo& argument) {
    for (uint64_t i = 0; i < sizeof(argument.value); ++i) argument.value[i] = 0;
}

void write_hex_line(const char* label, uint64_t value) {
    char line[96];
    uint64_t cursor = 0;
    append_text(line, sizeof(line), cursor, "[cat] ");
    append_text(line, sizeof(line), cursor, label);
    append_hex(line, sizeof(line), cursor, value);
    write_line(line);
}

void write_path_line(const char* path) {
    char line[96];
    uint64_t cursor = 0;
    append_text(line, sizeof(line), cursor, "[cat] path ");
    append_text(line, sizeof(line), cursor, path);
    write_line(line);
}

uint64_t first32(const char* bytes) {
    return static_cast<uint8_t>(bytes[0]) |
           (static_cast<uint64_t>(static_cast<uint8_t>(bytes[1])) << 8) |
           (static_cast<uint64_t>(static_cast<uint8_t>(bytes[2])) << 16) |
           (static_cast<uint64_t>(static_cast<uint8_t>(bytes[3])) << 24);
}

bool is_text_chunk(const char* bytes, uint64_t length) {
    if (length == 0) return false;
    for (uint64_t i = 0; i < length; ++i) {
        unsigned char c = static_cast<unsigned char>(bytes[i]);
        if (c == '\n' || c == '\r' || c == '\t') continue;
        if (c < 32 || c > 126) return false;
    }
    return true;
}

void write_bytes(const char* bytes, uint64_t length) {
    uint64_t written = 0;
    while (written < length) {
        auto result = syscall(hybrid::SyscallNumber::Write, hybrid::kStdoutFd, reinterpret_cast<uint64_t>(bytes + written), length - written);
        if (result.error == hybrid::kSyscallErrorWouldBlock) {
            syscall(hybrid::SyscallNumber::Yield);
            continue;
        }
        if (result.error != hybrid::kSyscallErrorNone || result.value == 0) break;
        written += result.value;
    }
}

hybrid::SyscallResult read_blocking(uint64_t fd, char* buffer, uint64_t size) {
    bool reported_wait = false;
    for (;;) {
        auto result = syscall(hybrid::SyscallNumber::Read, fd, reinterpret_cast<uint64_t>(buffer), size);
        if (result.error != hybrid::kSyscallErrorWouldBlock) return result;
        if (!reported_wait) {
            write_line("[cat] pipe read wouldblock");
            reported_wait = true;
        }
        syscall(hybrid::SyscallNumber::Yield);
    }
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo path;
    clear_argument(path);
    uint64_t fd = hybrid::kStdinFd;
    bool close_when_done = false;
    auto argc = syscall(hybrid::SyscallNumber::GetArgumentCount);
    if (argc.error != hybrid::kSyscallErrorNone || argc.value < 2 ||
        syscall(hybrid::SyscallNumber::GetArgument, 1, reinterpret_cast<uint64_t>(&path)).error != hybrid::kSyscallErrorNone) {
        write_path_line("<stdin>");
    } else {
        write_path_line(path.value);
        auto opened = syscall(hybrid::SyscallNumber::Open, reinterpret_cast<uint64_t>(path.value), strlen(path.value) + 1);
        if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
            write_hex_line("open error ", opened.error);
            syscall(hybrid::SyscallNumber::Exit, 3);
        }
        fd = opened.value;
        close_when_done = true;
    }

    char bytes[16];
    auto read = read_blocking(fd, bytes, sizeof(bytes));
    if (read.error != hybrid::kSyscallErrorNone || read.value < 4) {
        if (close_when_done) syscall(hybrid::SyscallNumber::Close, fd);
        write_hex_line("read error ", read.error);
        syscall(hybrid::SyscallNumber::Exit, 4);
    }
    uint64_t total = read.value;
    write_hex_line("bytes ", read.value);
    write_hex_line("first32 ", first32(bytes));
    if (is_text_chunk(bytes, read.value)) {
        write_line("[cat] text");
        write_bytes(bytes, read.value);
        for (;;) {
            auto next = read_blocking(fd, bytes, sizeof(bytes));
            if (next.error != hybrid::kSyscallErrorNone || next.value == 0) break;
            total += next.value;
            write_bytes(bytes, next.value);
        }
    }
    if (close_when_done) syscall(hybrid::SyscallNumber::Close, fd);
    syscall(hybrid::SyscallNumber::Exit, total);
    for (;;) asm volatile("pause");
}
