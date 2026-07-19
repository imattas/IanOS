#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

bool text_equals(const char* a, const char* b) {
    uint64_t i = 0;
    for (;; ++i) {
        if (a[i] != b[i]) return false;
        if (a[i] == 0) return true;
    }
}

bool starts_with_path(const char* path, const char* prefix) {
    if (!prefix || prefix[0] == 0) return true;
    uint64_t i = 0;
    for (; prefix[i] != 0; ++i) {
        if (path[i] != prefix[i]) return false;
    }
    return path[i] == 0 || prefix[i - 1] == '/' || path[i] == '/';
}

const char* flag_text(uint32_t flags) {
    if ((flags & hybrid::MountDiskBacked) != 0 && (flags & hybrid::MountReadOnly) != 0) return "ro,disk";
    if ((flags & hybrid::MountMemoryBacked) != 0 && (flags & hybrid::MountWritable) != 0) return "rw,mem";
    if ((flags & hybrid::MountDiskBacked) != 0) return "disk";
    if ((flags & hybrid::MountMemoryBacked) != 0) return "mem";
    return "none";
}

void write_mount(const hybrid::MountInfo& info) {
    char line[224];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[findmnt] target=");
    hybrid::user::append_text(line, sizeof(line), cursor, info.path);
    hybrid::user::append_text(line, sizeof(line), cursor, " source=");
    hybrid::user::append_text(line, sizeof(line), cursor, info.source);
    hybrid::user::append_text(line, sizeof(line), cursor, " fstype=");
    hybrid::user::append_text(line, sizeof(line), cursor, info.fs_type);
    hybrid::user::append_text(line, sizeof(line), cursor, " flags=");
    hybrid::user::append_text(line, sizeof(line), cursor, flag_text(info.flags));
    hybrid::user::write_line(line);
    hybrid::user::write_hex_line("[findmnt] ", "nodes ", info.node_count);
    hybrid::user::write_hex_line("[findmnt] ", "bytes ", info.total_bytes);
}

int main_result() {
    hybrid::ArgumentInfo filter_arg;
    const char* filter = "";
    if (get_arg(1, filter_arg)) filter = filter_arg.value;

    auto count = hybrid::user::syscall(hybrid::SyscallNumber::GetMountCount);
    if (count.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[findmnt] ", "count error ", count.error);
        return 1;
    }

    hybrid::user::write_hex_line("[findmnt] ", "filesystems ", count.value);
    if (filter[0] != 0) hybrid::user::write_text_line("[findmnt] ", "filter ", filter);

    uint64_t matches = 0;
    for (uint64_t i = 0; i < count.value; ++i) {
        hybrid::MountInfo info;
        auto* bytes = reinterpret_cast<unsigned char*>(&info);
        for (uint64_t j = 0; j < sizeof(info); ++j) bytes[j] = 0;
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetMountInfo, i, reinterpret_cast<uint64_t>(&info));
        if (result.error != hybrid::kSyscallErrorNone) {
            hybrid::user::write_hex_line("[findmnt] ", "info error ", result.error);
            return 2;
        }
        if (filter[0] != 0 && !starts_with_path(info.path, filter) && !text_equals(info.source, filter)) continue;
        write_mount(info);
        ++matches;
    }

    hybrid::user::write_hex_line("[findmnt] ", "matches ", matches);
    return matches == 0 ? 3 : 0;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::user::exit(static_cast<uint64_t>(main_result()));
}
