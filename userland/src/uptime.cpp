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

void write_line(const char* text) {
    syscall(hybrid::SyscallNumber::Write, hybrid::kStdoutFd, reinterpret_cast<uint64_t>(text), strlen(text));
    static const char newline[] = "\n";
    syscall(hybrid::SyscallNumber::Write, hybrid::kStdoutFd, reinterpret_cast<uint64_t>(newline), 1);
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

} // namespace

extern "C" [[noreturn]] void _start() {
    auto ticks = syscall(hybrid::SyscallNumber::GetTicks);
    if (ticks.error != hybrid::kSyscallErrorNone) {
        write_line("[uptime] ticks error");
        syscall(hybrid::SyscallNumber::Exit, 1);
    }
    char line[80];
    uint64_t cursor = 0;
    append_text(line, sizeof(line), cursor, "[uptime] ticks ");
    append_hex(line, sizeof(line), cursor, ticks.value);
    write_line(line);
    syscall(hybrid::SyscallNumber::Exit, 0);
    for (;;) asm volatile("pause");
}
