#include "hybrid/user.hpp"

namespace {

struct Buffer {
    char bytes[1024];
    uint64_t length;
};

struct NetStats {
    char iface[16];
    char link[16];
    uint64_t speed_mbps;
    uint64_t mac_valid;
    uint64_t rx_packets;
    uint64_t rx_bytes;
    uint64_t tx_packets;
    uint64_t tx_completed;
    uint64_t rx_drops;
    uint64_t tx_errors;
    uint64_t mac_address;
};

char g_chunk[32];
Buffer g_summary;
Buffer g_dev;

bool starts_with(const char* line, uint64_t length, const char* prefix) {
    uint64_t i = 0;
    for (; prefix[i] != 0; ++i) {
        if (i >= length || line[i] != prefix[i]) return false;
    }
    return true;
}

void append_decimal(char* buffer, uint64_t capacity, uint64_t& cursor, uint64_t value) {
    char digits[20];
    uint64_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && count < sizeof(digits));
    while (count != 0) hybrid::user::append_char(buffer, capacity, cursor, digits[--count]);
}

uint64_t parse_decimal_text(const char* text) {
    uint64_t value = 0;
    for (uint64_t i = 0; text[i] != 0; ++i) {
        if (text[i] < '0' || text[i] > '9') break;
        value = value * 10 + static_cast<uint64_t>(text[i] - '0');
    }
    return value;
}

bool read_file(const char* path, Buffer& out) {
    out.length = 0;
    auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open,
                                        reinterpret_cast<uint64_t>(path),
                                        hybrid::user::strlen(path) + 1);
    if (opened.error != hybrid::kSyscallErrorNone) return false;
    while (out.length < sizeof(out.bytes)) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::Read,
                                          opened.value,
                                          reinterpret_cast<uint64_t>(g_chunk),
                                          sizeof(g_chunk));
        if (read.error == hybrid::kSyscallErrorNotFound && out.length != 0) break;
        if (read.error != hybrid::kSyscallErrorNone) {
            hybrid::user::syscall(hybrid::SyscallNumber::Close, opened.value);
            return false;
        }
        if (read.value == 0) break;
        for (uint64_t i = 0; i < read.value && out.length < sizeof(out.bytes); ++i) {
            out.bytes[out.length++] = g_chunk[i];
        }
    }
    hybrid::user::syscall(hybrid::SyscallNumber::Close, opened.value);
    return out.length != 0;
}

void copy_token(const char* line, uint64_t length, uint64_t& cursor, char* out, uint64_t capacity) {
    while (cursor < length && line[cursor] == ' ') ++cursor;
    uint64_t written = 0;
    while (cursor < length && line[cursor] != ' ' && line[cursor] != '\n' && line[cursor] != '\r') {
        if (written + 1 < capacity) out[written++] = line[cursor];
        ++cursor;
    }
    if (capacity != 0) out[written] = 0;
}

uint64_t parse_decimal_token(const char* line, uint64_t length, uint64_t& cursor) {
    char token[24];
    copy_token(line, length, cursor, token, sizeof(token));
    return parse_decimal_text(token);
}

template <typename Fn>
void for_each_line(const Buffer& buffer, Fn fn) {
    uint64_t start = 0;
    for (uint64_t i = 0; i <= buffer.length; ++i) {
        if (i != buffer.length && buffer.bytes[i] != '\n') continue;
        uint64_t length = i - start;
        if (length != 0 && buffer.bytes[start + length - 1] == '\r') --length;
        if (length != 0) fn(buffer.bytes + start, length);
        start = i + 1;
    }
}

void parse_summary(NetStats& stats) {
    for_each_line(g_summary, [&](const char* line, uint64_t length) {
        const char* prefix = "e1000_mac ";
        if (!starts_with(line, length, prefix)) return;
        char value[24];
        uint64_t cursor = hybrid::user::strlen(prefix);
        copy_token(line, length, cursor, value, sizeof(value));
        stats.mac_address = parse_decimal_text(value);
    });
}

