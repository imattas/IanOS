#include "hybrid/user.hpp"

namespace {

struct Buffer {
    char bytes[1024];
    uint64_t length;
};

struct DevRow {
    char iface[16];
    char link[16];
    uint64_t speed_mbps;
    uint64_t mac_valid;
};

struct RouteRow {
    char iface[16];
    char flags[8];
    uint64_t use;
    uint64_t metric;
    uint64_t mtu;
};

char g_chunk[32];
Buffer g_dev;
Buffer g_route;

bool streq(const char* left, const char* right) {
    uint64_t i = 0;
    while (left[i] != 0 && right[i] != 0) {
        if (left[i] != right[i]) return false;
        ++i;
    }
    return left[i] == 0 && right[i] == 0;
}

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

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
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
    uint64_t value = 0;
    for (uint64_t i = 0; token[i] != 0; ++i) {
        if (token[i] < '0' || token[i] > '9') break;
        value = value * 10 + static_cast<uint64_t>(token[i] - '0');
    }
    return value;
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

bool load_dev_row(DevRow& row) {
    bool found = false;
    for_each_line(g_dev, [&](const char* line, uint64_t length) {
        if (found || !starts_with(line, length, "eth0 ")) return;
        uint64_t cursor = 0;
        copy_token(line, length, cursor, row.iface, sizeof(row.iface));
        copy_token(line, length, cursor, row.link, sizeof(row.link));
        row.speed_mbps = parse_decimal_token(line, length, cursor);
        row.mac_valid = parse_decimal_token(line, length, cursor);
        found = true;
    });
    return found;
}

bool load_route_row(RouteRow& row) {
    bool found = false;
    for_each_line(g_route, [&](const char* line, uint64_t length) {
        if (found || !starts_with(line, length, "eth0 ")) return;
        uint64_t cursor = 0;
        char discard[16];
        copy_token(line, length, cursor, row.iface, sizeof(row.iface));
        copy_token(line, length, cursor, discard, sizeof(discard));
        copy_token(line, length, cursor, discard, sizeof(discard));
        copy_token(line, length, cursor, row.flags, sizeof(row.flags));
        parse_decimal_token(line, length, cursor);
        row.use = parse_decimal_token(line, length, cursor);
        row.metric = parse_decimal_token(line, length, cursor);
        copy_token(line, length, cursor, discard, sizeof(discard));
        row.mtu = parse_decimal_token(line, length, cursor);
        found = true;
    });
    return found;
}

void write_link(const DevRow& row) {
    char line[160];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[ip] link ");
    hybrid::user::append_text(line, sizeof(line), cursor, row.iface);
    hybrid::user::append_text(line, sizeof(line), cursor, " state ");
    hybrid::user::append_text(line, sizeof(line), cursor, row.link);
    hybrid::user::append_text(line, sizeof(line), cursor, " speed ");
    append_decimal(line, sizeof(line), cursor, row.speed_mbps);
    hybrid::user::append_text(line, sizeof(line), cursor, "mbps mac-valid ");
    append_decimal(line, sizeof(line), cursor, row.mac_valid);
    hybrid::user::write_line(line);
}

void write_addr(const DevRow& row) {
    char line[144];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[ip] addr ");
    hybrid::user::append_text(line, sizeof(line), cursor, row.iface);
    hybrid::user::append_text(line, sizeof(line), cursor, " inet none scope link");
    hybrid::user::write_line(line);
}

void write_route(const RouteRow& row) {
    char line[192];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[ip] route default dev ");
    hybrid::user::append_text(line, sizeof(line), cursor, row.iface);
    hybrid::user::append_text(line, sizeof(line), cursor, " flags ");
    hybrid::user::append_text(line, sizeof(line), cursor, row.flags);
    hybrid::user::append_text(line, sizeof(line), cursor, " metric ");
    append_decimal(line, sizeof(line), cursor, row.metric);
    hybrid::user::append_text(line, sizeof(line), cursor, " mtu ");
    append_decimal(line, sizeof(line), cursor, row.mtu);
    hybrid::user::append_text(line, sizeof(line), cursor, " use ");
    append_decimal(line, sizeof(line), cursor, row.use);
    hybrid::user::write_line(line);
}

int main_result() {
    if (!read_file("/proc/net/dev", g_dev) || !read_file("/proc/net/route", g_route)) {
        hybrid::user::write_line("[ip] read error");
        return 1;
    }

    DevRow dev{};
    RouteRow route{};
    const bool have_dev = load_dev_row(dev);
    const bool have_route = load_route_row(route);
    if (!have_dev) {
        hybrid::user::write_line("[ip] no interfaces");
        return 2;
    }

    hybrid::ArgumentInfo arg{};
    const bool have_arg = get_arg(1, arg);
    if (!have_arg) {
        write_link(dev);
        write_addr(dev);
        if (have_route) write_route(route);
        return have_route ? 0 : 3;
    }

    if (streq(arg.value, "link")) {
        write_link(dev);
        return 0;
    }
    if (streq(arg.value, "addr")) {
        write_addr(dev);
        return 0;
    }
    if (streq(arg.value, "route")) {
        if (have_route) {
            write_route(route);
            return 0;
        }
        hybrid::user::write_line("[ip] route empty");
        return 3;
    }

    hybrid::user::write_line("[ip] usage: ip [link|addr|route]");
    return 4;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::user::exit(static_cast<uint64_t>(main_result()));
}
