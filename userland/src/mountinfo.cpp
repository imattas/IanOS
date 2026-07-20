#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

bool equals(const char* a, const char* b) {
    uint64_t i = 0;
    for (;; ++i) {
        if (a[i] != b[i]) return false;
        if (a[i] == 0) return true;
    }
}

void print_line_prefix(const char* line, uint64_t length) {
    char out[224];
    uint64_t cursor = 0;
    hybrid::user::append_text(out, sizeof(out), cursor, "[mountinfo] ");
    for (uint64_t i = 0; i < length; ++i) hybrid::user::append_char(out, sizeof(out), cursor, line[i]);
    hybrid::user::write_line(out);
}

int main_result() {
    hybrid::ArgumentInfo arg;
    const char* path = "/proc/self/mountinfo";
    if (get_arg(1, arg)) {
        if (equals(arg.value, "all") || equals(arg.value, "-a")) {
            path = "/proc/mountinfo";
        } else {
            path = arg.value;
        }
    }

    hybrid::user::write_text_line("[mountinfo] ", "path ", path);
    auto open_result = hybrid::user::syscall(
        hybrid::SyscallNumber::Open,
        reinterpret_cast<uint64_t>(path),
        hybrid::user::strlen(path) + 1);
    if (open_result.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[mountinfo] ", "open error ", open_result.error);
        return 1;
    }

    char buffer[512];
    uint64_t total = 0;
    char line[192];
    uint64_t line_len = 0;
    for (;;) {
        auto read_result = hybrid::user::syscall(
            hybrid::SyscallNumber::Read,
            open_result.value,
            reinterpret_cast<uint64_t>(buffer),
            sizeof(buffer));
        if (read_result.error != hybrid::kSyscallErrorNone) {
            if (read_result.error == hybrid::kSyscallErrorNotFound && total != 0) break;
            hybrid::user::write_hex_line("[mountinfo] ", "read error ", read_result.error);
            hybrid::user::syscall(hybrid::SyscallNumber::Close, open_result.value);
            return 2;
        }
        if (read_result.value == 0) break;
        total += read_result.value;
        for (uint64_t i = 0; i < read_result.value; ++i) {
            if (buffer[i] == '\n') {
                print_line_prefix(line, line_len);
                line_len = 0;
            } else if (line_len + 1 < sizeof(line)) {
                line[line_len++] = buffer[i];
            }
        }
    }
    if (line_len != 0) print_line_prefix(line, line_len);
    hybrid::user::syscall(hybrid::SyscallNumber::Close, open_result.value);
    hybrid::user::write_hex_line("[mountinfo] ", "bytes ", total);
    return total == 0 ? 3 : 0;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::user::exit(static_cast<uint64_t>(main_result()));
}
