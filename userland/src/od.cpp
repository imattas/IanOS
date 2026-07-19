#include "hybrid/user.hpp"

namespace {

enum class OutputBase {
    Octal,
    Hex,
};

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

bool text_equals(const char* left, const char* right) {
    if (!left || !right) return false;
    uint64_t i = 0;
    for (; left[i] != 0 && right[i] != 0; ++i) {
        if (left[i] != right[i]) return false;
    }
    return left[i] == 0 && right[i] == 0;
}

void copy_text(char* out, uint64_t capacity, const char* text) {
    if (!out || capacity == 0) return;
    uint64_t i = 0;
    if (text) {
        for (; i + 1 < capacity && text[i] != 0; ++i) out[i] = text[i];
    }
    out[i] = 0;
}

hybrid::SyscallResult read_blocking(uint64_t fd, unsigned char* buffer, uint64_t size) {
    for (;;) {
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::Read, fd, reinterpret_cast<uint64_t>(buffer), size);
        if (result.error != hybrid::kSyscallErrorWouldBlock) return result;
        hybrid::user::syscall(hybrid::SyscallNumber::Yield);
    }
}

void append_octal(char* line, uint64_t capacity, uint64_t& cursor, uint64_t value, uint64_t digits) {
    for (int shift = static_cast<int>((digits - 1) * 3); shift >= 0; shift -= 3) {
        hybrid::user::append_char(line, capacity, cursor, static_cast<char>('0' + ((value >> shift) & 7)));
    }
}

void append_hex(char* line, uint64_t capacity, uint64_t& cursor, uint64_t value, uint64_t digits) {
    for (int shift = static_cast<int>((digits - 1) * 4); shift >= 0; shift -= 4) {
        hybrid::user::append_char(line, capacity, cursor, hybrid::user::hex_digit(value >> shift));
    }
}

void write_row(uint64_t offset, const unsigned char* bytes, uint64_t length, OutputBase base) {
    char line[128];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[od] ");
    append_octal(line, sizeof(line), cursor, offset, 7);
    for (uint64_t i = 0; i < length; ++i) {
        hybrid::user::append_char(line, sizeof(line), cursor, ' ');
        if (base == OutputBase::Octal) append_octal(line, sizeof(line), cursor, bytes[i], 3);
        else append_hex(line, sizeof(line), cursor, bytes[i], 2);
    }
    hybrid::user::write_line(line);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    OutputBase base = OutputBase::Octal;
    char path_storage[64];
    path_storage[0] = 0;

    for (uint64_t index = 1;; ++index) {
        hybrid::ArgumentInfo arg{};
        if (!get_arg(index, arg)) break;
        if (text_equals(arg.value, "-t") || text_equals(arg.value, "-A")) {
            hybrid::ArgumentInfo value{};
            if (!get_arg(index + 1, value)) {
                hybrid::user::write_line("[od] usage od [-t x1|o1] [path]");
                hybrid::user::exit(1);
            }
            if (text_equals(arg.value, "-t")) {
                if (text_equals(value.value, "x1")) base = OutputBase::Hex;
                else if (text_equals(value.value, "o1")) base = OutputBase::Octal;
                else {
                    hybrid::user::write_line("[od] unsupported type");
                    hybrid::user::exit(1);
                }
            }
            ++index;
            continue;
        }
        copy_text(path_storage, sizeof(path_storage), arg.value);
    }

    uint64_t fd = hybrid::kStdinFd;
    bool close_when_done = false;
    const bool has_path = path_storage[0] != 0;
    if (has_path) {
        auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open,
                                            reinterpret_cast<uint64_t>(path_storage),
                                            hybrid::user::strlen(path_storage) + 1);
        if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
            hybrid::user::write_hex_line("[od] ", "open error ", opened.error);
            hybrid::user::exit(2);
        }
        fd = opened.value;
        close_when_done = true;
    }

    hybrid::user::write_text_line("[od] ", "path ", has_path ? path_storage : "<stdin>");
    hybrid::user::write_text_line("[od] ", "format ", base == OutputBase::Octal ? "o1" : "x1");

    unsigned char buffer[16];
    uint64_t offset = 0;
    for (uint64_t rows = 0; rows < 8; ++rows) {
        auto read = read_blocking(fd, buffer, sizeof(buffer));
        if (read.error == hybrid::kSyscallErrorNotFound && read.value == 0) break;
        if (read.error != hybrid::kSyscallErrorNone) {
            if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);
            hybrid::user::write_hex_line("[od] ", "read error ", read.error);
            hybrid::user::exit(3);
        }
        if (read.value == 0) break;
        write_row(offset, buffer, read.value, base);
        offset += read.value;
        if (read.value < sizeof(buffer)) break;
    }

    if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);
    hybrid::user::write_hex_line("[od] ", "bytes ", offset);
    hybrid::user::exit(0);
}
