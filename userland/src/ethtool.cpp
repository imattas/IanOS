#include "hybrid/user.hpp"

namespace {

struct Buffer {
    char bytes[1024];
    uint64_t length;
};

struct EthtoolInfo {
    uint64_t present;
    char link[16];
    uint64_t speed_mbps;
    uint64_t full_duplex;
    uint64_t mac_valid;
    uint64_t mmio_mapped;
    uint64_t command_enabled;
    uint64_t rings_allocated;
    uint64_t rings_programmed;
    uint64_t ring_registers_verified;
    uint64_t tx_packets;
    uint64_t tx_completed;
    uint64_t rx_packets;
    uint64_t rx_bytes;
};

char g_chunk[32];
Buffer g_summary;

bool streq(const char* left, const char* right) {
    uint64_t i = 0;
    while (left[i] != 0 && right[i] != 0) {
        if (left[i] != right[i]) return false;
        ++i;
    }
    return left[i] == 0 && right[i] == 0;
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

void parse_summary(EthtoolInfo& info) {
    for_each_line(g_summary, [&](const char* line, uint64_t length) {
        uint64_t cursor = 0;
        char key[40];
        char value[24];
        copy_token(line, length, cursor, key, sizeof(key));
        copy_token(line, length, cursor, value, sizeof(value));
        if (streq(key, "e1000_present")) info.present = parse_decimal_text(value);
        else if (streq(key, "e1000_link")) {
            uint64_t out = 0;
            for (; value[out] != 0 && out + 1 < sizeof(info.link); ++out) info.link[out] = value[out];
            info.link[out] = 0;
        } else if (streq(key, "e1000_speed_mbps")) info.speed_mbps = parse_decimal_text(value);
        else if (streq(key, "e1000_full_duplex")) info.full_duplex = parse_decimal_text(value);
        else if (streq(key, "e1000_mac_valid")) info.mac_valid = parse_decimal_text(value);
        else if (streq(key, "e1000_mmio_mapped")) info.mmio_mapped = parse_decimal_text(value);
        else if (streq(key, "e1000_command_enabled")) info.command_enabled = parse_decimal_text(value);
        else if (streq(key, "e1000_rings_allocated")) info.rings_allocated = parse_decimal_text(value);
        else if (streq(key, "e1000_rings_programmed")) info.rings_programmed = parse_decimal_text(value);
        else if (streq(key, "e1000_ring_registers_verified")) info.ring_registers_verified = parse_decimal_text(value);
        else if (streq(key, "e1000_tx_packets")) info.tx_packets = parse_decimal_text(value);
        else if (streq(key, "e1000_tx_completed")) info.tx_completed = parse_decimal_text(value);
        else if (streq(key, "e1000_rx_packets")) info.rx_packets = parse_decimal_text(value);
        else if (streq(key, "e1000_rx_bytes")) info.rx_bytes = parse_decimal_text(value);
    });
}

const char* yes_no(uint64_t value) {
    return value != 0 ? "yes" : "no";
}

void write_decimal_line(const char* label, uint64_t value) {
    char line[128];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[ethtool] ");
    hybrid::user::append_text(line, sizeof(line), cursor, label);
    append_decimal(line, sizeof(line), cursor, value);
    hybrid::user::write_line(line);
}

int main_result() {
    if (!read_file("/proc/net/summary", g_summary)) {
        hybrid::user::write_line("[ethtool] read error");
        return 1;
    }

    EthtoolInfo info{};
    parse_summary(info);
    if (info.present == 0) {
        hybrid::user::write_line("[ethtool] no e1000 device");
        return 2;
    }

    hybrid::user::write_line("[ethtool] Settings for eth0:");
    hybrid::user::write_text_line("[ethtool] ", "Link detected: ", streq(info.link, "up") ? "yes" : "no");
    write_decimal_line("Speed: ", info.speed_mbps);
    hybrid::user::write_text_line("[ethtool] ", "Duplex: ", info.full_duplex != 0 ? "Full" : "Half");
    hybrid::user::write_text_line("[ethtool] ", "MAC valid: ", yes_no(info.mac_valid));
    hybrid::user::write_text_line("[ethtool] ", "MMIO mapped: ", yes_no(info.mmio_mapped));
    hybrid::user::write_text_line("[ethtool] ", "PCI command enabled: ", yes_no(info.command_enabled));
    hybrid::user::write_text_line("[ethtool] ", "Rings allocated: ", yes_no(info.rings_allocated));
    hybrid::user::write_text_line("[ethtool] ", "Rings programmed: ", yes_no(info.rings_programmed));
    hybrid::user::write_text_line("[ethtool] ", "Ring registers verified: ", yes_no(info.ring_registers_verified));
    write_decimal_line("TX packets: ", info.tx_packets);
    write_decimal_line("TX completed: ", info.tx_completed);
    write_decimal_line("RX packets: ", info.rx_packets);
    write_decimal_line("RX bytes: ", info.rx_bytes);
    return 0;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::user::exit(static_cast<uint64_t>(main_result()));
}
