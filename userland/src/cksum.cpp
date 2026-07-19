#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

hybrid::SyscallResult read_blocking(uint64_t fd, unsigned char* buffer, uint64_t size) {
    for (;;) {
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::Read, fd, reinterpret_cast<uint64_t>(buffer), size);
        if (result.error != hybrid::kSyscallErrorWouldBlock) return result;
        hybrid::user::syscall(hybrid::SyscallNumber::Yield);
    }
}

uint32_t crc32_update(uint32_t crc, const unsigned char* data, uint64_t length) {
    crc = ~crc;
    for (uint64_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint64_t bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return ~crc;
}

void write_digest(uint32_t crc, const char* label) {
    char line[128];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[cksum] crc32 0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        hybrid::user::append_char(line, sizeof(line), cursor, hybrid::user::hex_digit(crc >> shift));
    }
    hybrid::user::append_text(line, sizeof(line), cursor, "  ");
    hybrid::user::append_text(line, sizeof(line), cursor, label);
    hybrid::user::write_line(line);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo path{};
    const bool has_path = get_arg(1, path);
    uint64_t fd = hybrid::kStdinFd;
    bool close_when_done = false;
    const char* label = "<stdin>";
    if (has_path) {
        label = path.value;
        auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open,
                                            reinterpret_cast<uint64_t>(path.value),
                                            hybrid::user::strlen(path.value) + 1);
        if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
            hybrid::user::write_hex_line("[cksum] ", "open error ", opened.error);
            hybrid::user::exit(2);
        }
        fd = opened.value;
        close_when_done = true;
    }

    uint32_t crc = 0;
    uint64_t bytes = 0;
    unsigned char buffer[32];
    for (;;) {
        auto read = read_blocking(fd, buffer, sizeof(buffer));
        if (read.error == hybrid::kSyscallErrorNotFound && read.value == 0) break;
        if (read.error != hybrid::kSyscallErrorNone) {
            if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);
            hybrid::user::write_hex_line("[cksum] ", "read error ", read.error);
            hybrid::user::exit(3);
        }
        if (read.value == 0) break;
        crc = crc32_update(crc, buffer, read.value);
        bytes += read.value;
    }
    if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);

    write_digest(crc, label);
    hybrid::user::write_hex_line("[cksum] ", "bytes ", bytes);
    hybrid::user::exit(0);
}
