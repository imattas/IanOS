#include "hybrid/user.hpp"

namespace {

void append_two(char* buffer, uint64_t capacity, uint64_t& cursor, uint64_t value) {
    hybrid::user::append_char(buffer, capacity, cursor, static_cast<char>('0' + ((value / 10) % 10)));
    hybrid::user::append_char(buffer, capacity, cursor, static_cast<char>('0' + (value % 10)));
}

void append_four(char* buffer, uint64_t capacity, uint64_t& cursor, uint64_t value) {
    hybrid::user::append_char(buffer, capacity, cursor, static_cast<char>('0' + ((value / 1000) % 10)));
    hybrid::user::append_char(buffer, capacity, cursor, static_cast<char>('0' + ((value / 100) % 10)));
    hybrid::user::append_char(buffer, capacity, cursor, static_cast<char>('0' + ((value / 10) % 10)));
    hybrid::user::append_char(buffer, capacity, cursor, static_cast<char>('0' + (value % 10)));
}

}

extern "C" [[noreturn]] void _start() {
    hybrid::DateTimeInfo info;
    unsigned char* bytes = reinterpret_cast<unsigned char*>(&info);
    for (uint64_t i = 0; i < sizeof(info); ++i) bytes[i] = 0;

    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetDateTime, reinterpret_cast<uint64_t>(&info));
    if (result.error != hybrid::kSyscallErrorNone || result.value != 1) {
        hybrid::user::write_line("[date] error");
        hybrid::user::exit(1);
    }

    char line[40];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[date] ");
    append_four(line, sizeof(line), cursor, info.year);
    hybrid::user::append_char(line, sizeof(line), cursor, '-');
    append_two(line, sizeof(line), cursor, info.month);
    hybrid::user::append_char(line, sizeof(line), cursor, '-');
    append_two(line, sizeof(line), cursor, info.day);
    hybrid::user::append_char(line, sizeof(line), cursor, ' ');
    append_two(line, sizeof(line), cursor, info.hour);
    hybrid::user::append_char(line, sizeof(line), cursor, ':');
    append_two(line, sizeof(line), cursor, info.minute);
    hybrid::user::append_char(line, sizeof(line), cursor, ':');
    append_two(line, sizeof(line), cursor, info.second);
    hybrid::user::write_line(line);
    hybrid::user::exit(0);
}
