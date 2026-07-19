#include "hybrid/syscall.hpp"

namespace {

hybrid::SyscallResult syscall(hybrid::SyscallNumber number, uint64_t arg0 = 0, uint64_t arg1 = 0, uint64_t arg2 = 0, uint64_t arg3 = 0) {
    register uint64_t rax asm("rax") = static_cast<uint64_t>(number);
    register uint64_t rdi asm("rdi") = arg0;
    register uint64_t rsi asm("rsi") = arg1;
    register uint64_t rdx asm("rdx") = arg2;
    register uint64_t r10 asm("r10") = arg3;
    asm volatile("int $0x80"
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

void write_hex_line(const char* label, uint64_t value) {
    char line[96];
    uint64_t cursor = 0;
    append_text(line, sizeof(line), cursor, "[kmsg] ");
    append_text(line, sizeof(line), cursor, label);
    append_hex(line, sizeof(line), cursor, value);
    append_char(line, sizeof(line), cursor, '\n');
    write_text(line);
}

uint64_t count_lines(const char* bytes, uint64_t length) {
    uint64_t lines = 0;
    for (uint64_t i = 0; i < length; ++i) {
        if (bytes[i] == '\n') ++lines;
    }
    return lines;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    const char* path = "/proc/kmsg";
    auto opened = syscall(hybrid::SyscallNumber::VfsOpen, reinterpret_cast<uint64_t>(path), strlen(path) + 1);
    if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
        write_hex_line("open error ", opened.error);
        syscall(hybrid::SyscallNumber::Exit, 1);
    }

    char buffer[2048];
    uint64_t total = 0;
    uint64_t lines = 0;
    for (;;) {
        auto read = syscall(hybrid::SyscallNumber::VfsReadHandle, opened.value, reinterpret_cast<uint64_t>(buffer), sizeof(buffer));
        if (read.error != hybrid::kSyscallErrorNone) {
            if (read.error == hybrid::kSyscallErrorNotFound && total != 0) break;
            write_hex_line("read error ", read.error);
            syscall(hybrid::SyscallNumber::VfsClose, opened.value);
            syscall(hybrid::SyscallNumber::Exit, 2);
        }
        if (read.value == 0) break;
        total += read.value;
        lines += count_lines(buffer, read.value);
        write_bytes(buffer, read.value);
    }
    syscall(hybrid::SyscallNumber::VfsClose, opened.value);
    write_hex_line("bytes ", total);
    write_hex_line("lines ", lines);
    syscall(hybrid::SyscallNumber::Exit, lines == 0 ? 3 : 0);
    for (;;) asm volatile("pause");
}
