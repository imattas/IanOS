#include "hybrid/user.hpp"

namespace {

void clear(void* ptr, uint64_t size) {
    auto* bytes = reinterpret_cast<unsigned char*>(ptr);
    for (uint64_t i = 0; i < size; ++i) bytes[i] = 0;
}

bool streq(const char* left, const char* right) {
    uint64_t i = 0;
    while (left[i] != 0 && right[i] != 0) {
        if (left[i] != right[i]) return false;
        ++i;
    }
    return left[i] == 0 && right[i] == 0;
}

bool stat_path(const char* path, hybrid::VfsStatInfo& out) {
    clear(&out, sizeof(out));
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::VfsStatInfo,
                                        reinterpret_cast<uint64_t>(path),
                                        hybrid::user::strlen(path) + 1,
                                        reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone && result.value == 1;
}

bool find_mount(const char* path, hybrid::MountInfo& out) {
    clear(&out, sizeof(out));
    auto count = hybrid::user::syscall(hybrid::SyscallNumber::GetMountCount);
    if (count.error != hybrid::kSyscallErrorNone) return false;
    for (uint64_t i = 0; i < count.value; ++i) {
        hybrid::MountInfo info;
        clear(&info, sizeof(info));
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetMountInfo, i, reinterpret_cast<uint64_t>(&info));
        if (result.error != hybrid::kSyscallErrorNone) continue;
        if (streq(info.path, path)) {
            out = info;
            return true;
        }
    }
    return false;
}

void write_value(const char* label, uint64_t value) {
    hybrid::user::write_hex_line("[imginfo] ", label, value);
}

uint64_t add_file_size(const char* path, bool& ok) {
    hybrid::VfsStatInfo stat;
    if (!stat_path(path, stat)) {
        hybrid::user::write_text_line("[imginfo] ", "missing ", path);
        ok = false;
        return 0;
    }
    hybrid::user::write_text_line("[imginfo] ", "file ", path);
    write_value("size ", stat.size);
    return stat.size;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::MountInfo boot_mount;
    if (!find_mount("/mnt/boot", boot_mount)) {
        hybrid::user::write_line("[imginfo] missing /mnt/boot mount");
        hybrid::user::exit(1);
    }

    hybrid::SystemInfo system{};
    auto system_result = hybrid::user::syscall(hybrid::SyscallNumber::GetSystemInfo, reinterpret_cast<uint64_t>(&system));
    if (system_result.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[imginfo] ", "system error ", system_result.error);
        hybrid::user::exit(2);
    }

    hybrid::user::write_text_line("[imginfo] ", "mount ", boot_mount.path);
    hybrid::user::write_text_line("[imginfo] ", "type ", boot_mount.fs_type);
    hybrid::user::write_text_line("[imginfo] ", "source ", boot_mount.source);
    write_value("mounted_bytes ", boot_mount.total_bytes);
    write_value("nodes ", boot_mount.node_count);
    write_value("modules ", system.boot_module_count);

    bool ok = true;
    uint64_t known_bytes = 0;
    known_bytes += add_file_size("/mnt/boot/kernel.elf", ok);
    known_bytes += add_file_size("/mnt/boot/efi/boot/bootx64.efi", ok);
    known_bytes += add_file_size("/mnt/boot/user/init.elf", ok);
    known_bytes += add_file_size("/mnt/boot/bin/imginfo.elf", ok);
    write_value("known_bytes ", known_bytes);
    write_value("unlisted_bytes ", boot_mount.total_bytes > known_bytes ? boot_mount.total_bytes - known_bytes : 0);

    hybrid::user::exit(ok && boot_mount.total_bytes > known_bytes ? 0 : 3);
}
