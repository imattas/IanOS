#include "hybrid/user.hpp"

namespace {

struct Buffer {
    char bytes[8192];
    uint64_t length;
};

Buffer g_modules;
char g_chunk[32];

bool read_modules(Buffer& out) {
    out.length = 0;
    const char* path = "/proc/modules";
    auto opened = hybrid::user::syscall(hybrid::SyscallNumber::VfsOpen, reinterpret_cast<uint64_t>(path), hybrid::user::strlen(path) + 1);
    if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
        hybrid::user::write_hex_line("[lsmod] ", "open error ", opened.error);
        return false;
    }

    while (out.length < sizeof(out.bytes)) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::VfsReadHandle, opened.value, reinterpret_cast<uint64_t>(g_chunk), sizeof(g_chunk));
        if (read.error != hybrid::kSyscallErrorNone) {
            if (read.error == hybrid::kSyscallErrorNotFound && out.length != 0) break;
            hybrid::user::write_hex_line("[lsmod] ", "read error ", read.error);
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

void emit_line(const char* line, uint64_t length, uint64_t index) {
    if (index == 0) {
        hybrid::user::write_text("[lsmod] ");
    } else {
        hybrid::user::write_text("[lsmod] module ");
    }
    hybrid::user::syscall(hybrid::SyscallNumber::Write, hybrid::kStdoutFd, reinterpret_cast<uint64_t>(line), length);
    hybrid::user::write_text("\n");
}

uint64_t emit_modules(const Buffer& buffer) {
    uint64_t rows = 0;
    uint64_t start = 0;
    for (uint64_t i = 0; i <= buffer.length; ++i) {
        if (i != buffer.length && buffer.bytes[i] != '\n') continue;
        uint64_t length = i - start;
        if (length != 0 && buffer.bytes[start + length - 1] == '\r') --length;
        if (length != 0) {
            emit_line(buffer.bytes + start, length, rows);
            ++rows;
        }
        start = i + 1;
    }
    return rows;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    if (!read_modules(g_modules)) {
        hybrid::user::exit(1);
    }
    uint64_t rows = emit_modules(g_modules);
    uint64_t modules = rows == 0 ? 0 : rows - 1;
    hybrid::user::write_hex_line("[lsmod] ", "modules ", modules);
    hybrid::user::exit(modules != 0 ? 0 : 2);
}
