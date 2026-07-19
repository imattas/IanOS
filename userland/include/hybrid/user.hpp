#pragma once

#include "hybrid/syscall.hpp"

namespace hybrid::user {

inline SyscallResult syscall(SyscallNumber number, uint64_t arg0 = 0, uint64_t arg1 = 0, uint64_t arg2 = 0, uint64_t arg3 = 0) {
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

inline uint64_t strlen(const char* text) {
    uint64_t length = 0;
    while (text[length] != 0) ++length;
    return length;
}

inline char hex_digit(uint64_t value) {
    value &= 0xf;
    return value < 10 ? static_cast<char>('0' + value) : static_cast<char>('a' + (value - 10));
}

inline void append_char(char* buffer, uint64_t capacity, uint64_t& cursor, char value) {
    if (cursor + 1 >= capacity) return;
    buffer[cursor++] = value;
    buffer[cursor] = 0;
}

inline void append_text(char* buffer, uint64_t capacity, uint64_t& cursor, const char* text) {
    for (uint64_t i = 0; text[i] != 0; ++i) append_char(buffer, capacity, cursor, text[i]);
}

inline void append_hex(char* buffer, uint64_t capacity, uint64_t& cursor, uint64_t value) {
    append_text(buffer, capacity, cursor, "0x");
    for (int shift = 60; shift >= 0; shift -= 4) append_char(buffer, capacity, cursor, hex_digit(value >> shift));
}

inline void write_fd(uint32_t fd, const char* text) {
    uint64_t length = strlen(text);
    uint64_t written = 0;
    while (written < length) {
        auto result = syscall(SyscallNumber::Write, fd, reinterpret_cast<uint64_t>(text + written), length - written);
        if (result.error == kSyscallErrorWouldBlock) {
            syscall(SyscallNumber::Yield);
            continue;
        }
        if (result.error != kSyscallErrorNone || result.value == 0) break;
        written += result.value;
    }
}

inline void write_text(const char* text) {
    write_fd(kStdoutFd, text);
}

inline void write_error(const char* text) {
    write_fd(kStderrFd, text);
}

inline void write_line(const char* text) {
    write_text(text);
    write_text("\n");
}

inline void write_hex_line(const char* prefix, const char* label, uint64_t value) {
    char line[128];
    uint64_t cursor = 0;
    append_text(line, sizeof(line), cursor, prefix);
    append_text(line, sizeof(line), cursor, label);
    append_hex(line, sizeof(line), cursor, value);
    write_line(line);
}

inline void write_text_line(const char* prefix, const char* label, const char* value) {
    char line[128];
    uint64_t cursor = 0;
    append_text(line, sizeof(line), cursor, prefix);
    append_text(line, sizeof(line), cursor, label);
    append_text(line, sizeof(line), cursor, value);
    write_line(line);
}

[[noreturn]] inline void exit(uint64_t code) {
    syscall(SyscallNumber::Exit, code);
    for (;;) asm volatile("pause");
}

} // namespace hybrid::user
