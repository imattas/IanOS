#include "hybrid/user.hpp"

namespace {

struct Sha1 {
    uint32_t state[5];
    unsigned char block[64];
    uint64_t block_used;
    uint64_t bytes;
};

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

uint32_t rotl(uint32_t value, uint32_t bits) {
    return (value << bits) | (value >> (32 - bits));
}

uint32_t load_be32(const unsigned char* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

void store_be64(unsigned char* out, uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        out[i] = static_cast<unsigned char>(value & 0xffu);
        value >>= 8;
    }
}

void init(Sha1& ctx) {
    ctx.state[0] = 0x67452301u;
    ctx.state[1] = 0xefcdab89u;
    ctx.state[2] = 0x98badcfeu;
    ctx.state[3] = 0x10325476u;
    ctx.state[4] = 0xc3d2e1f0u;
    for (uint64_t i = 0; i < sizeof(ctx.block); ++i) ctx.block[i] = 0;
    ctx.block_used = 0;
    ctx.bytes = 0;
}

void transform(Sha1& ctx, const unsigned char* block) {
    uint32_t words[80];
    for (uint64_t i = 0; i < 16; ++i) words[i] = load_be32(block + i * 4);
    for (uint64_t i = 16; i < 80; ++i) words[i] = rotl(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);

    uint32_t a = ctx.state[0];
    uint32_t b = ctx.state[1];
    uint32_t c = ctx.state[2];
    uint32_t d = ctx.state[3];
    uint32_t e = ctx.state[4];

    for (uint64_t i = 0; i < 80; ++i) {
        uint32_t f = 0;
        uint32_t k = 0;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5a827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdcu;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6u;
        }
        const uint32_t temp = rotl(a, 5) + f + e + k + words[i];
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = temp;
    }

    ctx.state[0] += a;
    ctx.state[1] += b;
    ctx.state[2] += c;
    ctx.state[3] += d;
    ctx.state[4] += e;
}

void update(Sha1& ctx, const unsigned char* data, uint64_t length) {
    ctx.bytes += length;
    uint64_t consumed = 0;
    while (consumed < length) {
        const uint64_t room = sizeof(ctx.block) - ctx.block_used;
        const uint64_t take = (length - consumed) < room ? (length - consumed) : room;
        for (uint64_t i = 0; i < take; ++i) ctx.block[ctx.block_used + i] = data[consumed + i];
        ctx.block_used += take;
        consumed += take;
        if (ctx.block_used == sizeof(ctx.block)) {
            transform(ctx, ctx.block);
            ctx.block_used = 0;
        }
    }
}

void finish(Sha1& ctx, unsigned char digest[20]) {
    const uint64_t bit_count = ctx.bytes * 8;
    ctx.block[ctx.block_used++] = 0x80;
    if (ctx.block_used > 56) {
        while (ctx.block_used < sizeof(ctx.block)) ctx.block[ctx.block_used++] = 0;
        transform(ctx, ctx.block);
        ctx.block_used = 0;
    }
    while (ctx.block_used < 56) ctx.block[ctx.block_used++] = 0;
    store_be64(ctx.block + 56, bit_count);
    transform(ctx, ctx.block);
    for (uint64_t i = 0; i < 5; ++i) {
        digest[i * 4 + 0] = static_cast<unsigned char>(ctx.state[i] >> 24);
        digest[i * 4 + 1] = static_cast<unsigned char>(ctx.state[i] >> 16);
        digest[i * 4 + 2] = static_cast<unsigned char>(ctx.state[i] >> 8);
        digest[i * 4 + 3] = static_cast<unsigned char>(ctx.state[i]);
    }
}

hybrid::SyscallResult read_blocking(uint64_t fd, unsigned char* buffer, uint64_t size) {
    for (;;) {
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::Read, fd, reinterpret_cast<uint64_t>(buffer), size);
        if (result.error != hybrid::kSyscallErrorWouldBlock) return result;
        hybrid::user::syscall(hybrid::SyscallNumber::Yield);
    }
}

void write_digest(const unsigned char digest[20], const char* path) {
    char line[128];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[sha1sum] ");
    for (uint64_t i = 0; i < 20; ++i) {
        hybrid::user::append_char(line, sizeof(line), cursor, hybrid::user::hex_digit(digest[i] >> 4));
        hybrid::user::append_char(line, sizeof(line), cursor, hybrid::user::hex_digit(digest[i]));
    }
    hybrid::user::append_text(line, sizeof(line), cursor, "  ");
    hybrid::user::append_text(line, sizeof(line), cursor, path);
    hybrid::user::write_line(line);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo path{};
    const bool has_path = get_arg(1, path);
    uint64_t fd = hybrid::kStdinFd;
    bool close_when_done = false;
    const char* label = "<stdin>";
    if (has_path) {
        label = path.value;
        auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open,
                                            reinterpret_cast<uint64_t>(path.value),
                                            hybrid::user::strlen(path.value) + 1);
        if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
            hybrid::user::write_hex_line("[sha1sum] ", "open error ", opened.error);
            hybrid::user::exit(2);
        }
        fd = opened.value;
        close_when_done = true;
    }

    Sha1 ctx;
    init(ctx);
    unsigned char buffer[32];
    for (;;) {
        auto read = read_blocking(fd, buffer, sizeof(buffer));
        if (read.error == hybrid::kSyscallErrorNotFound && read.value == 0) break;
        if (read.error != hybrid::kSyscallErrorNone) {
            if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);
            hybrid::user::write_hex_line("[sha1sum] ", "read error ", read.error);
            hybrid::user::exit(3);
        }
        if (read.value == 0) break;
        update(ctx, buffer, read.value);
    }
    if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);

    unsigned char digest[20];
    finish(ctx, digest);
    write_digest(digest, label);
    hybrid::user::write_hex_line("[sha1sum] ", "bytes ", ctx.bytes);
    hybrid::user::exit(0);
}
