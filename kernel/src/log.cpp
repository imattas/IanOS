#include "hk/log.hpp"
#include "hk/console.hpp"
#include "hk/sync/spinlock.hpp"

namespace {
hk::sync::SpinLock log_lock;
constexpr uint64_t kKernelLogCapacity = 8192;
char kernel_log[kKernelLogCapacity]{};
uint64_t kernel_log_head = 0;
uint64_t kernel_log_size_bytes = 0;
uint64_t kernel_log_total_bytes = 0;
bool console_log_output_enabled = true;

void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

uint8_t inb(uint16_t port) {
    uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

const char* prefix(hk::LogLevel level) {
    switch (level) {
    case hk::LogLevel::Debug: return "[DEBUG] ";
    case hk::LogLevel::Info: return "[INFO ] ";
    case hk::LogLevel::Warn: return "[WARN ] ";
    case hk::LogLevel::Error: return "[ERROR] ";
    }
    return "[???? ] ";
}

char hex_digit(uint64_t value) {
    value &= 0xf;
    return value < 10 ? static_cast<char>('0' + value) : static_cast<char>('a' + (value - 10));
}

void append_kernel_log_char(char c) {
    kernel_log[kernel_log_head] = c;
    kernel_log_head = (kernel_log_head + 1) % kKernelLogCapacity;
    if (kernel_log_size_bytes < kKernelLogCapacity) ++kernel_log_size_bytes;
    ++kernel_log_total_bytes;
}

void append_kernel_log_text(const char* text) {
    if (!text) return;
    while (*text) append_kernel_log_char(*text++);
}

void append_kernel_log_hex(uint64_t value) {
    append_kernel_log_text("0x");
    for (int i = 60; i >= 0; i -= 4) append_kernel_log_char(hex_digit(value >> i));
}

void write_hex_digits(uint64_t value) {
    hk::serial_write("0x");
    for (int i = 60; i >= 0; i -= 4) {
        char c[2] = {hex_digit(value >> i), 0};
        hk::serial_write(c);
    }
}
}

namespace hk {

void serial_initialize() {
    constexpr uint16_t com1 = 0x3f8;
    outb(com1 + 1, 0x00);
    outb(com1 + 3, 0x80);
    outb(com1 + 0, 0x03);
    outb(com1 + 1, 0x00);
    outb(com1 + 3, 0x03);
    outb(com1 + 2, 0xC7);
    outb(com1 + 4, 0x0B);
}

void serial_write(const char* text) {
    constexpr uint16_t com1 = 0x3f8;
    while (*text) {
        char c = *text++;
        if (c == '\n') {
            while ((inb(com1 + 5) & 0x20) == 0) {}
            outb(com1, '\r');
        }
        while ((inb(com1 + 5) & 0x20) == 0) {}
        outb(com1, static_cast<uint8_t>(c));
    }
}

void set_console_log_enabled(bool enabled) {
    uint64_t flags = hk::sync::irq_save();
    {
        hk::sync::LockGuard guard(log_lock);
        console_log_output_enabled = enabled;
    }
    hk::sync::irq_restore(flags);
}

bool console_log_enabled() {
    uint64_t flags = hk::sync::irq_save();
    bool enabled = false;
    {
        hk::sync::LockGuard guard(log_lock);
        enabled = console_log_output_enabled;
    }
    hk::sync::irq_restore(flags);
    return enabled;
}

void log(LogLevel level, const char* text) {
    uint64_t flags = hk::sync::irq_save();
    {
        hk::sync::LockGuard guard(log_lock);
        serial_write(prefix(level));
        serial_write(text);
        serial_write("\n");
        append_kernel_log_text(prefix(level));
        append_kernel_log_text(text);
        append_kernel_log_char('\n');
        if (console_log_output_enabled) {
            console().write(prefix(level));
            console().write(text);
            console().write("\n");
        }
    }
    hk::sync::irq_restore(flags);
}

void log_hex(LogLevel level, const char* label, uint64_t value) {
    uint64_t flags = hk::sync::irq_save();
    {
        hk::sync::LockGuard guard(log_lock);
        serial_write(prefix(level));
        serial_write(label);
        serial_write("=");
        write_hex_digits(value);
        serial_write("\n");
        append_kernel_log_text(prefix(level));
        append_kernel_log_text(label);
        append_kernel_log_char('=');
        append_kernel_log_hex(value);
        append_kernel_log_char('\n');
        if (console_log_output_enabled) {
            console().write(prefix(level));
            console().write(label);
            console().write("=");
            console().write_hex(value);
            console().write("\n");
        }
    }
    hk::sync::irq_restore(flags);
}

uint64_t kernel_log_size() {
    uint64_t flags = hk::sync::irq_save();
    uint64_t size = 0;
    {
        hk::sync::LockGuard guard(log_lock);
        size = kernel_log_size_bytes;
    }
    hk::sync::irq_restore(flags);
    return size;
}

uint64_t copy_kernel_log(char* out, uint64_t capacity, uint64_t offset) {
    if (!out || capacity == 0 || capacity > kKernelLogCapacity) return 0;
    uint64_t flags = hk::sync::irq_save();
    uint64_t to_copy = 0;
    {
        hk::sync::LockGuard guard(log_lock);
        if (offset < kernel_log_size_bytes) {
            uint64_t available = kernel_log_size_bytes - offset;
            to_copy = available < capacity ? available : capacity;
            uint64_t start = (kernel_log_head + kKernelLogCapacity - kernel_log_size_bytes + offset) % kKernelLogCapacity;
            for (uint64_t i = 0; i < to_copy; ++i) out[i] = kernel_log[(start + i) % kKernelLogCapacity];
        }
    }
    hk::sync::irq_restore(flags);
    return to_copy;
}

[[noreturn]] void panic(const char* reason, const char* file, int line) {
    asm volatile("cli");
    serial_write("[PANIC] ");
    serial_write(reason);
    serial_write(" at ");
    serial_write(file);
    serial_write(":");
    write_hex_digits(static_cast<uint64_t>(line));
    serial_write("\n");
    console().write("[PANIC] ");
    console().write(reason);
    console().write("\n");
    console().write(file);
    console().write(":");
    console().write_hex(static_cast<uint64_t>(line));
    console().write("\n");
    for (;;) asm volatile("hlt");
}

} // namespace hk
