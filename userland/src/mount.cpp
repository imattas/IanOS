#include "hybrid/user.hpp"

namespace {

const char* flag_text(uint32_t flags) {
    if ((flags & hybrid::MountDiskBacked) != 0 && (flags & hybrid::MountReadOnly) != 0) return "ro,disk";
    if ((flags & hybrid::MountMemoryBacked) != 0 && (flags & hybrid::MountWritable) != 0) return "rw,mem";
    if ((flags & hybrid::MountDiskBacked) != 0) return "disk";
    if ((flags & hybrid::MountMemoryBacked) != 0) return "mem";
    return "none";
}

void print_mount(const hybrid::MountInfo& info) {
    char line[192];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[mount] ");
    hybrid::user::append_text(line, sizeof(line), cursor, info.source);
    hybrid::user::append_text(line, sizeof(line), cursor, " on ");
    hybrid::user::append_text(line, sizeof(line), cursor, info.path);
    hybrid::user::append_text(line, sizeof(line), cursor, " type ");
    hybrid::user::append_text(line, sizeof(line), cursor, info.fs_type);
    hybrid::user::append_text(line, sizeof(line), cursor, " flags ");
    hybrid::user::append_text(line, sizeof(line), cursor, flag_text(info.flags));
    hybrid::user::write_line(line);
    hybrid::user::write_hex_line("[mount] ", "nodes ", info.node_count);
    hybrid::user::write_hex_line("[mount] ", "bytes ", info.total_bytes);
}

}

extern "C" [[noreturn]] void _start() {
    auto count = hybrid::user::syscall(hybrid::SyscallNumber::GetMountCount);
    if (count.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[mount] ", "count error ", count.error);
        hybrid::user::exit(1);
    }

    hybrid::user::write_hex_line("[mount] ", "count ", count.value);
    bool saw_root = false;
    bool saw_boot = false;
    for (uint64_t i = 0; i < count.value; ++i) {
        hybrid::MountInfo info;
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetMountInfo, i, reinterpret_cast<uint64_t>(&info));
        if (result.error != hybrid::kSyscallErrorNone) {
            hybrid::user::write_hex_line("[mount] ", "info error ", result.error);
            hybrid::user::exit(2);
        }
        print_mount(info);
        if (info.path[0] == '/' && info.path[1] == 0 && (info.flags & hybrid::MountMemoryBacked) != 0) saw_root = true;
        if (info.path[0] == '/' && info.path[1] == 'm' && (info.flags & hybrid::MountDiskBacked) != 0) saw_boot = true;
    }

    hybrid::user::exit((saw_root && saw_boot) ? 0 : 3);
}
