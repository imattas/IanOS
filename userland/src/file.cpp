#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

void clear(void* ptr, uint64_t size) {
    auto* bytes = reinterpret_cast<unsigned char*>(ptr);
    for (uint64_t i = 0; i < size; ++i) bytes[i] = 0;
}

bool is_printable(unsigned char value) {
    return value == '\n' || value == '\r' || value == '\t' || (value >= 32 && value < 127);
}

bool starts_with(const char* text, const char* prefix) {
    if (!text || !prefix) return false;
    for (uint64_t i = 0; prefix[i] != 0; ++i) {
        if (text[i] != prefix[i]) return false;
    }
    return true;
}

bool read_prefix(const char* path, unsigned char* buffer, uint64_t capacity, uint64_t& out_size, uint64_t& error) {
    out_size = 0;
    error = hybrid::kSyscallErrorNone;
    auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open,
                                        reinterpret_cast<uint64_t>(path),
                                        hybrid::user::strlen(path) + 1);
    if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
        error = opened.error;
        return false;
    }
    while (out_size < capacity) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::Read,
                                          opened.value,
                                          reinterpret_cast<uint64_t>(buffer + out_size),
                                          capacity - out_size);
        if (read.value == 0 && (read.error == hybrid::kSyscallErrorNone || read.error == hybrid::kSyscallErrorNotFound)) break;
        if (read.error != hybrid::kSyscallErrorNone) {
            error = read.error;
            hybrid::user::syscall(hybrid::SyscallNumber::Close, opened.value);
            return false;
        }
        if (read.value == 0) break;
        out_size += read.value;
    }
    hybrid::user::syscall(hybrid::SyscallNumber::Close, opened.value);
    return true;
}

const char* classify_file(const hybrid::VfsStatInfo& info, const unsigned char* prefix, uint64_t size) {
    if (info.type == hybrid::VfsNodeType::Directory || (info.flags & hybrid::VfsNodeDirectory)) return "directory";
    if (info.type == hybrid::VfsNodeType::CharacterDevice || (info.flags & hybrid::VfsNodeCharacterDevice)) return "character device";
    if (info.type == hybrid::VfsNodeType::VirtualFile || (info.flags & hybrid::VfsNodeVirtual)) return "virtual text";
    if (starts_with(info.path, "/proc/")) return "virtual text";
    if (size >= 4 && prefix[0] == 0x7f && prefix[1] == 'E' && prefix[2] == 'L' && prefix[3] == 'F') {
        if (size >= 20 && prefix[4] == 2 && prefix[5] == 1 && prefix[18] == 0x3e && prefix[19] == 0) {
            return "ELF64 x86_64 executable";
        }
        return "ELF executable";
    }
    if (size == 0 && info.size == 0) return "empty";
    uint64_t printable = 0;
    for (uint64_t i = 0; i < size; ++i) {
        if (is_printable(prefix[i])) ++printable;
    }
    if (size == 0 || printable * 100 >= size * 90) return "ASCII text";
    return "binary data";
}

void describe(const char* path) {
    hybrid::VfsStatInfo info;
    clear(&info, sizeof(info));
    auto stat = hybrid::user::syscall(hybrid::SyscallNumber::VfsStatInfo,
                                      reinterpret_cast<uint64_t>(path),
                                      hybrid::user::strlen(path) + 1,
                                      reinterpret_cast<uint64_t>(&info));
    if (stat.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_text_line("[file] ", "path ", path);
        hybrid::user::write_hex_line("[file] ", "error ", stat.error);
        return;
    }

    unsigned char prefix[64];
    uint64_t prefix_size = 0;
    uint64_t read_error = hybrid::kSyscallErrorNone;
    clear(prefix, sizeof(prefix));
    if (info.type == hybrid::VfsNodeType::MemoryFile || info.type == hybrid::VfsNodeType::VirtualFile) {
        read_prefix(path, prefix, sizeof(prefix), prefix_size, read_error);
    }

    hybrid::user::write_text_line("[file] ", "path ", info.path);
    hybrid::user::write_text_line("[file] ", "type ", classify_file(info, prefix, prefix_size));
    hybrid::user::write_hex_line("[file] ", "size ", info.size);
    if (read_error != hybrid::kSyscallErrorNone) hybrid::user::write_hex_line("[file] ", "read error ", read_error);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo path;
    if (!get_arg(1, path)) {
        hybrid::user::write_line("[file] usage file <path> [path...]");
        hybrid::user::exit(1);
    }

    uint64_t processed = 0;
    for (uint64_t index = 1; get_arg(index, path); ++index) {
        describe(path.value);
        ++processed;
    }
    hybrid::user::write_hex_line("[file] ", "files ", processed);
    hybrid::user::exit(processed == 0 ? 1 : 0);
}
