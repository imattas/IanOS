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

bool parse_u64(const char* text, uint64_t& out) {
    if (!text || text[0] == 0) return false;
    uint64_t value = 0;
    for (uint64_t i = 0; text[i] != 0; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + static_cast<uint64_t>(text[i] - '0');
    }
    if (value == 0) return false;
    out = value;
    return true;
}

bool streq(const char* left, const char* right) {
    if (!left || !right) return false;
    uint64_t i = 0;
    while (left[i] != 0 && right[i] != 0) {
        if (left[i] != right[i]) return false;
        ++i;
    }
    return left[i] == right[i];
}

bool parse_options(uint64_t& field, char& delimiter, hybrid::ArgumentInfo& path, bool& has_path) {
    delimiter = '\t';
    field = 0;
    has_path = false;

    uint64_t index = 1;
    while (true) {
        hybrid::ArgumentInfo arg{};
        if (!get_arg(index, arg)) break;
        if (streq(arg.value, "-d")) {
            hybrid::ArgumentInfo value{};
            if (!get_arg(index + 1, value) || value.value[0] == 0) return false;
            delimiter = value.value[0];
            index += 2;
            continue;
        }
        if (streq(arg.value, "-f")) {
            hybrid::ArgumentInfo value{};
            if (!get_arg(index + 1, value) || !parse_u64(value.value, field)) return false;
            index += 2;
            continue;
        }
        path = arg;
        has_path = true;
        ++index;
    }
    return field != 0;
}

void emit_selected_field(const char* line, uint64_t length, char delimiter, uint64_t wanted_field, uint64_t& selected) {
    uint64_t field = 1;
    uint64_t start = 0;
    uint64_t end = length;
    bool found = wanted_field == 1;
    for (uint64_t i = 0; i < length; ++i) {
        if (line[i] != delimiter) continue;
        if (field == wanted_field) {
            end = i;
            found = true;
            break;
        }
        ++field;
        start = i + 1;
        if (field == wanted_field) found = true;
    }
    if (!found) {
        start = 0;
        end = 0;
    }

    hybrid::user::write_text("[cut] ");
    if (end > start) {
        hybrid::user::syscall(hybrid::SyscallNumber::Write, hybrid::kStdoutFd, reinterpret_cast<uint64_t>(line + start), end - start);
        ++selected;
    }
    hybrid::user::write_text("\n");
}

} // namespace

extern "C" [[noreturn]] void _start() {
    uint64_t field = 0;
    char delimiter = '\t';
    hybrid::ArgumentInfo path{};
    bool has_path = false;
    if (!parse_options(field, delimiter, path, has_path)) {
        hybrid::user::write_line("[cut] usage cut -d <char> -f <field> [path]");
        hybrid::user::exit(1);
    }

    uint64_t fd = hybrid::kStdinFd;
    bool close_when_done = false;
    if (has_path) {
        auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open,
                                            reinterpret_cast<uint64_t>(path.value),
                                            hybrid::user::strlen(path.value) + 1);
        if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
            hybrid::user::write_hex_line("[cut] ", "open error ", opened.error);
            hybrid::user::exit(2);
        }
        fd = opened.value;
        close_when_done = true;
    }

    char delimiter_text[2] = {delimiter, 0};
    hybrid::user::write_text_line("[cut] ", "path ", has_path ? path.value : "<stdin>");
    hybrid::user::write_text_line("[cut] ", "delimiter ", delimiter_text);
    hybrid::user::write_hex_line("[cut] ", "field ", field);

    char read_buffer[32];
    char line[128];
    uint64_t line_length = 0;
    uint64_t emitted = 0;
    uint64_t selected = 0;

    for (;;) {
        auto read = read_blocking(fd, read_buffer, sizeof(read_buffer));
        if (eof_result(read)) break;
        if (read.error != hybrid::kSyscallErrorNone) {
            if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);
            hybrid::user::write_hex_line("[cut] ", "read error ", read.error);
            hybrid::user::exit(3);
        }
        if (read.value == 0) break;
        for (uint64_t i = 0; i < read.value; ++i) {
            const char c = read_buffer[i];
            if (c == '\r') continue;
            if (c == '\n') {
                emit_selected_field(line, line_length, delimiter, field, selected);
                line_length = 0;
                ++emitted;
            } else if (line_length + 1 < sizeof(line)) {
                line[line_length++] = c;
            }
        }
    }
    if (line_length != 0) {
        emit_selected_field(line, line_length, delimiter, field, selected);
        ++emitted;
    }
    if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);
    hybrid::user::write_hex_line("[cut] ", "lines ", emitted);
    hybrid::user::write_hex_line("[cut] ", "selected ", selected);
    hybrid::user::exit(selected);
}
