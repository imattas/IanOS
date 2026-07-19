#include "hybrid/user.hpp"

namespace {

struct Buffer {
    char bytes[1536];
    uint64_t length;
};

Buffer g_summary;
Buffer g_interrupts;
char g_read_chunk[16];

bool same_text(const char* left, const char* right, uint64_t count) {
    for (uint64_t i = 0; i < count; ++i) {
        if (left[i] != right[i]) return false;
    }
    return true;
}

bool starts_with(const char* line, uint64_t length, const char* prefix) {
    uint64_t prefix_len = hybrid::user::strlen(prefix);
    return prefix_len <= length && same_text(line, prefix, prefix_len);
}

void emit_prefixed_line(const char* prefix, const char* line, uint64_t length) {
    hybrid::user::write_text("[irqstat] ");
    hybrid::user::write_text(prefix);
    hybrid::user::syscall(hybrid::SyscallNumber::Write, hybrid::kStdoutFd, reinterpret_cast<uint64_t>(line), length);
    hybrid::user::write_text("\n");
}

bool read_file(const char* path, Buffer& out) {
    out.length = 0;
    auto opened = hybrid::user::syscall(hybrid::SyscallNumber::VfsOpen, reinterpret_cast<uint64_t>(path), hybrid::user::strlen(path) + 1);
    if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
        hybrid::user::write_hex_line("[irqstat] ", "open error ", opened.error);
        return false;
    }

    while (out.length < sizeof(out.bytes)) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::VfsReadHandle, opened.value, reinterpret_cast<uint64_t>(g_read_chunk), sizeof(g_read_chunk));
        if (read.error != hybrid::kSyscallErrorNone) {
            if (read.error == hybrid::kSyscallErrorNotFound && out.length != 0) break;
            hybrid::user::write_hex_line("[irqstat] ", "read error ", read.error);
            hybrid::user::syscall(hybrid::SyscallNumber::VfsClose, opened.value);
            return false;
        }
        if (read.value == 0) break;
        for (uint64_t i = 0; i < read.value && out.length < sizeof(out.bytes); ++i) {
            out.bytes[out.length++] = g_read_chunk[i];
        }
    }

    hybrid::user::syscall(hybrid::SyscallNumber::VfsClose, opened.value);
    return out.length != 0;
}

void process_summary_line(const char* line, uint64_t length, uint64_t& emitted) {
    const char* prefixes[] = {
        "dispatch_count ",
        "exception_dispatch_count ",
        "irq_dispatch_count ",
        "lapic_dispatch_count ",
        "syscall_dispatch_count ",
        "vector_0x20_count ",
        "vector_0x21_count ",
        "vector_0x40_count ",
        "vector_0x41_count ",
        "vector_0x80_count ",
        "lapic_ticks ",
        "user_preempt_switches ",
    };

    for (uint64_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        if (starts_with(line, length, prefixes[i])) {
            emit_prefixed_line("summary ", line, length);
            ++emitted;
            return;
        }
    }
}

void process_interrupt_line(const char* line, uint64_t length, uint64_t& emitted) {
    if (starts_with(line, length, "VECTOR ") ||
        starts_with(line, length, "32: ") ||
        starts_with(line, length, "33: ") ||
        starts_with(line, length, "64: ") ||
        starts_with(line, length, "65: ") ||
        starts_with(line, length, "128: ") ||
        starts_with(line, length, "ERR: ") ||
        starts_with(line, length, "MIS: ") ||
        starts_with(line, length, "EOI: ")) {
        emit_prefixed_line("interrupt ", line, length);
        ++emitted;
    }
}

template <typename Fn>
uint64_t for_each_line(const Buffer& buffer, Fn fn) {
    uint64_t emitted = 0;
    uint64_t start = 0;
    for (uint64_t i = 0; i <= buffer.length; ++i) {
        if (i != buffer.length && buffer.bytes[i] != '\n') continue;
        uint64_t length = i - start;
        if (length != 0 && buffer.bytes[start + length - 1] == '\r') --length;
        if (length != 0) fn(buffer.bytes + start, length, emitted);
        start = i + 1;
    }
    return emitted;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    bool summary_ok = read_file("/proc/irq/summary", g_summary);
    bool interrupts_ok = read_file("/proc/interrupts", g_interrupts);
    if (!summary_ok || !interrupts_ok) {
        hybrid::user::write_hex_line("[irqstat] ", "summary ok ", summary_ok ? 1 : 0);
        hybrid::user::write_hex_line("[irqstat] ", "interrupts ok ", interrupts_ok ? 1 : 0);
        hybrid::user::exit(1);
    }

    uint64_t summary_lines = for_each_line(g_summary, process_summary_line);
    uint64_t interrupt_lines = for_each_line(g_interrupts, process_interrupt_line);
    hybrid::user::write_hex_line("[irqstat] ", "summary lines ", summary_lines);
    hybrid::user::write_hex_line("[irqstat] ", "interrupt lines ", interrupt_lines);
    hybrid::user::exit(summary_lines != 0 && interrupt_lines != 0 ? 0 : 2);
}
