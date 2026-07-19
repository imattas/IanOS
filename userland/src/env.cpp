#include "hybrid/user.hpp"

namespace {

void write_env(const hybrid::EnvironmentInfo& env) {
    char line[128];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[env] ");
    hybrid::user::append_text(line, sizeof(line), cursor, env.key);
    hybrid::user::append_char(line, sizeof(line), cursor, '=');
    hybrid::user::append_text(line, sizeof(line), cursor, env.value);
    hybrid::user::write_line(line);
}

void clear_env(hybrid::EnvironmentInfo& env) {
    for (uint64_t i = 0; i < sizeof(env.key); ++i) env.key[i] = 0;
    for (uint64_t i = 0; i < sizeof(env.value); ++i) env.value[i] = 0;
}

}

extern "C" [[noreturn]] void _start() {
    auto count = hybrid::user::syscall(hybrid::SyscallNumber::GetEnvironmentCount);
    if (count.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[env] ", "error ", count.error);
        hybrid::user::exit(1);
    }
    hybrid::user::write_hex_line("[env] ", "count ", count.value);
    for (uint64_t i = 0; i < count.value && i < 8; ++i) {
        hybrid::EnvironmentInfo env;
        clear_env(env);
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetEnvironment, i, reinterpret_cast<uint64_t>(&env));
        if (result.error == hybrid::kSyscallErrorNone) write_env(env);
    }
    hybrid::user::exit(count.value);
}
