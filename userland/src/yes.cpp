#include "hybrid/user.hpp"

namespace {

constexpr uint64_t kDefaultInteractiveLimit = 64;
constexpr uint64_t kLineCapacity = 96;

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
    out = value;
    return true;
}

void append_word(char* line, uint64_t capacity, uint64_t& cursor, const char* word) {
    if (cursor != 0) hybrid::user::append_char(line, capacity, cursor, ' ');
    hybrid::user::append_text(line, capacity, cursor, word);
}

void write_yes_line(const char* text) {
    hybrid::user::write_text(text);
    hybrid::user::write_text("\n");
}

} // namespace

extern "C" [[noreturn]] void _start() {
    char line[kLineCapacity];
    uint64_t cursor = 0;
    uint64_t limit = kDefaultInteractiveLimit;
    bool explicit_limit = false;

    for (uint64_t index = 1;; ++index) {
        hybrid::ArgumentInfo arg{};
        if (!get_arg(index, arg)) break;

        if (starts_with(arg.value, "count=")) {
            if (!parse_u64(arg.value + 6, limit)) {
                hybrid::user::write_line("[yes] bad count");
                hybrid::user::exit(1);
            }
            explicit_limit = true;
            continue;
        }
        if (arg.value[0] == '-' && arg.value[1] == 'n' && arg.value[2] == 0) {
            hybrid::ArgumentInfo value{};
            if (!get_arg(index + 1, value) || !parse_u64(value.value, limit)) {
                hybrid::user::write_line("[yes] bad count");
                hybrid::user::exit(1);
            }
            explicit_limit = true;
            ++index;
            continue;
        }
        append_word(line, sizeof(line), cursor, arg.value);
    }

    if (cursor == 0) hybrid::user::append_text(line, sizeof(line), cursor, "y");

    hybrid::user::write_text_line("[yes] ", "text ", line);
    hybrid::user::write_hex_line("[yes] ", explicit_limit ? "count " : "default limit ", limit);
    for (uint64_t emitted = 0; emitted < limit; ++emitted) {
        write_yes_line(line);
        hybrid::user::syscall(hybrid::SyscallNumber::Yield);
    }
    hybrid::user::write_hex_line("[yes] ", "lines ", limit);
    hybrid::user::exit(0);
}
