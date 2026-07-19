#include "hybrid/user.hpp"

namespace {

constexpr uint64_t kSha512Initial[8] = {
    0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull,
    0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
    0x510e527fade682d1ull, 0x9b05688c2b3e6c1full,
    0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull,
};

constexpr uint64_t kSha512Round[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
    0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
    0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
    0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
    0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
    0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
    0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,
};

struct Sha512 {
    uint64_t state[8];
    unsigned char block[128];
    uint64_t block_used;
    uint64_t bytes_low;
    uint64_t bytes_high;
};

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

uint64_t rotr(uint64_t value, uint64_t bits) {
    return (value >> bits) | (value << (64 - bits));
}

uint64_t load_be64(const unsigned char* bytes) {
    uint64_t value = 0;
    for (uint64_t i = 0; i < 8; ++i) value = (value << 8) | bytes[i];
    return value;
}

void store_be64(unsigned char* out, uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        out[i] = static_cast<unsigned char>(value & 0xffu);
        value >>= 8;
    }
}

void init(Sha512& ctx) {
    for (uint64_t i = 0; i < 8; ++i) ctx.state[i] = kSha512Initial[i];
    for (uint64_t i = 0; i < sizeof(ctx.block); ++i) ctx.block[i] = 0;
    ctx.block_used = 0;
    ctx.bytes_low = 0;
    ctx.bytes_high = 0;
}

void add_bytes(Sha512& ctx, uint64_t length) {
    const uint64_t old = ctx.bytes_low;
    ctx.bytes_low += length;
    if (ctx.bytes_low < old) ++ctx.bytes_high;
}

void transform(Sha512& ctx, const unsigned char* block) {
    uint64_t w[80];
    for (uint64_t i = 0; i < 16; ++i) w[i] = load_be64(block + i * 8);
    for (uint64_t i = 16; i < 80; ++i) {
        const uint64_t s0 = rotr(w[i - 15], 1) ^ rotr(w[i - 15], 8) ^ (w[i - 15] >> 7);
        const uint64_t s1 = rotr(w[i - 2], 19) ^ rotr(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint64_t a = ctx.state[0];
    uint64_t b = ctx.state[1];
    uint64_t c = ctx.state[2];
    uint64_t d = ctx.state[3];
    uint64_t e = ctx.state[4];
    uint64_t f = ctx.state[5];
    uint64_t g = ctx.state[6];
    uint64_t h = ctx.state[7];

    for (uint64_t i = 0; i < 80; ++i) {
        const uint64_t s1 = rotr(e, 14) ^ rotr(e, 18) ^ rotr(e, 41);
        const uint64_t ch = (e & f) ^ ((~e) & g);
        const uint64_t temp1 = h + s1 + ch + kSha512Round[i] + w[i];
        const uint64_t s0 = rotr(a, 28) ^ rotr(a, 34) ^ rotr(a, 39);
        const uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint64_t temp2 = s0 + maj;
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

void update(Sha512& ctx, const unsigned char* data, uint64_t length) {
    add_bytes(ctx, length);
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

void finish(Sha512& ctx, unsigned char digest[64]) {
    const uint64_t bit_low = ctx.bytes_low << 3;
    const uint64_t bit_high = (ctx.bytes_high << 3) | (ctx.bytes_low >> 61);
    ctx.block[ctx.block_used++] = 0x80;
    if (ctx.block_used > 112) {
        while (ctx.block_used < sizeof(ctx.block)) ctx.block[ctx.block_used++] = 0;
        transform(ctx, ctx.block);
        ctx.block_used = 0;
    }
    while (ctx.block_used < 112) ctx.block[ctx.block_used++] = 0;
    store_be64(ctx.block + 112, bit_high);
    store_be64(ctx.block + 120, bit_low);
    transform(ctx, ctx.block);
    for (uint64_t i = 0; i < 8; ++i) store_be64(digest + i * 8, ctx.state[i]);
}

hybrid::SyscallResult read_blocking(uint64_t fd, unsigned char* buffer, uint64_t size) {
    for (;;) {
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::Read, fd, reinterpret_cast<uint64_t>(buffer), size);
        if (result.error != hybrid::kSyscallErrorWouldBlock) return result;
        hybrid::user::syscall(hybrid::SyscallNumber::Yield);
    }
}

void write_digest(const unsigned char digest[64], const char* path) {
    char line[192];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[sha512sum] ");
    for (uint64_t i = 0; i < 64; ++i) {
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
            hybrid::user::write_hex_line("[sha512sum] ", "open error ", opened.error);
            hybrid::user::exit(2);
        }
        fd = opened.value;
        close_when_done = true;
    }

    Sha512 ctx;
    init(ctx);
    unsigned char buffer[64];
    for (;;) {
        auto read = read_blocking(fd, buffer, sizeof(buffer));
        if (read.error == hybrid::kSyscallErrorNotFound && read.value == 0) break;
        if (read.error != hybrid::kSyscallErrorNone) {
            if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);
            hybrid::user::write_hex_line("[sha512sum] ", "read error ", read.error);
            hybrid::user::exit(3);
        }
        if (read.value == 0) break;
        update(ctx, buffer, read.value);
    }
    if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);

    unsigned char digest[64];
    finish(ctx, digest);
    write_digest(digest, label);
    hybrid::user::write_hex_line("[sha512sum] ", "bytes ", ctx.bytes_low);
    hybrid::user::exit(0);
}
