#include "hybrid/user.hpp"

namespace {

void append_decimal(char* buffer, uint64_t capacity, uint64_t& cursor, uint64_t value) {
    char digits[20];
    uint64_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && count < sizeof(digits));
    while (count != 0) hybrid::user::append_char(buffer, capacity, cursor, digits[--count]);
}

void write_mount_row(const hybrid::MountInfo& mount) {
    char line[192];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[lsblk] mount ");
    hybrid::user::append_text(line, sizeof(line), cursor, mount.source);
    hybrid::user::append_text(line, sizeof(line), cursor, " ");
    hybrid::user::append_text(line, sizeof(line), cursor, mount.path);
    hybrid::user::append_text(line, sizeof(line), cursor, " ");
    hybrid::user::append_text(line, sizeof(line), cursor, mount.fs_type);
    hybrid::user::append_text(line, sizeof(line), cursor, " bytes ");
    append_decimal(line, sizeof(line), cursor, mount.total_bytes);
    hybrid::user::write_line(line);
}

bool is_boot_disk_mount(const hybrid::MountInfo& mount) {
    return (mount.flags & hybrid::MountDiskBacked) != 0 &&
        mount.source[0] == 'b' && mount.source[1] == 'o' && mount.source[2] == 'o' && mount.source[3] == 't';
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::BlockDeviceInfo block{};
    auto block_result = hybrid::user::syscall(hybrid::SyscallNumber::GetBlockDeviceInfo, reinterpret_cast<uint64_t>(&block));
    if (block_result.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[lsblk] ", "block error ", block_result.error);
        hybrid::user::exit(1);
    }

    uint64_t sectors = block.sector_count != 0 ? block.sector_count : block.last_lba + 1;
    uint64_t bytes = sectors * block.sector_size;
    hybrid::user::write_line("[lsblk] NAME TYPE SIZE SECTORS MOUNTPOINT");

    char disk_line[192];
    uint64_t cursor = 0;
    hybrid::user::append_text(disk_line, sizeof(disk_line), cursor, "[lsblk] bootdisk disk ");
    append_decimal(disk_line, sizeof(disk_line), cursor, bytes);
    hybrid::user::append_char(disk_line, sizeof(disk_line), cursor, ' ');
    append_decimal(disk_line, sizeof(disk_line), cursor, sectors);
    hybrid::user::append_text(disk_line, sizeof(disk_line), cursor, " /mnt/boot");
    hybrid::user::write_line(disk_line);

    hybrid::user::write_hex_line("[lsblk] ", "initialized ", block.initialized);
    hybrid::user::write_hex_line("[lsblk] ", "sector size ", block.sector_size);
    hybrid::user::write_hex_line("[lsblk] ", "sector count ", block.sector_count);
    hybrid::user::write_hex_line("[lsblk] ", "last lba ", block.last_lba);
    hybrid::user::write_hex_line("[lsblk] ", "reads ", block.sector_reads);
    hybrid::user::write_hex_line("[lsblk] ", "cache hits ", block.cache_hits);
    hybrid::user::write_hex_line("[lsblk] ", "cache misses ", block.cache_misses);

    auto mount_count = hybrid::user::syscall(hybrid::SyscallNumber::GetMountCount);
    if (mount_count.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[lsblk] ", "mount count error ", mount_count.error);
        hybrid::user::exit(2);
    }

    bool saw_boot_mount = false;
    for (uint64_t i = 0; i < mount_count.value; ++i) {
        hybrid::MountInfo mount{};
        auto mount_result = hybrid::user::syscall(hybrid::SyscallNumber::GetMountInfo, i, reinterpret_cast<uint64_t>(&mount));
        if (mount_result.error != hybrid::kSyscallErrorNone) {
            hybrid::user::write_hex_line("[lsblk] ", "mount info error ", mount_result.error);
            hybrid::user::exit(3);
        }
        if (!is_boot_disk_mount(mount)) continue;
        saw_boot_mount = true;
        write_mount_row(mount);
    }

    hybrid::user::exit(block.initialized != 0 && block.sector_size != 0 && saw_boot_mount ? 0 : 4);
}
