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

void write_arg(uint64_t index, const char* value) {
    char line[96];
    uint64_t cursor = 0;
    append_text(line, sizeof(line), cursor, "[args] argv[");
    append_hex(line, sizeof(line), cursor, index);
    append_text(line, sizeof(line), cursor, "]=");
    append_text(line, sizeof(line), cursor, value);
    write_line(line);
}

void write_count(uint64_t count) {
    char line[64];
    uint64_t cursor = 0;
    append_text(line, sizeof(line), cursor, "[args] argc ");
    append_hex(line, sizeof(line), cursor, count);
    write_line(line);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    auto count = syscall(hybrid::SyscallNumber::GetArgumentCount);
    if (count.error != hybrid::kSyscallErrorNone) {
        write_line("[args] argc error");
        syscall(hybrid::SyscallNumber::Exit, 1);
    }
    write_count(count.value);
    for (uint64_t i = 0; i < count.value && i < 4; ++i) {
        hybrid::ArgumentInfo argument;
        clear_argument(argument);
        auto result = syscall(hybrid::SyscallNumber::GetArgument, i, reinterpret_cast<uint64_t>(&argument));
        if (result.error == hybrid::kSyscallErrorNone) write_arg(i, argument.value);
    }
    syscall(hybrid::SyscallNumber::Exit, count.value);
    for (;;) asm volatile("pause");
}
