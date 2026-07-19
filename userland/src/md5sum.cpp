#include "hybrid/user.hpp"

namespace {

constexpr uint32_t kMd5Shift[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

constexpr uint32_t kMd5Table[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
    0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
    0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
    0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
    0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
};

struct Md5 {
    uint32_t state[4];
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

uint32_t load_le32(const unsigned char* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

void store_le64(unsigned char* out, uint64_t value) {
    for (uint64_t i = 0; i < 8; ++i) {
        out[i] = static_cast<unsigned char>(value & 0xffu);
        value >>= 8;
    }
}

void init(Md5& ctx) {
    ctx.state[0] = 0x67452301u;
    ctx.state[1] = 0xefcdab89u;
    ctx.state[2] = 0x98badcfeu;
    ctx.state[3] = 0x10325476u;
    for (uint64_t i = 0; i < sizeof(ctx.block); ++i) ctx.block[i] = 0;
    ctx.block_used = 0;
    ctx.bytes = 0;
}

void transform(Md5& ctx, const unsigned char* block) {
    uint32_t word[16];
    for (uint64_t i = 0; i < 16; ++i) word[i] = load_le32(block + i * 4);

    uint32_t a = ctx.state[0];
    uint32_t b = ctx.state[1];
    uint32_t c = ctx.state[2];
    uint32_t d = ctx.state[3];

    for (uint64_t i = 0; i < 64; ++i) {
        uint32_t f = 0;
        uint64_t g = 0;
        if (i < 16) {
            f = (b & c) | ((~b) & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | ((~d) & c);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | (~d));
            g = (7 * i) & 15;
        }
        const uint32_t next = b + rotl(a + f + kMd5Table[i] + word[g], kMd5Shift[i]);
        a = d;
        d = c;
        c = b;
        b = next;
    }

    ctx.state[0] += a;
    ctx.state[1] += b;
    ctx.state[2] += c;
    ctx.state[3] += d;
}

void update(Md5& ctx, const unsigned char* data, uint64_t length) {
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

void finish(Md5& ctx, unsigned char digest[16]) {
    const uint64_t bit_count = ctx.bytes * 8;
    ctx.block[ctx.block_used++] = 0x80;
    if (ctx.block_used > 56) {
        while (ctx.block_used < sizeof(ctx.block)) ctx.block[ctx.block_used++] = 0;
        transform(ctx, ctx.block);
        ctx.block_used = 0;
    }
    while (ctx.block_used < 56) ctx.block[ctx.block_used++] = 0;
    store_le64(ctx.block + 56, bit_count);
    transform(ctx, ctx.block);
    for (uint64_t i = 0; i < 4; ++i) {
        digest[i * 4 + 0] = static_cast<unsigned char>(ctx.state[i] & 0xffu);
        digest[i * 4 + 1] = static_cast<unsigned char>((ctx.state[i] >> 8) & 0xffu);
        digest[i * 4 + 2] = static_cast<unsigned char>((ctx.state[i] >> 16) & 0xffu);
        digest[i * 4 + 3] = static_cast<unsigned char>((ctx.state[i] >> 24) & 0xffu);
    }
}

hybrid::SyscallResult read_blocking(uint64_t fd, unsigned char* buffer, uint64_t size) {
    for (;;) {
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::Read, fd, reinterpret_cast<uint64_t>(buffer), size);
        if (result.error != hybrid::kSyscallErrorWouldBlock) return result;
        hybrid::user::syscall(hybrid::SyscallNumber::Yield);
    }
}

void write_digest(const unsigned char digest[16], const char* path) {
    char line[128];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[md5sum] ");
    for (uint64_t i = 0; i < 16; ++i) {
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
            hybrid::user::write_hex_line("[md5sum] ", "open error ", opened.error);
            hybrid::user::exit(2);
        }
        fd = opened.value;
        close_when_done = true;
    }

    Md5 ctx;
    init(ctx);
    unsigned char buffer[32];
    for (;;) {
        auto read = read_blocking(fd, buffer, sizeof(buffer));
        if (read.error == hybrid::kSyscallErrorNotFound && read.value == 0) break;
        if (read.error != hybrid::kSyscallErrorNone) {
            if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);
            hybrid::user::write_hex_line("[md5sum] ", "read error ", read.error);
            hybrid::user::exit(3);
        }
        if (read.value == 0) break;
        update(ctx, buffer, read.value);
    }
    if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);

    unsigned char digest[16];
    finish(ctx, digest);
    write_digest(digest, label);
    hybrid::user::write_hex_line("[md5sum] ", "bytes ", ctx.bytes);
    hybrid::user::exit(0);
}
