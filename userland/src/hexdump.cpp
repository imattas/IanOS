#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

char hex_digit(uint8_t value) {
    value &= 0x0f;
    return value < 10 ? static_cast<char>('0' + value) : static_cast<char>('a' + value - 10);
}

void append_hex_byte(char* line, uint64_t capacity, uint64_t& cursor, uint8_t value) {
    hybrid::user::append_char(line, capacity, cursor, hex_digit(value >> 4));
    hybrid::user::append_char(line, capacity, cursor, hex_digit(value));
}

void append_hex_offset(char* line, uint64_t capacity, uint64_t& cursor, uint64_t value) {
    for (int shift = 28; shift >= 0; shift -= 4) {
        hybrid::user::append_char(line, capacity, cursor, hex_digit(static_cast<uint8_t>(value >> shift)));
    }
}

char printable(uint8_t value) {
    return value >= 32 && value <= 126 ? static_cast<char>(value) : '.';
}

void write_row(uint64_t offset, const unsigned char* bytes, uint64_t length) {
    char line[128];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[hexdump] ");
    append_hex_offset(line, sizeof(line), cursor, offset);
    hybrid::user::append_text(line, sizeof(line), cursor, "  ");
    for (uint64_t i = 0; i < 16; ++i) {
        if (i < length) append_hex_byte(line, sizeof(line), cursor, bytes[i]);
        else hybrid::user::append_text(line, sizeof(line), cursor, "  ");
        hybrid::user::append_char(line, sizeof(line), cursor, i == 7 ? '-' : ' ');
    }
    hybrid::user::append_text(line, sizeof(line), cursor, " |");
    for (uint64_t i = 0; i < length; ++i) hybrid::user::append_char(line, sizeof(line), cursor, printable(bytes[i]));
    hybrid::user::append_char(line, sizeof(line), cursor, '|');
    hybrid::user::write_line(line);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo path{};
    if (!get_arg(1, path)) {
        hybrid::user::write_line("[hexdump] usage hexdump <path>");
        hybrid::user::exit(1);
    }

    auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open,
                                        reinterpret_cast<uint64_t>(path.value),
                                        hybrid::user::strlen(path.value) + 1);
    if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
        hybrid::user::write_hex_line("[hexdump] ", "open error ", opened.error);
        hybrid::user::exit(2);
    }

    hybrid::user::write_text_line("[hexdump] ", "path ", path.value);
    unsigned char buffer[16];
    uint64_t offset = 0;
    for (uint64_t rows = 0; rows < 16; ++rows) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::Read, opened.value, reinterpret_cast<uint64_t>(buffer), sizeof(buffer));
        if (read.error != hybrid::kSyscallErrorNone) {
            hybrid::user::write_hex_line("[hexdump] ", "read error ", read.error);
            hybrid::user::syscall(hybrid::SyscallNumber::Close, opened.value);
            hybrid::user::exit(3);
        }
        if (read.value == 0) break;
        write_row(offset, buffer, read.value);
        offset += read.value;
        if (read.value < sizeof(buffer)) break;
    }
    hybrid::user::write_hex_line("[hexdump] ", "bytes ", offset);
    hybrid::user::syscall(hybrid::SyscallNumber::Close, opened.value);
    hybrid::user::exit(offset);
}
