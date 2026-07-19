#include "hybrid/user.hpp"

namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

bool streq(const char* left, const char* right) {
    uint64_t i = 0;
    while (left[i] != 0 && right[i] != 0) {
        if (left[i] != right[i]) return false;
        ++i;
    }
    return left[i] == 0 && right[i] == 0;
}

hybrid::SyscallResult read_blocking(uint64_t fd, unsigned char* buffer, uint64_t size) {
    for (;;) {
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::Read, fd, reinterpret_cast<uint64_t>(buffer), size);
        if (result.error != hybrid::kSyscallErrorWouldBlock) return result;
        hybrid::user::syscall(hybrid::SyscallNumber::Yield);
    }
}

void emit_line(const char* text) {
    hybrid::user::write_text("[base64] ");
    hybrid::user::write_line(text);
}

void emit_output_char(char (&line)[80], uint64_t& used, char value, uint64_t& produced) {
    if (used + 1 >= sizeof(line)) {
        line[used] = 0;
        emit_line(line);
        used = 0;
    }
    line[used++] = value;
    line[used] = 0;
    ++produced;
}

void encode_triplet(const unsigned char* in, uint64_t count, char (&line)[80], uint64_t& used, uint64_t& produced) {
    const uint32_t a = count > 0 ? in[0] : 0;
    const uint32_t b = count > 1 ? in[1] : 0;
    const uint32_t c = count > 2 ? in[2] : 0;
    const uint32_t bits = (a << 16) | (b << 8) | c;
    emit_output_char(line, used, kAlphabet[(bits >> 18) & 0x3f], produced);
    emit_output_char(line, used, kAlphabet[(bits >> 12) & 0x3f], produced);
    emit_output_char(line, used, count > 1 ? kAlphabet[(bits >> 6) & 0x3f] : '=', produced);
    emit_output_char(line, used, count > 2 ? kAlphabet[bits & 0x3f] : '=', produced);
}

int decode_value(char value) {
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return 26 + value - 'a';
    if (value >= '0' && value <= '9') return 52 + value - '0';
    if (value == '+') return 62;
    if (value == '/') return 63;
    if (value == '=') return -2;
    if (value == '\r' || value == '\n' || value == '\t' || value == ' ') return -3;
    return -1;
}

void emit_decoded_byte(char (&line)[80], uint64_t& used, unsigned char value, uint64_t& produced) {
    if (value == '\r') return;
    if (value == '\n') {
        line[used] = 0;
        emit_line(line);
        used = 0;
        ++produced;
        return;
    }
    emit_output_char(line, used, static_cast<char>(value), produced);
}

bool decode_quad(const char (&quad)[4], char (&line)[80], uint64_t& used, uint64_t& produced) {
    int values[4];
    for (uint64_t i = 0; i < 4; ++i) {
        values[i] = decode_value(quad[i]);
        if (values[i] == -3 || values[i] == -1) return false;
    }
    if (values[0] < 0 || values[1] < 0) return false;
    if (values[2] == -2 && values[3] != -2) return false;
    const uint32_t bits = (static_cast<uint32_t>(values[0]) << 18) |
                          (static_cast<uint32_t>(values[1]) << 12) |
                          (static_cast<uint32_t>(values[2] < 0 ? 0 : values[2]) << 6) |
                          static_cast<uint32_t>(values[3] < 0 ? 0 : values[3]);
    emit_decoded_byte(line, used, static_cast<unsigned char>((bits >> 16) & 0xff), produced);
    if (values[2] != -2) emit_decoded_byte(line, used, static_cast<unsigned char>((bits >> 8) & 0xff), produced);
    if (values[3] != -2) emit_decoded_byte(line, used, static_cast<unsigned char>(bits & 0xff), produced);
    return true;
}

void write_stat(const char* label, uint64_t value) {
    hybrid::user::write_hex_line("[base64] ", label, value);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo arg;
    bool decode = false;
    uint64_t path_index = 1;
    if (get_arg(1, arg) && streq(arg.value, "-d")) {
        decode = true;
        path_index = 2;
    }

    hybrid::ArgumentInfo path;
    uint64_t fd = hybrid::kStdinFd;
    bool close_when_done = false;
    const bool has_path = get_arg(path_index, path);
    if (has_path) {
        auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open, reinterpret_cast<uint64_t>(path.value), hybrid::user::strlen(path.value) + 1);
        if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
            hybrid::user::write_hex_line("[base64] ", "open error ", opened.error);
            hybrid::user::exit(2);
        }
        fd = opened.value;
        close_when_done = true;
    }

    hybrid::user::write_text_line("[base64] ", "path ", has_path ? path.value : "<stdin>");
    hybrid::user::write_text_line("[base64] ", "mode ", decode ? "decode" : "encode");

    uint64_t consumed = 0;
    uint64_t produced = 0;
    char line[80];
    uint64_t line_used = 0;
    line[0] = 0;
    unsigned char buffer[48];

    if (!decode) {
        unsigned char carry[3];
        uint64_t carry_used = 0;
        for (;;) {
            auto read = read_blocking(fd, buffer, sizeof(buffer));
            if (read.error != hybrid::kSyscallErrorNone || read.value == 0) break;
            consumed += read.value;
            uint64_t index = 0;
            while (index < read.value) {
                carry[carry_used++] = buffer[index++];
                if (carry_used == 3) {
                    encode_triplet(carry, 3, line, line_used, produced);
                    carry_used = 0;
                }
            }
        }
        if (carry_used != 0) encode_triplet(carry, carry_used, line, line_used, produced);
        if (line_used != 0) emit_line(line);
    } else {
        char quad[4];
        uint64_t quad_used = 0;
        bool invalid = false;
        for (;;) {
            auto read = read_blocking(fd, buffer, sizeof(buffer));
            if (read.error != hybrid::kSyscallErrorNone || read.value == 0) break;
            for (uint64_t i = 0; i < read.value; ++i) {
                const int value = decode_value(static_cast<char>(buffer[i]));
                if (value == -3) continue;
                if (value == -1) {
                    invalid = true;
                    break;
                }
                quad[quad_used++] = static_cast<char>(buffer[i]);
                if (quad_used == 4) {
                    if (!decode_quad(quad, line, line_used, produced)) invalid = true;
                    quad_used = 0;
                    if (invalid) break;
                }
            }
            consumed += read.value;
            if (invalid) break;
        }
        if (!invalid && quad_used != 0) invalid = true;
        if (invalid) {
            if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);
            hybrid::user::write_line("[base64] decode error");
            hybrid::user::exit(3);
        }
        if (line_used != 0) emit_line(line);
    }

    if (close_when_done) hybrid::user::syscall(hybrid::SyscallNumber::Close, fd);
    write_stat("bytes ", consumed);
    write_stat("output ", produced);
    hybrid::user::exit(produced);
}
