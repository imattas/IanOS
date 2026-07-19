#include "hybrid/user.hpp"

namespace {

uint64_t parse_decimal(const char* text, bool& ok) {
    ok = false;
    uint64_t value = 0;
    if (!text || text[0] == 0) return 0;
    for (uint64_t i = 0; text[i] != 0; ++i) {
        if (text[i] < '0' || text[i] > '9') return 0;
        value = value * 10 + static_cast<uint64_t>(text[i] - '0');
        ok = true;
    }
    return value;
}

}

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo argument;
    auto* bytes = reinterpret_cast<unsigned char*>(&argument);
    for (uint64_t i = 0; i < sizeof(argument); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, 1, reinterpret_cast<uint64_t>(&argument));
    if (result.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_line("[sleep] missing ticks");
        hybrid::user::exit(1);
    }
    bool ok = false;
    uint64_t ticks = parse_decimal(argument.value, ok);
    if (!ok) {
        hybrid::user::write_line("[sleep] invalid ticks");
        hybrid::user::exit(1);
    }
    hybrid::user::write_hex_line("[sleep] ", "ticks ", ticks);
    hybrid::user::syscall(hybrid::SyscallNumber::SleepTicks, ticks);
    hybrid::user::write_line("[sleep] done");
    hybrid::user::exit(ticks);
}
