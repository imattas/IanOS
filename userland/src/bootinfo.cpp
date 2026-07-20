#include "hybrid/user.hpp"

namespace {

struct Buffer {
    char bytes[1536];
    uint64_t length;
};

Buffer g_buffer;
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

bool read_bootinfo(Buffer& out) {
    out.length = 0;
    static const char path[] = "/proc/bootinfo";
    auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open,
                                        reinterpret_cast<uint64_t>(path),
                                        sizeof(path));
    if (opened.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[bootinfo] ", "open error ", opened.error);
        return false;
    }

    while (out.length < sizeof(out.bytes)) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::Read,
                                          opened.value,
                                          reinterpret_cast<uint64_t>(g_chunk),
                                          sizeof(g_chunk));
        if (read.error == hybrid::kSyscallErrorNotFound && out.length != 0) break;
        if (read.error != hybrid::kSyscallErrorNone) {
            hybrid::user::write_hex_line("[bootinfo] ", "read error ", read.error);
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

void emit_line(const char* line, uint64_t length) {
    hybrid::user::write_text("[bootinfo] ");
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

uint64_t emit_selected(const Buffer& buffer) {
    uint64_t emitted = 0;
    for_each_line(buffer, [&](const char* line, uint64_t length) {
        const char* keys[] = {
            "magic ",
            "version ",
            "size ",
            "flags ",
            "rsdp ",
            "memory_map_entries ",
            "memory_map_descriptor_size ",
            "kernel_physical_base ",
            "kernel_physical_end ",
            "kernel_entry ",
            "user_init_base ",
            "user_init_size ",
            "boot_module_count ",
            "hhdm_offset ",
            "framebuffer_width ",
            "framebuffer_height ",
            "framebuffer_scanline ",
            "framebuffer_bytes_per_pixel ",
        };
        for (uint64_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
            if (!starts_with(line, length, keys[i])) continue;
            emit_line(line, length);
            ++emitted;
            return;
        }
    });
    return emitted;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    if (!read_bootinfo(g_buffer)) hybrid::user::exit(1);
    const uint64_t lines = emit_selected(g_buffer);
    hybrid::user::write_hex_line("[bootinfo] ", "lines ", lines);
    hybrid::user::exit(lines >= 12 ? 0 : 2);
}
