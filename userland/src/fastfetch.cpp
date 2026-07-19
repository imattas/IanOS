#include "hybrid/user.hpp"

namespace {

constexpr uint64_t kCapacity = 96;

void append_decimal(char* buffer, uint64_t capacity, uint64_t& cursor, uint64_t value) {
    char digits[20];
    uint64_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && count < sizeof(digits));
    while (count != 0) hybrid::user::append_char(buffer, capacity, cursor, digits[--count]);
}

void write_fetch_line(const char* key, const char* value) {
    char line[kCapacity];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[fastfetch] ");
    hybrid::user::append_text(line, sizeof(line), cursor, key);
    hybrid::user::append_text(line, sizeof(line), cursor, ": ");
    hybrid::user::append_text(line, sizeof(line), cursor, value ? value : "");
    hybrid::user::write_line(line);
}

void write_fetch_hex(const char* key, uint64_t value) {
    char line[kCapacity];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[fastfetch] ");
    hybrid::user::append_text(line, sizeof(line), cursor, key);
    hybrid::user::append_text(line, sizeof(line), cursor, ": ");
    hybrid::user::append_hex(line, sizeof(line), cursor, value);
    hybrid::user::write_line(line);
}

void write_fetch_decimal(const char* key, uint64_t value) {
    char line[kCapacity];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[fastfetch] ");
    hybrid::user::append_text(line, sizeof(line), cursor, key);
    hybrid::user::append_text(line, sizeof(line), cursor, ": ");
    append_decimal(line, sizeof(line), cursor, value);
    hybrid::user::write_line(line);
}

void write_fetch_geometry(const hybrid::FramebufferInfo& fb) {
    char geometry[64];
    uint64_t cursor = 0;
    append_decimal(geometry, sizeof(geometry), cursor, fb.width);
    hybrid::user::append_char(geometry, sizeof(geometry), cursor, 'x');
    append_decimal(geometry, sizeof(geometry), cursor, fb.height);
    hybrid::user::append_char(geometry, sizeof(geometry), cursor, 'x');
    append_decimal(geometry, sizeof(geometry), cursor, fb.bytes_per_pixel * 8);
    write_fetch_line("Framebuffer", geometry);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::SystemInfo system{};
    hybrid::MemoryStatsInfo memory{};
    hybrid::FramebufferInfo fb{};

    auto system_result = hybrid::user::syscall(hybrid::SyscallNumber::GetSystemInfo, reinterpret_cast<uint64_t>(&system));
    auto memory_result = hybrid::user::syscall(hybrid::SyscallNumber::GetMemoryStats, reinterpret_cast<uint64_t>(&memory));
    auto cpu_count = hybrid::user::syscall(hybrid::SyscallNumber::GetCpuCount);
    auto device_count = hybrid::user::syscall(hybrid::SyscallNumber::GetDeviceCount);
    auto storage_count = hybrid::user::syscall(hybrid::SyscallNumber::GetStorageDeviceCount);
    auto network_count = hybrid::user::syscall(hybrid::SyscallNumber::GetNetworkDeviceCount);
    auto display_count = hybrid::user::syscall(hybrid::SyscallNumber::GetDisplayDeviceCount);
    auto fb_result = hybrid::user::syscall(hybrid::SyscallNumber::GetFramebufferInfo, reinterpret_cast<uint64_t>(&fb));

    if (system_result.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[fastfetch] ", "error ", system_result.error);
        hybrid::user::exit(1);
    }

    char os_line[64];
    uint64_t os_cursor = 0;
    hybrid::user::append_text(os_line, sizeof(os_line), os_cursor, system.sysname);
    hybrid::user::append_char(os_line, sizeof(os_line), os_cursor, ' ');
    hybrid::user::append_text(os_line, sizeof(os_line), os_cursor, system.release);

    write_fetch_line("OS", os_line);
    write_fetch_line("Kernel", system.kernel_type);
    write_fetch_line("Machine", system.machine);
    write_fetch_line("Boot", system.boot_mode);
    write_fetch_hex("Boot flags", system.boot_info_flags);
    write_fetch_decimal("Modules", system.boot_module_count);
    if (cpu_count.error == hybrid::kSyscallErrorNone) write_fetch_decimal("CPUs", cpu_count.value);
    if (memory_result.error == hybrid::kSyscallErrorNone) {
        write_fetch_decimal("Memory free KiB", memory.free_pages * 4);
        write_fetch_decimal("Memory used KiB", memory.used_pages * 4);
        write_fetch_decimal("Usable KiB", memory.usable_bytes / 1024);
    }
    if (fb_result.error == hybrid::kSyscallErrorNone) write_fetch_geometry(fb);
    if (device_count.error == hybrid::kSyscallErrorNone) write_fetch_decimal("Devices", device_count.value);
    if (storage_count.error == hybrid::kSyscallErrorNone) write_fetch_decimal("Storage", storage_count.value);
    if (network_count.error == hybrid::kSyscallErrorNone) write_fetch_decimal("Network", network_count.value);
    if (display_count.error == hybrid::kSyscallErrorNone) write_fetch_decimal("Display", display_count.value);

    hybrid::user::exit(0);
}