bool parse_dev(NetStats& stats) {
    bool found = false;
    for_each_line(g_dev, [&](const char* line, uint64_t length) {
        if (found || !starts_with(line, length, "eth0 ")) return;
        uint64_t cursor = 0;
        copy_token(line, length, cursor, stats.iface, sizeof(stats.iface));
        copy_token(line, length, cursor, stats.link, sizeof(stats.link));
        stats.speed_mbps = parse_decimal_token(line, length, cursor);
        stats.mac_valid = parse_decimal_token(line, length, cursor);
        stats.rx_packets = parse_decimal_token(line, length, cursor);
        stats.rx_bytes = parse_decimal_token(line, length, cursor);
        stats.tx_packets = parse_decimal_token(line, length, cursor);
        stats.tx_completed = parse_decimal_token(line, length, cursor);
        stats.rx_drops = parse_decimal_token(line, length, cursor);
        stats.tx_errors = parse_decimal_token(line, length, cursor);
        found = true;
    });
    return found;
}

void append_mac(char* line, uint64_t capacity, uint64_t& cursor, uint64_t mac) {
    for (int shift = 40; shift >= 0; shift -= 8) {
        if (shift != 40) hybrid::user::append_char(line, capacity, cursor, ':');
        uint64_t byte = (mac >> shift) & 0xff;
        hybrid::user::append_char(line, capacity, cursor, hybrid::user::hex_digit(byte >> 4));
        hybrid::user::append_char(line, capacity, cursor, hybrid::user::hex_digit(byte));
    }
}

void write_iface_line(const NetStats& stats) {
    char line[192];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[ifconfig] ");
    hybrid::user::append_text(line, sizeof(line), cursor, stats.iface);
    hybrid::user::append_text(line, sizeof(line), cursor, " flags=");
    hybrid::user::append_text(line, sizeof(line), cursor, starts_with(stats.link, hybrid::user::strlen(stats.link), "up") ? "UP,RUNNING" : "DOWN");
    hybrid::user::append_text(line, sizeof(line), cursor, " mtu 1500");
    hybrid::user::write_line(line);
}

void write_link_line(const NetStats& stats) {
    char line[192];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[ifconfig] ether ");
    if (stats.mac_valid != 0) append_mac(line, sizeof(line), cursor, stats.mac_address);
    else hybrid::user::append_text(line, sizeof(line), cursor, "none");
    hybrid::user::append_text(line, sizeof(line), cursor, " speed ");
    append_decimal(line, sizeof(line), cursor, stats.speed_mbps);
    hybrid::user::append_text(line, sizeof(line), cursor, "mbps");
    hybrid::user::write_line(line);
}

void write_packet_line(const NetStats& stats) {
    char line[192];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[ifconfig] RX packets ");
    append_decimal(line, sizeof(line), cursor, stats.rx_packets);
    hybrid::user::append_text(line, sizeof(line), cursor, " bytes ");
    append_decimal(line, sizeof(line), cursor, stats.rx_bytes);
    hybrid::user::append_text(line, sizeof(line), cursor, " dropped ");
    append_decimal(line, sizeof(line), cursor, stats.rx_drops);
    hybrid::user::write_line(line);

    cursor = 0;
    line[0] = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[ifconfig] TX packets ");
    append_decimal(line, sizeof(line), cursor, stats.tx_packets);
    hybrid::user::append_text(line, sizeof(line), cursor, " completed ");
    append_decimal(line, sizeof(line), cursor, stats.tx_completed);
    hybrid::user::append_text(line, sizeof(line), cursor, " errors ");
    append_decimal(line, sizeof(line), cursor, stats.tx_errors);
    hybrid::user::write_line(line);
}

int main_result() {
    if (!read_file("/proc/net/summary", g_summary) || !read_file("/proc/net/dev", g_dev)) {
        hybrid::user::write_line("[ifconfig] read error");
        return 1;
    }

    NetStats stats{};
    parse_summary(stats);
    if (!parse_dev(stats)) {
        hybrid::user::write_line("[ifconfig] no interfaces");
        return 2;
    }

    write_iface_line(stats);
    hybrid::user::write_line("[ifconfig] inet none");
    write_link_line(stats);
    write_packet_line(stats);
    return 0;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::user::exit(static_cast<uint64_t>(main_result()));
}
