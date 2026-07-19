#include "hk/time/rtc.hpp"
#include "hk/arch/x86_64/io.hpp"

namespace hk::time {
namespace {
constexpr uint16_t kCmosAddress = 0x70;
constexpr uint16_t kCmosData = 0x71;
constexpr uint8_t kRegSeconds = 0x00;
constexpr uint8_t kRegMinutes = 0x02;
constexpr uint8_t kRegHours = 0x04;
constexpr uint8_t kRegDay = 0x07;
constexpr uint8_t kRegMonth = 0x08;
constexpr uint8_t kRegYear = 0x09;
constexpr uint8_t kRegStatusA = 0x0a;
constexpr uint8_t kRegStatusB = 0x0b;
constexpr uint8_t kRegCentury = 0x32;

uint8_t cmos_read(uint8_t reg) {
    hk::arch::x86_64::outb(kCmosAddress, static_cast<uint8_t>(reg | 0x80));
    hk::arch::x86_64::io_wait();
    return hk::arch::x86_64::inb(kCmosData);
}

bool update_in_progress() {
    return (cmos_read(kRegStatusA) & 0x80) != 0;
}

uint8_t bcd_to_binary(uint8_t value) {
    return static_cast<uint8_t>((value & 0x0f) + ((value >> 4) * 10));
}

bool valid(const hybrid::DateTimeInfo& dt) {
    return dt.year >= 2020 && dt.year < 2100 &&
        dt.month >= 1 && dt.month <= 12 &&
        dt.day >= 1 && dt.day <= 31 &&
        dt.hour < 24 && dt.minute < 60 && dt.second < 60;
}

struct RawRtc {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint8_t year;
    uint8_t century;
};

RawRtc read_raw() {
    RawRtc value{};
    value.second = cmos_read(kRegSeconds);
    value.minute = cmos_read(kRegMinutes);
    value.hour = cmos_read(kRegHours);
    value.day = cmos_read(kRegDay);
    value.month = cmos_read(kRegMonth);
    value.year = cmos_read(kRegYear);
    value.century = cmos_read(kRegCentury);
    return value;
}

bool same_snapshot(const RawRtc& a, const RawRtc& b) {
    return a.second == b.second && a.minute == b.minute && a.hour == b.hour &&
        a.day == b.day && a.month == b.month && a.year == b.year && a.century == b.century;
}
}

bool read_rtc_datetime(hybrid::DateTimeInfo& out) {
    RawRtc first{};
    RawRtc second{};
    bool stable = false;
    for (uint32_t attempt = 0; attempt < 128; ++attempt) {
        while (update_in_progress()) {
            asm volatile("pause");
        }
        first = read_raw();
        while (update_in_progress()) {
            asm volatile("pause");
        }
        second = read_raw();
        if (same_snapshot(first, second)) {
            stable = true;
            break;
        }
    }
    if (!stable) return false;

    uint8_t status_b = cmos_read(kRegStatusB);
    bool binary = (status_b & 0x04) != 0;
    bool hour_24 = (status_b & 0x02) != 0;
    bool pm = (second.hour & 0x80) != 0;

    if (!binary) {
        second.second = bcd_to_binary(second.second);
        second.minute = bcd_to_binary(second.minute);
        second.hour = static_cast<uint8_t>((second.hour & 0x80) | bcd_to_binary(static_cast<uint8_t>(second.hour & 0x7f)));
        second.day = bcd_to_binary(second.day);
        second.month = bcd_to_binary(second.month);
        second.year = bcd_to_binary(second.year);
        if (second.century != 0) second.century = bcd_to_binary(second.century);
    }

    second.hour = static_cast<uint8_t>(second.hour & 0x7f);
    if (!hour_24) {
        if (pm && second.hour < 12) second.hour = static_cast<uint8_t>(second.hour + 12);
        if (!pm && second.hour == 12) second.hour = 0;
    }

    uint16_t year = second.century != 0
        ? static_cast<uint16_t>(static_cast<uint16_t>(second.century) * 100 + second.year)
        : static_cast<uint16_t>(2000 + second.year);

    out.year = year;
    out.month = second.month;
    out.day = second.day;
    out.hour = second.hour;
    out.minute = second.minute;
    out.second = second.second;
    out.reserved = 0;
    return valid(out);
}

} // namespace hk::time
