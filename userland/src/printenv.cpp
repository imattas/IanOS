#include "hybrid/user.hpp"

namespace {

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

void write_assignment(const hybrid::EnvironmentInfo& env) {
    char line[128];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[printenv] ");
    hybrid::user::append_text(line, sizeof(line), cursor, env.key);
    hybrid::user::append_char(line, sizeof(line), cursor, '=');
    hybrid::user::append_text(line, sizeof(line), cursor, env.value);
    hybrid::user::write_line(line);
}

void clear_env(hybrid::EnvironmentInfo& env) {
    auto* bytes = reinterpret_cast<unsigned char*>(&env);
    for (uint64_t i = 0; i < sizeof(env); ++i) bytes[i] = 0;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    auto count = hybrid::user::syscall(hybrid::SyscallNumber::GetEnvironmentCount);
    if (count.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[printenv] ", "error ", count.error);
        hybrid::user::exit(1);
    }

    hybrid::ArgumentInfo key{};
    const bool has_key = get_arg(1, key);
    uint64_t printed = 0;
    for (uint64_t i = 0; i < count.value && i < 8; ++i) {
        hybrid::EnvironmentInfo env{};
        clear_env(env);
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetEnvironment, i, reinterpret_cast<uint64_t>(&env));
        if (result.error != hybrid::kSyscallErrorNone) continue;
        if (has_key && !text_equals(env.key, key.value)) continue;
        write_assignment(env);
        ++printed;
    }

    if (has_key && printed == 0) {
        hybrid::user::write_text_line("[printenv] ", "missing ", key.value);
        hybrid::user::exit(1);
    }
    hybrid::user::write_hex_line("[printenv] ", "count ", printed);
    hybrid::user::exit(0);
}
