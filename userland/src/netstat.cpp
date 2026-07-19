#include "hybrid/user.hpp"

namespace {

struct Buffer {
    char bytes[1024];
    uint64_t length;
};

Buffer g_summary;
Buffer g_dev;
char g_chunk[16];

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

void emit_line(const char* prefix, const char* line, uint64_t length) {
    hybrid::user::write_text("[netstat] ");
    hybrid::user::write_text(prefix);
    hybrid::user::syscall(hybrid::SyscallNumber::Write, hybrid::kStdoutFd, reinterpret_cast<uint64_t>(line), length);
    hybrid::user::write_text("\n");
}

bool read_file(const char* path, Buffer& out) {
    out.length = 0;
    auto opened = hybrid::user::syscall(hybrid::SyscallNumber::VfsOpen, reinterpret_cast<uint64_t>(path), hybrid::user::strlen(path) + 1);
    if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
        hybrid::user::write_hex_line("[netstat] ", "open error ", opened.error);
        return false;
    }

    while (out.length < sizeof(out.bytes)) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::VfsReadHandle, opened.value, reinterpret_cast<uint64_t>(g_chunk), sizeof(g_chunk));
        if (read.error != hybrid::kSyscallErrorNone) {
            if (read.error == hybrid::kSyscallErrorNotFound && out.length != 0) break;
            hybrid::user::write_hex_line("[netstat] ", "read error ", read.error);
            hybrid::user::syscall(hybrid::SyscallNumber::VfsClose, opened.value);
            return false;
        }
        if (read.value == 0) break;
        for (uint64_t i = 0; i < read.value && out.length < sizeof(out.bytes); ++i) {
            out.bytes[out.length++] = g_chunk[i];
        }
    }

    hybrid::user::syscall(hybrid::SyscallNumber::VfsClose, opened.value);
    return out.length != 0;
}

void process_summary_line(const char* line, uint64_t length, uint64_t& emitted) {
    const char* prefixes[] = {
        "interfaces ",
        "e1000_present ",
        "e1000_link ",
        "e1000_speed_mbps ",
        "e1000_full_duplex ",
        "e1000_mac_valid ",
        "e1000_mmio_mapped ",
        "e1000_command_enabled ",
        "e1000_rings_programmed ",
        "e1000_ring_registers_verified ",
        "e1000_tx_packets ",
        "e1000_tx_completed ",
        "e1000_rx_packets ",
        "e1000_rx_bytes ",
        "e1000_rx_empty_polls ",
    };

    for (uint64_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        if (starts_with(line, length, prefixes[i])) {
            emit_line("summary ", line, length);
            ++emitted;
            return;
        }
    }
}

void process_dev_line(const char* line, uint64_t length, uint64_t& emitted) {
    if (starts_with(line, length, "IFACE ") || starts_with(line, length, "eth0 ")) {
        emit_line("dev ", line, length);
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
    bool summary_ok = read_file("/proc/net/summary", g_summary);
    bool dev_ok = read_file("/proc/net/dev", g_dev);
    if (!summary_ok || !dev_ok) {
        hybrid::user::write_hex_line("[netstat] ", "summary ok ", summary_ok ? 1 : 0);
        hybrid::user::write_hex_line("[netstat] ", "dev ok ", dev_ok ? 1 : 0);
        hybrid::user::exit(1);
    }

    uint64_t summary_lines = for_each_line(g_summary, process_summary_line);
    uint64_t dev_lines = for_each_line(g_dev, process_dev_line);
    hybrid::user::write_hex_line("[netstat] ", "summary lines ", summary_lines);
    hybrid::user::write_hex_line("[netstat] ", "dev lines ", dev_lines);
    hybrid::user::exit(summary_lines != 0 && dev_lines != 0 ? 0 : 2);
}
