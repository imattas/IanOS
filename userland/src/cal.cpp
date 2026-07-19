#include "hybrid/user.hpp"

namespace {

const char* const kMonthNames[] = {
    "", "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December",
};

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
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

void append_decimal(char* out, uint64_t capacity, uint64_t& cursor, uint64_t value) {
    char digits[24];
    uint64_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && count < sizeof(digits));
    while (count > 0) hybrid::user::append_char(out, capacity, cursor, digits[--count]);
}

bool leap_year(uint64_t year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

uint64_t days_in_month(uint64_t month, uint64_t year) {
    if (month == 2) return leap_year(year) ? 29 : 28;
    if (month == 4 || month == 6 || month == 9 || month == 11) return 30;
    return 31;
}

uint64_t weekday(uint64_t year, uint64_t month, uint64_t day) {
    if (month < 3) {
        month += 12;
        --year;
    }
    const uint64_t k = year % 100;
    const uint64_t j = year / 100;
    const uint64_t h = (day + ((13 * (month + 1)) / 5) + k + (k / 4) + (j / 4) + (5 * j)) % 7;
    return (h + 6) % 7;
}

void append_day(char* line, uint64_t capacity, uint64_t& cursor, uint64_t day) {
    if (day < 10) hybrid::user::append_char(line, capacity, cursor, ' ');
    append_decimal(line, capacity, cursor, day);
}

void emit_row(uint64_t cell_start, uint64_t days, uint64_t first_weekday) {
    char line[96];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[cal] ");
    for (uint64_t column = 0; column < 7; ++column) {
        if (column != 0) hybrid::user::append_char(line, sizeof(line), cursor, ' ');
        const uint64_t cell = cell_start + column;
        if (cell < first_weekday || cell >= first_weekday + days) {
            hybrid::user::append_text(line, sizeof(line), cursor, "  ");
            continue;
        }
        append_day(line, sizeof(line), cursor, cell - first_weekday + 1);
    }
    hybrid::user::write_line(line);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    uint64_t month = 0;
    uint64_t year = 0;
    hybrid::ArgumentInfo month_arg{};
    hybrid::ArgumentInfo year_arg{};
    if (get_arg(1, month_arg) || get_arg(2, year_arg)) {
        if (!get_arg(2, year_arg) || !parse_u64(month_arg.value, month) || !parse_u64(year_arg.value, year)) {
            hybrid::user::write_line("[cal] usage cal [month year]");
            hybrid::user::exit(1);
        }
    } else {
        hybrid::DateTimeInfo now{};
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetDateTime, reinterpret_cast<uint64_t>(&now));
        if (result.error != hybrid::kSyscallErrorNone || result.value != 1) {
            hybrid::user::write_line("[cal] date error");
            hybrid::user::exit(1);
        }
        month = now.month;
        year = now.year;
    }

    if (month < 1 || month > 12 || year < 1 || year > 9999) {
        hybrid::user::write_line("[cal] range error");
        hybrid::user::exit(1);
    }

    char title[96];
    uint64_t title_cursor = 0;
    hybrid::user::append_text(title, sizeof(title), title_cursor, "[cal] ");
    hybrid::user::append_text(title, sizeof(title), title_cursor, kMonthNames[month]);
    hybrid::user::append_char(title, sizeof(title), title_cursor, ' ');
    append_decimal(title, sizeof(title), title_cursor, year);
    hybrid::user::write_line(title);
    hybrid::user::write_line("[cal] Su Mo Tu We Th Fr Sa");

    const uint64_t first = weekday(year, month, 1);
    const uint64_t days = days_in_month(month, year);
    for (uint64_t cell_start = 0; cell_start < days + first; cell_start += 7) {
        emit_row(cell_start, days, first);
    }
    hybrid::user::write_hex_line("[cal] ", "days ", days);
    hybrid::user::exit(0);
}
