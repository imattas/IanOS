#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
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

bool copy_until(char delimiter, const char* in, uint64_t& cursor, char* out, uint64_t capacity) {
    uint64_t out_cursor = 0;
    while (in[cursor] != 0 && in[cursor] != delimiter) {
        if (out_cursor + 1 >= capacity) return false;
        out[out_cursor++] = in[cursor++];
    }
    if (in[cursor] != delimiter) return false;
    out[out_cursor] = 0;
    ++cursor;
    return out_cursor != 0;
}

bool parse_substitution(const char* script, char* pattern, uint64_t pattern_cap, char* replacement, uint64_t replacement_cap) {
    if (!script || script[0] != 's' || script[1] == 0) return false;
    const char delimiter = script[1];
    uint64_t cursor = 2;
    if (!copy_until(delimiter, script, cursor, pattern, pattern_cap)) return false;
    return copy_until(delimiter, script, cursor, replacement, replacement_cap);
}

bool matches_at(const char* line, uint64_t length, uint64_t offset, const char* pattern, uint64_t pattern_length) {
    if (pattern_length == 0 || offset + pattern_length > length) return false;
    for (uint64_t i = 0; i < pattern_length; ++i) {
        if (line[offset + i] != pattern[i]) return false;
    }
    return true;
}

void transform_line(const char* line, uint64_t length, const char* pattern, const char* replacement, char* out, uint64_t capacity, uint64_t& out_length, uint64_t& substitutions) {
    const uint64_t pattern_length = hybrid::user::strlen(pattern);
    const uint64_t replacement_length = hybrid::user::strlen(replacement);
    out_length = 0;
    for (uint64_t i = 0; i < length;) {
        if (matches_at(line, length, i, pattern, pattern_length)) {
            for (uint64_t j = 0; j < replacement_length && out_length + 1 < capacity; ++j) out[out_length++] = replacement[j];
            i += pattern_length;
            ++substitutions;
        } else {
            if (out_length + 1 < capacity) out[out_length++] = line[i];
            ++i;
        }
    }
}

void emit_line(const char* line, uint64_t length, const char* pattern, const char* replacement, uint64_t& substitutions) {
    char transformed[160];
    uint64_t transformed_length = 0;
    transform_line(line, length, pattern, replacement, transformed, sizeof(transformed), transformed_length, substitutions);

    char output[192];
    uint64_t cursor = 0;
    hybrid::user::append_text(output, sizeof(output), cursor, "[sed] ");
    for (uint64_t i = 0; i < transformed_length; ++i) hybrid::user::append_char(output, sizeof(output), cursor, transformed[i]);
    hybrid::user::write_line(output);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo script{};
    hybrid::ArgumentInfo path{};
    if (!get_arg(1, script)) {
        hybrid::user::write_line("[sed] usage sed s/from/to/ [path]");
        hybrid::user::exit(1);
    }

    char pattern[32];
    char replacement[64];
    if (!parse_substitution(script.value, pattern, sizeof(pattern), replacement, sizeof(replacement))) {
        hybrid::user::write_line("[sed] bad script");
        hybrid::user::exit(1);
    }

    uint64_t fd = hybrid::kStdinFd;
    bool close_when_done = false;
    const bool has_path = get_arg(2, path);
    if (has_path) {
        auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open,
                                            reinterpret_cast<uint64_t>(path.value),
                                            hybrid::user::strlen(path.value) + 1);
        if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
            hybrid::user::write_hex_line("[sed] ", "open error ", opened.error);
            hybrid::user::exit(2);
        }
        fd = opened.value;
        close_when_done = true;
    }

    hybrid::user::write_text_line("[sed] ", "path ", has_path ? path.value : "<stdin>");
    hybrid::user::write_text_line("[sed] ", "pattern ", pattern);
    hybrid::user::write_text_line("[sed] ", "replacement ", replacement);

    char read_buffer[32];
    char line[128];
    uint64_t line_length = 0;
    uint64_t emitted = 0;
    uint64_t substitutions = 0;

    for (;;) {
        auto read = read_blocking(fd, read_buffer, sizeof(read_buffer));
        if (eof_result(read)) break;
        if (read.error != hybrid::kSyscallErrorNone) {
            if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);
            hybrid::user::write_hex_line("[sed] ", "read error ", read.error);
            hybrid::user::exit(3);
        }
        if (read.value == 0) break;
        for (uint64_t i = 0; i < read.value; ++i) {
            const char c = read_buffer[i];
            if (c == '\r') continue;
            if (c == '\n') {
                emit_line(line, line_length, pattern, replacement, substitutions);
                line_length = 0;
                ++emitted;
            } else if (line_length + 1 < sizeof(line)) {
                line[line_length++] = c;
            }
        }
    }
    if (line_length != 0) {
        emit_line(line, line_length, pattern, replacement, substitutions);
        ++emitted;
    }
    if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);
    hybrid::user::write_hex_line("[sed] ", "lines ", emitted);
    hybrid::user::write_hex_line("[sed] ", "substitutions ", substitutions);
    hybrid::user::exit(substitutions);
}
