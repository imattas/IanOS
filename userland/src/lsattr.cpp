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

void append_flag(char* buffer, uint64_t capacity, uint64_t& cursor, uint32_t flags, uint32_t bit, char set_char) {
    hybrid::user::append_char(buffer, capacity, cursor, (flags & bit) != 0 ? set_char : '-');
}

bool describe(const char* path) {
    hybrid::VfsStatInfo info;
    clear(&info, sizeof(info));
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::VfsStatInfo,
                                        reinterpret_cast<uint64_t>(path),
                                        hybrid::user::strlen(path) + 1,
                                        reinterpret_cast<uint64_t>(&info));
    if (result.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_text_line("[lsattr] ", "path ", path);
        hybrid::user::write_hex_line("[lsattr] ", "error ", result.error);
        return false;
    }

    char line[128];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[lsattr] attrs ");
    append_flag(line, sizeof(line), cursor, info.flags, hybrid::VfsNodeReadable, 'r');
    append_flag(line, sizeof(line), cursor, info.flags, hybrid::VfsNodeWritable, 'w');
    append_flag(line, sizeof(line), cursor, info.flags, hybrid::VfsNodeDirectory, 'd');
    append_flag(line, sizeof(line), cursor, info.flags, hybrid::VfsNodeMemoryBacked, 'm');
    append_flag(line, sizeof(line), cursor, info.flags, hybrid::VfsNodeDiskBacked, 'k');
    append_flag(line, sizeof(line), cursor, info.flags, hybrid::VfsNodeCharacterDevice, 'c');
    append_flag(line, sizeof(line), cursor, info.flags, hybrid::VfsNodeVirtual, 'v');
    hybrid::user::append_text(line, sizeof(line), cursor, " ");
    hybrid::user::append_text(line, sizeof(line), cursor, info.path);
    hybrid::user::write_line(line);
    hybrid::user::write_text_line("[lsattr] ", "path ", info.path);
    hybrid::user::write_hex_line("[lsattr] ", "flags ", info.flags);
    return true;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo path;
    if (!get_arg(1, path)) {
        hybrid::user::write_line("[lsattr] usage lsattr <path> [path...]");
        hybrid::user::exit(1);
    }

    uint64_t processed = 0;
    uint64_t failures = 0;
    for (uint64_t index = 1; get_arg(index, path); ++index) {
        if (describe(path.value)) ++processed;
        else ++failures;
    }
    hybrid::user::write_hex_line("[lsattr] ", "files ", processed);
    hybrid::user::write_hex_line("[lsattr] ", "failures ", failures);
    hybrid::user::exit(failures != 0 || processed == 0 ? 1 : 0);
}
