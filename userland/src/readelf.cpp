#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

uint16_t read16(const unsigned char* data, uint64_t offset) {
    return static_cast<uint16_t>(data[offset]) |
           static_cast<uint16_t>(static_cast<uint16_t>(data[offset + 1]) << 8);
}

uint32_t read32(const unsigned char* data, uint64_t offset) {
    return static_cast<uint32_t>(data[offset]) |
           (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}

uint64_t read64(const unsigned char* data, uint64_t offset) {
    return static_cast<uint64_t>(read32(data, offset)) |
           (static_cast<uint64_t>(read32(data, offset + 4)) << 32);
}

const char* elf_type(uint16_t value) {
    if (value == 1) return "relocatable";
    if (value == 2) return "executable";
    if (value == 3) return "shared";
    if (value == 4) return "core";
    return "unknown";
}

const char* machine_name(uint16_t value) {
    if (value == 0x3e) return "x86_64";
    if (value == 0x03) return "x86";
    if (value == 0xb7) return "aarch64";
    return "unknown";
}

void write_kv_text(const char* key, const char* value) {
    hybrid::user::write_text_line("[readelf] ", key, value);
}

void write_kv_hex(const char* key, uint64_t value) {
    hybrid::user::write_hex_line("[readelf] ", key, value);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo path{};
    if (!get_arg(1, path)) {
        hybrid::user::write_line("[readelf] usage readelf <path>");
        hybrid::user::exit(1);
    }

    auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open,
                                        reinterpret_cast<uint64_t>(path.value),
                                        hybrid::user::strlen(path.value) + 1);
    if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
        hybrid::user::write_hex_line("[readelf] ", "open error ", opened.error);
        hybrid::user::exit(2);
    }

    unsigned char header[64];
    uint64_t got = 0;
    while (got < sizeof(header)) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::Read,
                                          opened.value,
                                          reinterpret_cast<uint64_t>(header + got),
                                          sizeof(header) - got);
        if (read.error != hybrid::kSyscallErrorNone) {
            hybrid::user::write_hex_line("[readelf] ", "read error ", read.error);
            hybrid::user::syscall(hybrid::SyscallNumber::Close, opened.value);
            hybrid::user::exit(3);
        }
        if (read.value == 0) break;
        got += read.value;
    }
    hybrid::user::syscall(hybrid::SyscallNumber::Close, opened.value);

    write_kv_text("path ", path.value);
    if (got < sizeof(header)) {
        write_kv_hex("header-bytes ", got);
        hybrid::user::write_line("[readelf] error short file");
        hybrid::user::exit(4);
    }
    if (header[0] != 0x7f || header[1] != 'E' || header[2] != 'L' || header[3] != 'F') {
        hybrid::user::write_line("[readelf] error bad magic");
        hybrid::user::exit(5);
    }
    if (header[4] != 2 || header[5] != 1) {
        hybrid::user::write_hex_line("[readelf] ", "class ", header[4]);
        hybrid::user::write_hex_line("[readelf] ", "data ", header[5]);
        hybrid::user::write_line("[readelf] error unsupported elf");
        hybrid::user::exit(6);
    }

    const uint16_t type = read16(header, 16);
    const uint16_t machine = read16(header, 18);
    const uint32_t version = read32(header, 20);
    const uint64_t entry = read64(header, 24);
    const uint64_t phoff = read64(header, 32);
    const uint64_t shoff = read64(header, 40);
    const uint16_t ehsize = read16(header, 52);
    const uint16_t phentsize = read16(header, 54);
    const uint16_t phnum = read16(header, 56);
    const uint16_t shentsize = read16(header, 58);
    const uint16_t shnum = read16(header, 60);
    const uint16_t shstrndx = read16(header, 62);

    write_kv_text("class ", "ELF64");
    write_kv_text("data ", "little-endian");
    write_kv_text("type ", elf_type(type));
    write_kv_text("machine ", machine_name(machine));
    write_kv_hex("version ", version);
    write_kv_hex("entry ", entry);
    write_kv_hex("phoff ", phoff);
    write_kv_hex("shoff ", shoff);
    write_kv_hex("ehsize ", ehsize);
    write_kv_hex("phentsize ", phentsize);
    write_kv_hex("phnum ", phnum);
    write_kv_hex("shentsize ", shentsize);
    write_kv_hex("shnum ", shnum);
    write_kv_hex("shstrndx ", shstrndx);
    hybrid::user::exit(0);
}
