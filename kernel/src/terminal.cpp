#include "hk/terminal.hpp"
#include "hk/console.hpp"
#include "hk/log.hpp"
#include "hk/sync/spinlock.hpp"

namespace {
hk::sync::SpinLock terminal_lock;

constexpr uint32_t kInputBufferSize = 256;
constexpr uint32_t kLineBufferSize = 128;

char raw_buffer[kInputBufferSize]{};
uint32_t raw_head = 0;
uint32_t raw_tail = 0;
uint32_t raw_count = 0;

char cooked_buffer[kInputBufferSize]{};
uint32_t cooked_head = 0;
uint32_t cooked_tail = 0;
uint32_t cooked_count = 0;

char line_buffer[kLineBufferSize]{};
uint32_t line_length = 0;
hybrid::TerminalInputMode mode = hybrid::TerminalInputMode::Raw;
hk::terminal::Stats terminal_stats{};

void clear_queue(char* buffer, uint32_t& head, uint32_t& tail, uint32_t& count) {
    for (uint32_t i = 0; i < kInputBufferSize; ++i) buffer[i] = 0;
    head = 0;
    tail = 0;
    count = 0;
}

void clear_input_locked() {
    clear_queue(raw_buffer, raw_head, raw_tail, raw_count);
    clear_queue(cooked_buffer, cooked_head, cooked_tail, cooked_count);
    for (uint32_t i = 0; i < kLineBufferSize; ++i) line_buffer[i] = 0;
    line_length = 0;
}

void update_high_watermark() {
    uint32_t active = raw_count > cooked_count ? raw_count : cooked_count;
    if (active > terminal_stats.input_high_watermark) terminal_stats.input_high_watermark = active;
}

void push_queue(char* buffer, uint32_t& head, uint32_t& tail, uint32_t& count, char c) {
    if (count == kInputBufferSize) {
        tail = (tail + 1) % kInputBufferSize;
        --count;
        ++terminal_stats.dropped_input_bytes;
    }
    buffer[head] = c;
    head = (head + 1) % kInputBufferSize;
    ++count;
    update_high_watermark();
}

bool pop_queue(char* buffer, uint32_t& tail, uint32_t& count, char& out) {
    if (count == 0) return false;
    out = buffer[tail];
    tail = (tail + 1) % kInputBufferSize;
    --count;
    return true;
}

void push_cooked(char c) {
    push_queue(cooked_buffer, cooked_head, cooked_tail, cooked_count, c);
    ++terminal_stats.canonical_input_bytes;
}

void canonical_input(char c) {
    if (c == 0) return;
    if (c == '\r') c = '\n';
    if (c == '\b' || c == 0x7f) {
        if (line_length != 0) {
            --line_length;
            line_buffer[line_length] = 0;
        }
        return;
    }
    if (c == '\n') {
        for (uint32_t i = 0; i < line_length; ++i) push_cooked(line_buffer[i]);
        push_cooked('\n');
        ++terminal_stats.canonical_line_commits;
        for (uint32_t i = 0; i < kLineBufferSize; ++i) line_buffer[i] = 0;
        line_length = 0;
        return;
    }
    if (c >= 1 && c < 32) {
        push_cooked(c);
        return;
    }
    if (line_length + 1 < kLineBufferSize) {
        line_buffer[line_length++] = c;
        line_buffer[line_length] = 0;
    }
}
}

