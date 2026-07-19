#include "hybrid/user.hpp"

namespace {

struct Buffer {
    char bytes[1536];
    uint64_t length;
};

Buffer g_buffer;
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

void emit_line(const char* line, uint64_t length) {
    hybrid::user::write_text("[mmstat] ");
    hybrid::user::syscall(hybrid::SyscallNumber::Write, hybrid::kStdoutFd, reinterpret_cast<uint64_t>(line), length);
    hybrid::user::write_text("\n");
}

bool read_proc_mm(Buffer& out) {
    out.length = 0;
    const char* path = "/proc/mm/summary";
    auto opened = hybrid::user::syscall(hybrid::SyscallNumber::VfsOpen, reinterpret_cast<uint64_t>(path), hybrid::user::strlen(path) + 1);
    if (opened.error != hybrid::kSyscallErrorNone || opened.value == 0) {
        hybrid::user::write_hex_line("[mmstat] ", "open error ", opened.error);
        return false;
    }

    while (out.length < sizeof(out.bytes)) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::VfsReadHandle, opened.value, reinterpret_cast<uint64_t>(g_chunk), sizeof(g_chunk));
        if (read.error != hybrid::kSyscallErrorNone) {
            if (read.error == hybrid::kSyscallErrorNotFound && out.length != 0) break;
            hybrid::user::write_hex_line("[mmstat] ", "read error ", read.error);
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

void process_line(const char* line, uint64_t length, uint64_t& emitted) {
    const char* prefixes[] = {
        "pmm_total_pages ",
        "pmm_free_pages ",
        "pmm_used_pages ",
        "pmm_usable_bytes ",
        "pmm_reserved_bytes ",
        "pmm_allocate_page_calls ",
        "pmm_allocate_contiguous_calls ",
        "pmm_failed_allocations ",
        "pmm_peak_used_pages ",
        "vmm_active_pml4 ",
        "vmm_map_page_calls ",
        "vmm_mapped_pages ",
        "vmm_unmapped_pages ",
        "vmm_failed_maps ",
        "vmm_remote_shootdowns_requested ",
        "heap_bytes ",
        "heap_block_count ",
        "heap_used_blocks ",
        "heap_free_blocks ",
        "heap_used_bytes ",
        "heap_free_bytes ",
        "heap_largest_free_block ",
    };

    for (uint64_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        if (starts_with(line, length, prefixes[i])) {
            emit_line(line, length);
            ++emitted;
            return;
        }
    }
}

uint64_t process_buffer(const Buffer& buffer) {
    uint64_t emitted = 0;
    uint64_t start = 0;
    for (uint64_t i = 0; i <= buffer.length; ++i) {
        if (i != buffer.length && buffer.bytes[i] != '\n') continue;
        uint64_t length = i - start;
        if (length != 0 && buffer.bytes[start + length - 1] == '\r') --length;
        if (length != 0) process_line(buffer.bytes + start, length, emitted);
        start = i + 1;
    }
    return emitted;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    if (!read_proc_mm(g_buffer)) {
        hybrid::user::exit(1);
    }
    uint64_t lines = process_buffer(g_buffer);
    hybrid::user::write_hex_line("[mmstat] ", "lines ", lines);
    hybrid::user::exit(lines != 0 ? 0 : 2);
}
