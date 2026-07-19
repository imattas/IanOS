#include "hybrid/user.hpp"

namespace {

constexpr uint32_t kSha256Initial[8] = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
};

constexpr uint32_t kSha256Round[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

struct Sha256 {
    uint32_t state[8];
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

uint32_t rotr(uint32_t value, uint32_t bits) {
    return (value >> bits) | (value << (32 - bits));
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

void init(Sha256& ctx) {
    for (uint64_t i = 0; i < 8; ++i) ctx.state[i] = kSha256Initial[i];
    for (uint64_t i = 0; i < sizeof(ctx.block); ++i) ctx.block[i] = 0;
    ctx.block_used = 0;
    ctx.bytes = 0;
}

void transform(Sha256& ctx, const unsigned char* block) {
    uint32_t w[64];
    for (uint64_t i = 0; i < 16; ++i) w[i] = load_be32(block + i * 4);
    for (uint64_t i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = ctx.state[0];
    uint32_t b = ctx.state[1];
    uint32_t c = ctx.state[2];
    uint32_t d = ctx.state[3];
    uint32_t e = ctx.state[4];
    uint32_t f = ctx.state[5];
    uint32_t g = ctx.state[6];
    uint32_t h = ctx.state[7];

    for (uint64_t i = 0; i < 64; ++i) {
        const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t temp1 = h + s1 + ch + kSha256Round[i] + w[i];
        const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx.state[0] += a;
    ctx.state[1] += b;
    ctx.state[2] += c;
    ctx.state[3] += d;
    ctx.state[4] += e;
    ctx.state[5] += f;
    ctx.state[6] += g;
    ctx.state[7] += h;
}

void update(Sha256& ctx, const unsigned char* data, uint64_t length) {
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

void finish(Sha256& ctx, unsigned char digest[32]) {
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
    for (uint64_t i = 0; i < 8; ++i) {
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

void write_digest(const unsigned char digest[32], const char* path) {
    char line[128];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[sha256sum] ");
    for (uint64_t i = 0; i < 32; ++i) {
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
            hybrid::user::write_hex_line("[sha256sum] ", "open error ", opened.error);
            hybrid::user::exit(2);
        }
        fd = opened.value;
        close_when_done = true;
    }

    Sha256 ctx;
    init(ctx);
    unsigned char buffer[32];
    for (;;) {
        auto read = read_blocking(fd, buffer, sizeof(buffer));
        if (read.error == hybrid::kSyscallErrorNotFound && read.value == 0) break;
        if (read.error != hybrid::kSyscallErrorNone) {
            if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);
            hybrid::user::write_hex_line("[sha256sum] ", "read error ", read.error);
            hybrid::user::exit(3);
        }
        if (read.value == 0) break;
        update(ctx, buffer, read.value);
    }
    if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);

    unsigned char digest[32];
    finish(ctx, digest);
    write_digest(digest, label);
    hybrid::user::write_hex_line("[sha256sum] ", "bytes ", ctx.bytes);
    hybrid::user::exit(0);
}
