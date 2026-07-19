#include "hybrid/user.hpp"

namespace {

const char* flag_text(uint32_t flags) {
    if ((flags & hybrid::MountDiskBacked) != 0 && (flags & hybrid::MountReadOnly) != 0) return "ro,disk";
    if ((flags & hybrid::MountMemoryBacked) != 0 && (flags & hybrid::MountWritable) != 0) return "rw,mem";
    if ((flags & hybrid::MountDiskBacked) != 0) return "disk";
    if ((flags & hybrid::MountMemoryBacked) != 0) return "mem";
    return "none";
}

void print_summary(const hybrid::MountInfo& info) {
    char line[192];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[df] ");
    hybrid::user::append_text(line, sizeof(line), cursor, info.path);
    hybrid::user::append_text(line, sizeof(line), cursor, " ");
    hybrid::user::append_text(line, sizeof(line), cursor, info.fs_type);
    hybrid::user::append_text(line, sizeof(line), cursor, " ");
    hybrid::user::append_text(line, sizeof(line), cursor, info.source);
    hybrid::user::append_text(line, sizeof(line), cursor, " ");
    hybrid::user::append_text(line, sizeof(line), cursor, flag_text(info.flags));
    hybrid::user::write_line(line);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    auto count = hybrid::user::syscall(hybrid::SyscallNumber::GetMountCount);
    if (count.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[df] ", "count error ", count.error);
        hybrid::user::exit(1);
    }

    hybrid::user::write_hex_line("[df] ", "filesystems ", count.value);
    bool saw_disk = false;
    for (uint64_t i = 0; i < count.value; ++i) {
        hybrid::MountInfo info;
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetMountInfo, i, reinterpret_cast<uint64_t>(&info));
        if (result.error != hybrid::kSyscallErrorNone) {
            hybrid::user::write_hex_line("[df] ", "info error ", result.error);
            hybrid::user::exit(2);
        }
        print_summary(info);
        hybrid::user::write_hex_line("[df] ", "nodes ", info.node_count);
        hybrid::user::write_hex_line("[df] ", "used bytes ", info.total_bytes);
        if ((info.flags & hybrid::MountDiskBacked) != 0 && info.total_bytes != 0) saw_disk = true;
    }

    hybrid::user::exit(saw_disk ? 0 : 3);
}