namespace hk::terminal {

size_t write(const char* data, size_t length) {
    if (!data || length == 0 || length > 4096) return 0;
    uint64_t flags = hk::sync::irq_save();
    {
        hk::sync::LockGuard guard(terminal_lock);
        ++terminal_stats.write_calls;
        terminal_stats.bytes_written += length;
        for (size_t i = 0; i < length; ++i) {
            char text[2] = {data[i], 0};
            hk::serial_write(text);
            hk::console().write(text);
        }
    }
    hk::sync::irq_restore(flags);
    return length;
}

void push_input(char c) {
    if (c == 0) return;
    uint64_t flags = hk::sync::irq_save();
    {
        hk::sync::LockGuard guard(terminal_lock);
        if (mode == hybrid::TerminalInputMode::Raw) {
            push_queue(raw_buffer, raw_head, raw_tail, raw_count, c);
            ++terminal_stats.raw_input_bytes;
        } else {
            canonical_input(c);
        }
    }
    hk::sync::irq_restore(flags);
}

size_t inject_input(const char* data, size_t length) {
    if (!data || length == 0 || length > 4096) return 0;
    uint64_t flags = hk::sync::irq_save();
    size_t injected = 0;
    {
        hk::sync::LockGuard guard(terminal_lock);
        for (size_t i = 0; i < length; ++i) {
            char c = data[i];
            if (c == 0) break;
            if (mode == hybrid::TerminalInputMode::Raw) {
                push_queue(raw_buffer, raw_head, raw_tail, raw_count, c);
                ++terminal_stats.raw_input_bytes;
            } else {
                canonical_input(c);
            }
            ++injected;
        }
    }
    hk::sync::irq_restore(flags);
    return injected;
}

size_t read_input(char* buffer, size_t length) {
    if (!buffer || length == 0 || length > 4096) return 0;
    uint64_t flags = hk::sync::irq_save();
    size_t read = 0;
    {
        hk::sync::LockGuard guard(terminal_lock);
        char* source = mode == hybrid::TerminalInputMode::Raw ? raw_buffer : cooked_buffer;
        uint32_t& tail = mode == hybrid::TerminalInputMode::Raw ? raw_tail : cooked_tail;
        uint32_t& count = mode == hybrid::TerminalInputMode::Raw ? raw_count : cooked_count;
        while (read < length) {
            char c = 0;
            if (!pop_queue(source, tail, count, c)) break;
            buffer[read++] = c;
        }
    }
    hk::sync::irq_restore(flags);
    return read;
}

size_t read_key(char* buffer, size_t length) {
    if (!buffer || length == 0) return 0;
    uint64_t flags = hk::sync::irq_save();
    size_t read = 0;
    {
        hk::sync::LockGuard guard(terminal_lock);
        while (read < length) {
            char c = 0;
            if (!pop_queue(raw_buffer, raw_tail, raw_count, c)) break;
            buffer[read++] = c;
        }
    }
    hk::sync::irq_restore(flags);
    return read;
}

uint64_t buffered_input_count() {
    uint64_t flags = hk::sync::irq_save();
    uint64_t count = 0;
    {
        hk::sync::LockGuard guard(terminal_lock);
        count = mode == hybrid::TerminalInputMode::Raw ? raw_count : cooked_count;
    }
    hk::sync::irq_restore(flags);
    return count;
}

void set_input_mode(hybrid::TerminalInputMode next) {
    uint64_t flags = hk::sync::irq_save();
    {
        hk::sync::LockGuard guard(terminal_lock);
        mode = next == hybrid::TerminalInputMode::Canonical ? next : hybrid::TerminalInputMode::Raw;
        clear_input_locked();
    }
    hk::sync::irq_restore(flags);
}

hybrid::TerminalInputMode input_mode() {
    uint64_t flags = hk::sync::irq_save();
    hybrid::TerminalInputMode current;
    {
        hk::sync::LockGuard guard(terminal_lock);
        current = mode;
    }
    hk::sync::irq_restore(flags);
    return current;
}

void scroll_relative(int64_t rows) {
    uint64_t flags = hk::sync::irq_save();
    {
        hk::sync::LockGuard guard(terminal_lock);
        ++terminal_stats.scroll_relative_ops;
        hk::console().scroll_view(rows);
    }
    hk::sync::irq_restore(flags);
}

void scroll_to_bottom() {
    uint64_t flags = hk::sync::irq_save();
    {
        hk::sync::LockGuard guard(terminal_lock);
        ++terminal_stats.scroll_to_bottom_ops;
        hk::console().scroll_to_bottom();
    }
    hk::sync::irq_restore(flags);
}

void reset_input_line() {
    uint64_t flags = hk::sync::irq_save();
    {
        hk::sync::LockGuard guard(terminal_lock);
        ++terminal_stats.input_line_resets;
        hk::serial_write("\r\x1b[2K");
        hk::console().reset_input_line();
    }
    hk::sync::irq_restore(flags);
}

Stats stats() {
    uint64_t flags = hk::sync::irq_save();
    Stats snapshot{};
    {
        hk::sync::LockGuard guard(terminal_lock);
        snapshot = terminal_stats;
    }
    hk::sync::irq_restore(flags);
    return snapshot;
}

bool self_test() {
    hybrid::TerminalInputMode saved = input_mode();
    Stats before = stats();
    set_input_mode(hybrid::TerminalInputMode::Canonical);
    static const char canonical_input_text[] = "ab\bc\n";
    if (inject_input(canonical_input_text, sizeof(canonical_input_text) - 1) != sizeof(canonical_input_text) - 1) {
        set_input_mode(saved);
        return false;
    }
    char canonical[4]{};
    if (read_input(canonical, sizeof(canonical)) != 3 ||
        canonical[0] != 'a' || canonical[1] != 'c' || canonical[2] != '\n') {
        set_input_mode(saved);
        return false;
    }
    hk::log(hk::LogLevel::Info, "Terminal canonical input self-test");
    set_input_mode(hybrid::TerminalInputMode::Raw);
    static const char raw_input_text[] = {0x10, 'x'};
    if (inject_input(raw_input_text, sizeof(raw_input_text)) != sizeof(raw_input_text)) {
        set_input_mode(saved);
        return false;
    }
    char raw[2]{};
    if (read_input(raw, sizeof(raw)) != 2 || raw[0] != 0x10 || raw[1] != 'x') {
        set_input_mode(saved);
        return false;
    }
    hk::log(hk::LogLevel::Info, "Terminal raw input self-test");
    static char overflow_input[300];
    for (uint32_t i = 0; i < sizeof(overflow_input); ++i) overflow_input[i] = 'r';
    if (inject_input(overflow_input, sizeof(overflow_input)) != sizeof(overflow_input)) {
        set_input_mode(saved);
        return false;
    }
    if (buffered_input_count() != kInputBufferSize) {
        set_input_mode(saved);
        return false;
    }
    char overflow_drain[kInputBufferSize]{};
    if (read_input(overflow_drain, sizeof(overflow_drain)) != sizeof(overflow_drain)) {
        set_input_mode(saved);
        return false;
    }
    hk::log(hk::LogLevel::Info, "Terminal raw overflow self-test");
    scroll_relative(-1);
    scroll_to_bottom();
    reset_input_line();
    if (hk::console_log_enabled()) write("[terminal] reset line ok", sizeof("[terminal] reset line ok") - 1);
    Stats after = stats();
    if (after.canonical_line_commits <= before.canonical_line_commits ||
        after.raw_input_bytes < before.raw_input_bytes + sizeof(raw_input_text) + sizeof(overflow_input) ||
        after.dropped_input_bytes <= before.dropped_input_bytes ||
        after.input_high_watermark < kInputBufferSize ||
        after.scroll_relative_ops <= before.scroll_relative_ops ||
        after.scroll_to_bottom_ops <= before.scroll_to_bottom_ops ||
        after.input_line_resets <= before.input_line_resets) {
        set_input_mode(saved);
        return false;
    }
    hk::log_hex(hk::LogLevel::Info, "Terminal dropped input bytes", after.dropped_input_bytes);
    hk::log_hex(hk::LogLevel::Info, "Terminal input high watermark", after.input_high_watermark);
    hk::log_hex(hk::LogLevel::Info, "Terminal scroll relative ops", after.scroll_relative_ops);
    hk::log_hex(hk::LogLevel::Info, "Terminal scroll bottom ops", after.scroll_to_bottom_ops);
    hk::log_hex(hk::LogLevel::Info, "Terminal input line resets", after.input_line_resets);
    set_input_mode(saved);
    return true;
}

}
