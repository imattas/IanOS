#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
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

bool parse_i64(const char* text, int64_t& out) {
    if (!text || text[0] == 0) return false;
    uint64_t cursor = 0;
    bool negative = false;
    if (text[0] == '-') {
        negative = true;
        cursor = 1;
        if (text[cursor] == 0) return false;
    }
    int64_t value = 0;
    for (; text[cursor] != 0; ++cursor) {
        if (text[cursor] < '0' || text[cursor] > '9') return false;
        value = value * 10 + static_cast<int64_t>(text[cursor] - '0');
    }
    out = negative ? -value : value;
    return true;
}

void append_i64(char* out, uint64_t capacity, uint64_t& cursor, int64_t value) {
    if (value < 0) {
        hybrid::user::append_char(out, capacity, cursor, '-');
        value = -value;
    }
    char digits[24];
    uint64_t count = 0;
    auto remaining = static_cast<uint64_t>(value);
    do {
        digits[count++] = static_cast<char>('0' + (remaining % 10));
        remaining /= 10;
    } while (remaining != 0 && count < sizeof(digits));
    while (count > 0) hybrid::user::append_char(out, capacity, cursor, digits[--count]);
}

bool evaluate(int64_t left, const char* op, int64_t right, int64_t& result) {
    if (streq(op, "+")) {
        result = left + right;
    } else if (streq(op, "-")) {
        result = left - right;
    } else if (streq(op, "*")) {
        result = left * right;
    } else if (streq(op, "/")) {
        if (right == 0) return false;
        result = left / right;
    } else if (streq(op, "%")) {
        if (right == 0) return false;
        result = left % right;
    } else if (streq(op, "=") || streq(op, "==")) {
        result = left == right ? 1 : 0;
    } else if (streq(op, "!=")) {
        result = left != right ? 1 : 0;
    } else if (streq(op, "<")) {
        result = left < right ? 1 : 0;
    } else if (streq(op, "<=")) {
        result = left <= right ? 1 : 0;
    } else if (streq(op, ">")) {
        result = left > right ? 1 : 0;
    } else if (streq(op, ">=")) {
        result = left >= right ? 1 : 0;
    } else {
        return false;
    }
    return true;
}

void write_i64_line(const char* prefix, const char* label, int64_t value) {
    char line[96];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, prefix);
    hybrid::user::append_text(line, sizeof(line), cursor, label);
    append_i64(line, sizeof(line), cursor, value);
    hybrid::user::write_line(line);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo left_arg{};
    hybrid::ArgumentInfo op_arg{};
    hybrid::ArgumentInfo right_arg{};
    if (!get_arg(1, left_arg) || !get_arg(2, op_arg) || !get_arg(3, right_arg)) {
        hybrid::user::write_line("[expr] usage expr <left> <op> <right>");
        hybrid::user::exit(1);
    }

    int64_t left = 0;
    int64_t right = 0;
    if (!parse_i64(left_arg.value, left) || !parse_i64(right_arg.value, right)) {
        hybrid::user::write_line("[expr] bad number");
        hybrid::user::exit(1);
    }

    int64_t result = 0;
    if (!evaluate(left, op_arg.value, right, result)) {
        hybrid::user::write_line("[expr] bad operator");
        hybrid::user::exit(2);
    }

    hybrid::user::write_text_line("[expr] ", "operator ", op_arg.value);
    write_i64_line("[expr] ", "result ", result);
    hybrid::user::exit(result == 0 ? 1 : 0);
}
