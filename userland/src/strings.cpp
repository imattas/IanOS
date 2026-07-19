#include "hybrid/user.hpp"

namespace {

constexpr uint64_t kMinimumRun = 4;
constexpr uint64_t kMaxOutputLines = 24;

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

bool printable(unsigned char value) {
    return value == '\t' || (value >= 32 && value <= 126);
}

hybrid::SyscallResult read_next(uint64_t fd, unsigned char* buffer, uint64_t size) {
    for (;;) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::Read, fd, reinterpret_cast<uint64_t>(buffer), size);
        if (read.error != hybrid::kSyscallErrorWouldBlock) return read;
        hybrid::user::syscall(hybrid::SyscallNumber::Yield);
    }
}

bool eof_result(const hybrid::SyscallResult& result) {
    return result.value == 0 && (result.error == hybrid::kSyscallErrorNone || result.error == hybrid::kSyscallErrorNotFound);
}

void write_run(const char* run, uint64_t length) {
    char line[160];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[strings] ");
    for (uint64_t i = 0; i < length; ++i) {
        char ch = run[i];
        if (ch == '\t') ch = ' ';
        hybrid::user::append_char(line, sizeof(line), cursor, ch);
    }
    hybrid::user::write_line(line);
}

void flush_run(char* run, uint64_t& length, uint64_t& lines) {
    if (length >= kMinimumRun && lines < kMaxOutputLines) {
        write_run(run, length);
        ++lines;
    }
    length = 0;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo path{};
    if (!get_arg(1, path)) {
        hybrid::user::write_line("[strings] usage strings <path>");
        hybrid::user::exit(1);
    }

    auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open,
                                        reinterpret_cast<uint64_t>(path.value),
                                        hybrid::user::strlen(path.value) + 1);
    if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
        hybrid::user::write_hex_line("[strings] ", "open error ", opened.error);
        hybrid::user::exit(2);
    }

    hybrid::user::write_text_line("[strings] ", "path ", path.value);
    unsigned char buffer[32];
    char run[96];
    uint64_t run_length = 0;
    uint64_t lines = 0;
    uint64_t bytes = 0;

    for (;;) {
        auto read = read_next(opened.value, buffer, sizeof(buffer));
        if (eof_result(read)) break;
        if (read.error != hybrid::kSyscallErrorNone) {
            hybrid::user::syscall(hybrid::SyscallNumber::Close, opened.value);
            hybrid::user::write_hex_line("[strings] ", "read error ", read.error);
            hybrid::user::exit(3);
        }
        if (read.value == 0) break;
        bytes += read.value;
        for (uint64_t i = 0; i < read.value; ++i) {
            if (printable(buffer[i])) {
                if (run_length + 1 < sizeof(run)) {
                    run[run_length++] = static_cast<char>(buffer[i]);
                    run[run_length] = 0;
                } else {
                    flush_run(run, run_length, lines);
                    run[run_length++] = static_cast<char>(buffer[i]);
                    run[run_length] = 0;
                }
            } else {
                flush_run(run, run_length, lines);
            }
        }
    }

    flush_run(run, run_length, lines);
    hybrid::user::syscall(hybrid::SyscallNumber::Close, opened.value);
    hybrid::user::write_hex_line("[strings] ", "lines ", lines);
    hybrid::user::write_hex_line("[strings] ", "bytes ", bytes);
    hybrid::user::exit(lines);
}
