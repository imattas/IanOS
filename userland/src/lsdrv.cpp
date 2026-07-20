#include "hybrid/user.hpp"

namespace {

struct Buffer {
    char bytes[1536];
    uint64_t length;
};

Buffer g_summary;
Buffer g_devices;
char g_chunk[32];

bool same_text(const char* left, const char* right, uint64_t count) {
    for (uint64_t i = 0; i < count; ++i) {
        if (left[i] != right[i]) return false;
    }
    return true;
}

bool starts_with(const char* line, uint64_t length, const char* prefix) {
    const uint64_t prefix_length = hybrid::user::strlen(prefix);
    return prefix_length <= length && same_text(line, prefix, prefix_length);
}

bool read_file(const char* path, Buffer& out) {
    out.length = 0;
    auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open,
                                        reinterpret_cast<uint64_t>(path),
                                        hybrid::user::strlen(path) + 1);
    if (opened.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[lsdrv] ", "open error ", opened.error);
        return false;
    }

    while (out.length < sizeof(out.bytes)) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::Read,
                                          opened.value,
                                          reinterpret_cast<uint64_t>(g_chunk),
                                          sizeof(g_chunk));
        if (read.error == hybrid::kSyscallErrorNotFound && out.length != 0) break;
        if (read.error != hybrid::kSyscallErrorNone) {
            hybrid::user::write_hex_line("[lsdrv] ", "read error ", read.error);
            hybrid::user::syscall(hybrid::SyscallNumber::Close, opened.value);
            return false;
        }
        if (read.value == 0) break;
        for (uint64_t i = 0; i < read.value && out.length < sizeof(out.bytes); ++i) {
            out.bytes[out.length++] = g_chunk[i];
        }
    }

    hybrid::user::syscall(hybrid::SyscallNumber::Close, opened.value);
    return out.length != 0;
}

void emit_prefixed(const char* line, uint64_t length) {
    hybrid::user::write_text("[lsdrv] ");
    hybrid::user::syscall(hybrid::SyscallNumber::Write,
                          hybrid::kStdoutFd,
                          reinterpret_cast<uint64_t>(line),
                          length);
    hybrid::user::write_text("\n");
}

template <typename Fn>
void for_each_line(const Buffer& buffer, Fn fn) {
    uint64_t start = 0;
    for (uint64_t i = 0; i <= buffer.length; ++i) {
        if (i != buffer.length && buffer.bytes[i] != '\n') continue;
        uint64_t length = i - start;
        if (length != 0 && buffer.bytes[start + length - 1] == '\r') --length;
        if (length != 0) fn(buffer.bytes + start, length);
        start = i + 1;
    }
}

uint64_t emit_summary(const Buffer& buffer) {
    uint64_t emitted = 0;
    for_each_line(buffer, [&](const char* line, uint64_t length) {
        const char* keys[] = {
            "registered_drivers ",
            "started_drivers ",
            "failed_drivers ",
            "start_attempts ",
            "start_successes ",
            "start_failures ",
            "imported_devices ",
            "ahci_devices ",
            "e1000_devices ",
            "vga_devices ",
            "bus_master_required_devices ",
            "command_bits_union ",
        };
        for (uint64_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
            if (!starts_with(line, length, keys[i])) continue;
            emit_prefixed(line, length);
            ++emitted;
            return;
        }
    });
    return emitted;
}

uint64_t emit_devices(const Buffer& buffer) {
    uint64_t emitted = 0;
    for_each_line(buffer, [&](const char* line, uint64_t length) {
        if (!starts_with(line, length, "driver ")) return;
        emit_prefixed(line, length);
        ++emitted;
    });
    return emitted;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    if (!read_file("/proc/driver/summary", g_summary)) hybrid::user::exit(1);
    if (!read_file("/proc/driver/devices", g_devices)) hybrid::user::exit(2);

    hybrid::user::write_line("[lsdrv] NAME KIND BDF VENDOR DEVICE REQUIRED_COMMAND_BITS BUS_MASTER STATE");
    const uint64_t summary_lines = emit_summary(g_summary);
    const uint64_t device_lines = emit_devices(g_devices);
    hybrid::user::write_hex_line("[lsdrv] ", "summary lines ", summary_lines);
    hybrid::user::write_hex_line("[lsdrv] ", "device lines ", device_lines);
    hybrid::user::exit(summary_lines != 0 && device_lines != 0 ? 0 : 3);
}
