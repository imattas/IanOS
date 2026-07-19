#include "hybrid/user.hpp"

namespace {

struct Options {
    char input[64];
    char output[64];
    uint64_t block_size;
    uint64_t count;
    bool has_count;
};

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

bool starts_with(const char* text, const char* prefix) {
    if (!text || !prefix) return false;
    for (uint64_t i = 0; prefix[i] != 0; ++i) {
        if (text[i] != prefix[i]) return false;
    }
    return true;
}

bool parse_u64(const char* text, uint64_t& out) {
    if (!text || text[0] == 0) return false;
    uint64_t value = 0;
    for (uint64_t i = 0; text[i] != 0; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + static_cast<uint64_t>(text[i] - '0');
    }
    if (value == 0 || value > 512) return false;
    out = value;
    return true;
}

void copy_text(char* out, uint64_t capacity, const char* text) {
    if (!out || capacity == 0) return;
    uint64_t i = 0;
    if (text) {
        for (; i + 1 < capacity && text[i] != 0; ++i) out[i] = text[i];
    }
    out[i] = 0;
}

bool parse_options(Options& options) {
    options.input[0] = 0;
    options.output[0] = 0;
    options.block_size = 32;
    options.count = 0;
    options.has_count = false;

    for (uint64_t index = 1;; ++index) {
        hybrid::ArgumentInfo arg{};
        if (!get_arg(index, arg)) break;
        if (starts_with(arg.value, "if=")) {
            copy_text(options.input, sizeof(options.input), arg.value + 3);
        } else if (starts_with(arg.value, "of=")) {
            copy_text(options.output, sizeof(options.output), arg.value + 3);
        } else if (starts_with(arg.value, "bs=")) {
            if (!parse_u64(arg.value + 3, options.block_size)) return false;
        } else if (starts_with(arg.value, "count=")) {
            if (!parse_u64(arg.value + 6, options.count)) return false;
            options.has_count = true;
        } else {
            return false;
        }
    }
    return options.input[0] != 0 && options.output[0] != 0;
}

bool eof_result(const hybrid::SyscallResult& result) {
    return result.value == 0 && (result.error == hybrid::kSyscallErrorNone || result.error == hybrid::kSyscallErrorNotFound);
}

hybrid::SyscallResult read_blocking(uint64_t fd, char* buffer, uint64_t size) {
    for (;;) {
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::Read, fd, reinterpret_cast<uint64_t>(buffer), size);
        if (result.error != hybrid::kSyscallErrorWouldBlock) return result;
        hybrid::user::syscall(hybrid::SyscallNumber::Yield);
    }
}

} // namespace

extern "C" [[noreturn]] void _start() {
    Options options{};
    if (!parse_options(options)) {
        hybrid::user::write_line("[dd] usage: dd if=<path> of=<path> [bs=N] [count=N]");
        hybrid::user::exit(1);
    }

    auto input = hybrid::user::syscall(hybrid::SyscallNumber::Open,
                                       reinterpret_cast<uint64_t>(options.input),
                                       hybrid::user::strlen(options.input) + 1);
    if (input.error != hybrid::kSyscallErrorNone || input.value < 3) {
        hybrid::user::write_hex_line("[dd] ", "input error ", input.error);
        hybrid::user::exit(2);
    }

    auto created = hybrid::user::syscall(hybrid::SyscallNumber::CreateFile,
                                         reinterpret_cast<uint64_t>(options.output),
                                         hybrid::user::strlen(options.output) + 1);
    (void)created;
    auto output = hybrid::user::syscall(hybrid::SyscallNumber::Open,
                                        reinterpret_cast<uint64_t>(options.output),
                                        hybrid::user::strlen(options.output) + 1);
    if (output.error != hybrid::kSyscallErrorNone || output.value < 3) {
        hybrid::user::syscall(hybrid::SyscallNumber::Close, input.value);
        hybrid::user::write_hex_line("[dd] ", "output error ", output.error);
        hybrid::user::exit(3);
    }
    hybrid::user::syscall(hybrid::SyscallNumber::Seek, output.value, 0);

    char buffer[512];
    uint64_t records_in = 0;
    uint64_t records_out = 0;
    uint64_t bytes = 0;
    for (;;) {
        if (options.has_count && records_in >= options.count) break;
        auto read = read_blocking(input.value, buffer, options.block_size);
        if (eof_result(read)) break;
        if (read.error != hybrid::kSyscallErrorNone) {
            hybrid::user::syscall(hybrid::SyscallNumber::Close, input.value);
            hybrid::user::syscall(hybrid::SyscallNumber::Close, output.value);
            hybrid::user::write_hex_line("[dd] ", "read error ", read.error);
            hybrid::user::exit(4);
        }
        if (read.value == 0) break;
        ++records_in;
        uint64_t written = 0;
        while (written < read.value) {
            auto result = hybrid::user::syscall(hybrid::SyscallNumber::WriteFile,
                                                output.value,
                                                reinterpret_cast<uint64_t>(buffer + written),
                                                read.value - written);
            if (result.error != hybrid::kSyscallErrorNone || result.value == 0) {
                hybrid::user::syscall(hybrid::SyscallNumber::Close, input.value);
                hybrid::user::syscall(hybrid::SyscallNumber::Close, output.value);
                hybrid::user::write_hex_line("[dd] ", "write error ", result.error);
                hybrid::user::exit(5);
            }
            written += result.value;
        }
        ++records_out;
        bytes += read.value;
    }

    hybrid::user::syscall(hybrid::SyscallNumber::Close, input.value);
    hybrid::user::syscall(hybrid::SyscallNumber::Close, output.value);
    hybrid::user::write_text_line("[dd] ", "if ", options.input);
    hybrid::user::write_text_line("[dd] ", "of ", options.output);
    hybrid::user::write_hex_line("[dd] ", "bs ", options.block_size);
    hybrid::user::write_hex_line("[dd] ", "records in ", records_in);
    hybrid::user::write_hex_line("[dd] ", "records out ", records_out);
    hybrid::user::write_hex_line("[dd] ", "bytes ", bytes);
    hybrid::user::exit(bytes);
}
