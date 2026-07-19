#include "hk/fs/vfs.hpp"
#include "hk/apic/apic.hpp"
#include "hk/block/block_device.hpp"
#include "hk/console.hpp"
#include "hk/cpu/runtime.hpp"
#include "hk/cpu/topology.hpp"
#include "hk/drivers/ahci.hpp"
#include "hk/drivers/driver_manager.hpp"
#include "hk/drivers/e1000.hpp"
#include "hk/interrupts/irq.hpp"
#include "hk/lib/string.hpp"
#include "hk/log.hpp"
#include "hk/mm/heap.hpp"
#include "hk/mm/pmm.hpp"
#include "hk/mm/vmm.hpp"
#include "hk/pci/pci.hpp"
#include "hk/drivers/ps2_keyboard.hpp"
#include "hk/sched/scheduler.hpp"
#include "hk/smp/smp.hpp"
#include "hk/terminal.hpp"
#include "hk/timer/pit.hpp"
#include "hk/userspace/userspace.hpp"
#include "hybrid/version.hpp"

namespace hk::fs {
namespace {
constexpr char kEtcOsRelease[] =
    "NAME=" HYBRID_OS_NAME "\n"
    "VERSION=" HYBRID_OS_VERSION "\n"
    "ID=" HYBRID_OS_ID "\n"
    "PRETTY_NAME=\"" HYBRID_OS_NAME " " HYBRID_OS_VERSION "\"\n";

constexpr char kEtcHostname[] =
    HYBRID_OS_ID "\n";

constexpr char kProcVersion[] =
    HYBRID_PROC_VERSION "\n";

constexpr char kProcKernelOstype[] =
    HYBRID_OS_NAME "\n";

constexpr char kProcKernelOsrelease[] =
    HYBRID_OS_RELEASE "\n";

constexpr char kProcCpuInfo[] =
    "processor\t: 0\n"
    "vendor_id\t: QEMU64\n"
    "model name\t: x86_64 virtual CPU\n"
    "arch\t\t: x86_64\n";

constexpr uint32_t kMaxMountedFatNodes = 160;
constexpr uint64_t kVirtualFileScratchBytes = 8192;
char mounted_fat_paths[kMaxMountedFatNodes][64]{};
uint32_t mounted_fat_path_count = 0;
const hybrid::BootModule* boot_module_table = nullptr;
uint64_t boot_module_count = 0;

struct Fat16Geometry {
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t root_entries;
    uint16_t fat_sectors;
    uint32_t total_sectors;
    uint32_t root_sector;
    uint32_t root_dir_sectors;
    uint32_t data_sector;
};

uint16_t read_le16(const unsigned char* bytes, uint32_t offset) {
    return static_cast<uint16_t>(bytes[offset] | (static_cast<uint16_t>(bytes[offset + 1]) << 8));
}

uint32_t read_le32(const unsigned char* bytes, uint32_t offset) {
    return static_cast<uint32_t>(bytes[offset]) |
        (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
        (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
        (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

bool parse_fat16_geometry(const unsigned char* sector, Fat16Geometry& geometry) {
    if (!sector || sector[510] != 0x55 || sector[511] != 0xaa) return false;
    if (sector[54] != 'F' || sector[55] != 'A' || sector[56] != 'T' || sector[57] != '1' || sector[58] != '6') return false;
    geometry.bytes_per_sector = read_le16(sector, 11);
    geometry.sectors_per_cluster = sector[13];
    geometry.reserved_sectors = read_le16(sector, 14);
    geometry.fat_count = sector[16];
    geometry.root_entries = read_le16(sector, 17);
    uint16_t total16 = read_le16(sector, 19);
    geometry.total_sectors = total16 != 0 ? total16 : read_le32(sector, 32);
    geometry.fat_sectors = read_le16(sector, 22);
    if (geometry.bytes_per_sector != 512 || geometry.sectors_per_cluster == 0 ||
        geometry.reserved_sectors == 0 || geometry.fat_count == 0 ||
        geometry.root_entries == 0 || geometry.fat_sectors == 0 ||
        geometry.total_sectors == 0) {
        return false;
    }
    geometry.root_dir_sectors = ((static_cast<uint32_t>(geometry.root_entries) * 32u) + 511u) / 512u;
    geometry.root_sector = geometry.reserved_sectors + static_cast<uint32_t>(geometry.fat_count) * geometry.fat_sectors;
    geometry.data_sector = geometry.root_sector + geometry.root_dir_sectors;
    return geometry.data_sector < geometry.total_sectors;
}

char* reserve_mounted_path(const char* prefix, const char* name) {
    if (mounted_fat_path_count >= kMaxMountedFatNodes) return nullptr;
    char* out = mounted_fat_paths[mounted_fat_path_count++];
    uint64_t cursor = 0;
    for (uint64_t i = 0; prefix && prefix[i] != 0 && cursor + 1 < 64; ++i) out[cursor++] = prefix[i];
    if (cursor + 1 < 64 && cursor > 1 && out[cursor - 1] != '/') out[cursor++] = '/';
    for (uint64_t i = 0; name && name[i] != 0 && cursor + 1 < 64; ++i) out[cursor++] = name[i];
    out[cursor] = 0;
    for (++cursor; cursor < 64; ++cursor) out[cursor] = 0;
    return out[0] == 0 ? nullptr : out;
}

char lower_ascii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool fat_short_name(const unsigned char* entry, char (&out)[16]) {
    if (!entry || entry[0] == 0 || entry[0] == 0xe5 || entry[0] == '.') return false;
    uint64_t cursor = 0;
    for (uint64_t i = 0; i < 8 && entry[i] != ' '; ++i) out[cursor++] = lower_ascii(static_cast<char>(entry[i]));
    bool has_ext = false;
    for (uint64_t i = 8; i < 11; ++i) if (entry[i] != ' ') has_ext = true;
    if (has_ext && cursor + 1 < sizeof(out)) out[cursor++] = '.';
    for (uint64_t i = 8; i < 11 && entry[i] != ' ' && cursor + 1 < sizeof(out); ++i) out[cursor++] = lower_ascii(static_cast<char>(entry[i]));
    out[cursor] = 0;
    return cursor != 0;
}

void append_char(char* buffer, uint64_t capacity, uint64_t& cursor, char value) {
    if (cursor + 1 >= capacity) return;
    buffer[cursor++] = value;
    buffer[cursor] = 0;
}

void append_text(char* buffer, uint64_t capacity, uint64_t& cursor, const char* text) {
    if (!text) return;
    for (uint64_t i = 0; text[i] != 0; ++i) append_char(buffer, capacity, cursor, text[i]);
}

void append_decimal(char* buffer, uint64_t capacity, uint64_t& cursor, uint64_t value) {
    char digits[21];
    uint64_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && count < sizeof(digits));
    while (count > 0) append_char(buffer, capacity, cursor, digits[--count]);
}

void append_module_line(char* out, uint64_t capacity, uint64_t& cursor, const hybrid::BootModule& module);

const char* pci_bar_type_name(hk::pci::BarType type) {
    switch (type) {
    case hk::pci::BarType::Io: return "io";
    case hk::pci::BarType::Mmio32: return "mmio32";
    case hk::pci::BarType::Mmio64: return "mmio64";
    default: return "none";
    }
}

const char* pci_driver_kind_name(hk::pci::DriverKind kind) {
    switch (kind) {
    case hk::pci::DriverKind::Bridge: return "bridge";
    case hk::pci::DriverKind::Ahci: return "ahci";
    case hk::pci::DriverKind::E1000: return "e1000";
    case hk::pci::DriverKind::Vga: return "vga";
    case hk::pci::DriverKind::Smbus: return "smbus";
    default: return "unknown";
    }
}

const char* driver_device_state_name(hk::drivers::DeviceState state) {
    switch (state) {
    case hk::drivers::DeviceState::Started: return "started";
    case hk::drivers::DeviceState::Failed: return "failed";
    default: return "bound";
    }
}

const char* link_state_name(bool up) {
    return up ? "up" : "down";
}

const char* terminal_input_mode_name(hybrid::TerminalInputMode mode) {
    switch (mode) {
    case hybrid::TerminalInputMode::Canonical: return "canonical";
    default: return "raw";
    }
}

const char* cpu_run_state_name(hk::cpu::CpuRunState state) {
    switch (state) {
    case hk::cpu::CpuRunState::Bootstrap: return "bootstrap";
    case hk::cpu::CpuRunState::Parked: return "parked";
    case hk::cpu::CpuRunState::Scheduler: return "scheduler";
    default: return "offline";
    }
}

void append_cpu_topology_line(char* out, uint64_t capacity, uint64_t& cursor, const hk::cpu::CpuState& cpu) {
    hk::cpu::CpuRuntimeState runtime{};
    const bool has_runtime = hk::cpu::runtime().copy_state(cpu.cpu_id, runtime);
    append_text(out, capacity, cursor, "cpu ");
    append_decimal(out, capacity, cursor, cpu.cpu_id);
    append_text(out, capacity, cursor, " apic ");
    append_decimal(out, capacity, cursor, cpu.apic_id);
    append_text(out, capacity, cursor, " acpi ");
    append_decimal(out, capacity, cursor, cpu.acpi_processor_id);
    append_text(out, capacity, cursor, " enabled ");
    append_decimal(out, capacity, cursor, cpu.enabled ? 1 : 0);
    append_text(out, capacity, cursor, " online ");
    append_decimal(out, capacity, cursor, cpu.online ? 1 : 0);
    append_text(out, capacity, cursor, " bootstrap ");
    append_decimal(out, capacity, cursor, cpu.bootstrap ? 1 : 0);
    append_text(out, capacity, cursor, " startup_attempted ");
    append_decimal(out, capacity, cursor, cpu.startup_attempted ? 1 : 0);
    append_text(out, capacity, cursor, " state ");
    append_text(out, capacity, cursor, has_runtime ? cpu_run_state_name(runtime.state) : "unknown");
    append_text(out, capacity, cursor, " descriptors_ready ");
    append_decimal(out, capacity, cursor, has_runtime && runtime.descriptors_ready ? 1 : 0);
    append_text(out, capacity, cursor, " lapic_timer_ready ");
    append_decimal(out, capacity, cursor, has_runtime && runtime.local_apic_timer_ready ? 1 : 0);
    append_text(out, capacity, cursor, " bootstrap_work_done ");
    append_decimal(out, capacity, cursor, has_runtime && runtime.bootstrap_work_done ? 1 : 0);
    append_text(out, capacity, cursor, " ipi_work_done ");
    append_decimal(out, capacity, cursor, has_runtime && runtime.ipi_work_done ? 1 : 0);
    append_text(out, capacity, cursor, " scheduler_ticks ");
    append_decimal(out, capacity, cursor, has_runtime ? runtime.scheduler_ticks : 0);
    append_text(out, capacity, cursor, " interrupts ");
    append_decimal(out, capacity, cursor, has_runtime ? runtime.interrupts : 0);
    append_text(out, capacity, cursor, " work_counter ");
    append_decimal(out, capacity, cursor, hk::smp::work_counter(cpu.cpu_id));
    append_text(out, capacity, cursor, " tlb_shootdowns ");
    append_decimal(out, capacity, cursor, hk::smp::tlb_shootdown_counter(cpu.cpu_id));
    append_char(out, capacity, cursor, '\n');
}

void append_pci_device_line(char* out, uint64_t capacity, uint64_t& cursor, const hk::pci::Device& device) {
    append_text(out, capacity, cursor, "bdf ");
    append_decimal(out, capacity, cursor, device.bus);
    append_char(out, capacity, cursor, ':');
    append_decimal(out, capacity, cursor, device.device);
    append_char(out, capacity, cursor, '.');
    append_decimal(out, capacity, cursor, device.function);
    append_text(out, capacity, cursor, " vendor ");
    append_decimal(out, capacity, cursor, device.vendor_id);
    append_text(out, capacity, cursor, " device ");
    append_decimal(out, capacity, cursor, device.device_id);
    append_text(out, capacity, cursor, " class ");
    append_decimal(out, capacity, cursor, device.class_code);
    append_text(out, capacity, cursor, " subclass ");
    append_decimal(out, capacity, cursor, device.subclass);
    append_text(out, capacity, cursor, " prog_if ");
    append_decimal(out, capacity, cursor, device.prog_if);
    append_text(out, capacity, cursor, " command ");
    append_decimal(out, capacity, cursor, device.command);
    append_text(out, capacity, cursor, " status ");
    append_decimal(out, capacity, cursor, device.status);
    append_text(out, capacity, cursor, " driver ");
    append_text(out, capacity, cursor, pci_driver_kind_name(device.driver_kind));
    append_text(out, capacity, cursor, " bars ");
    append_decimal(out, capacity, cursor, device.bar_count);
    if (device.bar_count != 0) {
        append_text(out, capacity, cursor, " bar0 ");
        append_text(out, capacity, cursor, pci_bar_type_name(device.bars[0].type));
        append_text(out, capacity, cursor, " base ");
        append_decimal(out, capacity, cursor, device.bars[0].base);
        append_text(out, capacity, cursor, " size ");
        append_decimal(out, capacity, cursor, device.bars[0].size);
    }
    append_char(out, capacity, cursor, '\n');
}

void append_driver_device_line(char* out, uint64_t capacity, uint64_t& cursor, const hk::drivers::DriverDevice& device) {
    append_text(out, capacity, cursor, "driver ");
    append_text(out, capacity, cursor, device.driver_name ? device.driver_name : "unknown");
    append_text(out, capacity, cursor, " kind ");
    append_text(out, capacity, cursor, pci_driver_kind_name(device.kind));
    append_text(out, capacity, cursor, " bdf ");
    append_decimal(out, capacity, cursor, device.bus);
    append_char(out, capacity, cursor, ':');
    append_decimal(out, capacity, cursor, device.device);
    append_char(out, capacity, cursor, '.');
    append_decimal(out, capacity, cursor, device.function);
    append_text(out, capacity, cursor, " vendor ");
    append_decimal(out, capacity, cursor, device.vendor_id);
    append_text(out, capacity, cursor, " device ");
    append_decimal(out, capacity, cursor, device.device_id);
    append_text(out, capacity, cursor, " required_command_bits ");
    append_decimal(out, capacity, cursor, device.required_command_bits);
    append_text(out, capacity, cursor, " bus_master_required ");
    append_decimal(out, capacity, cursor, device.bus_master_required ? 1 : 0);
    append_text(out, capacity, cursor, " state ");
    append_text(out, capacity, cursor, driver_device_state_name(device.state));
    append_char(out, capacity, cursor, '\n');
}

void append_interrupt_line(char* out, uint64_t capacity, uint64_t& cursor, uint8_t vector, const char* source, const char* name) {
    append_decimal(out, capacity, cursor, vector);
    append_text(out, capacity, cursor, ": ");
    append_decimal(out, capacity, cursor, hk::interrupts::vector_dispatch_count(vector));
    append_text(out, capacity, cursor, " ");
    append_text(out, capacity, cursor, source);
    append_text(out, capacity, cursor, " ");
    append_text(out, capacity, cursor, name);
    append_char(out, capacity, cursor, '\n');
}

const char* proc_process_state_name(uint32_t state) {
    switch (state) {
    case 1: return "created";
    case 2: return "runnable";
    case 3: return "stopped";
    case 4: return "exited";
    default: return "empty";
    }
}

void append_proc_process_line(char* out, uint64_t capacity, uint64_t& cursor, const hybrid::ProcessInfo& process) {
    append_text(out, capacity, cursor, "pid ");
    append_decimal(out, capacity, cursor, process.pid);
    append_text(out, capacity, cursor, " ppid ");
    append_decimal(out, capacity, cursor, process.parent_pid);
    append_text(out, capacity, cursor, " pgid ");
    append_decimal(out, capacity, cursor, process.process_group_id);
    append_text(out, capacity, cursor, " state ");
    append_text(out, capacity, cursor, proc_process_state_name(process.state));
    append_text(out, capacity, cursor, " fds ");
    append_decimal(out, capacity, cursor, process.open_file_count);
    append_text(out, capacity, cursor, " ticks ");
    append_decimal(out, capacity, cursor, process.run_ticks);
    append_text(out, capacity, cursor, " name ");
    append_text(out, capacity, cursor, process.name);
    append_char(out, capacity, cursor, '\n');
}

const char* proc_fd_kind_name(hybrid::FileDescriptorInfoKind kind) {
    switch (kind) {
    case hybrid::FileDescriptorInfoKind::Vfs: return "vfs";
    case hybrid::FileDescriptorInfoKind::PipeRead: return "pipe-read";
    case hybrid::FileDescriptorInfoKind::PipeWrite: return "pipe-write";
    default: return "empty";
    }
}

void append_proc_fd_line(char* out, uint64_t capacity, uint64_t& cursor, const hybrid::FileDescriptorInfo& fd) {
    append_text(out, capacity, cursor, "fd ");
    append_decimal(out, capacity, cursor, fd.fd);
    append_text(out, capacity, cursor, " kind ");
    append_text(out, capacity, cursor, proc_fd_kind_name(fd.kind));
    if (fd.kind == hybrid::FileDescriptorInfoKind::Vfs) {
        append_text(out, capacity, cursor, " offset ");
        append_decimal(out, capacity, cursor, fd.offset);
        append_text(out, capacity, cursor, " path ");
        append_text(out, capacity, cursor, fd.path);
    } else if (fd.kind == hybrid::FileDescriptorInfoKind::PipeRead || fd.kind == hybrid::FileDescriptorInfoKind::PipeWrite) {
        append_text(out, capacity, cursor, " pipe ");
        append_decimal(out, capacity, cursor, fd.pipe_id);
    }
    append_char(out, capacity, cursor, '\n');
}

void append_mount_options(char* out, uint64_t capacity, uint64_t& cursor, uint32_t flags) {
    append_text(out, capacity, cursor, (flags & hybrid::MountReadOnly) != 0 ? "ro" : "rw");
    if ((flags & hybrid::MountMemoryBacked) != 0) append_text(out, capacity, cursor, ",mem");
    if ((flags & hybrid::MountDiskBacked) != 0) append_text(out, capacity, cursor, ",disk");
    if ((flags & hybrid::MountWritable) != 0 && (flags & hybrid::MountReadOnly) == 0) append_text(out, capacity, cursor, ",writable");
}

void append_mount_line(char* out, uint64_t capacity, uint64_t& cursor, const hybrid::MountInfo& mount) {
    append_text(out, capacity, cursor, mount.source);
    append_char(out, capacity, cursor, ' ');
    append_text(out, capacity, cursor, mount.path);
    append_char(out, capacity, cursor, ' ');
    append_text(out, capacity, cursor, mount.fs_type);
    append_char(out, capacity, cursor, ' ');
    append_mount_options(out, capacity, cursor, mount.flags);
    append_text(out, capacity, cursor, " 0 0\n");
}

bool append_decimal_text(char* out, uint64_t capacity, uint64_t& cursor, uint64_t value) {
    uint64_t before = cursor;
    append_decimal(out, capacity, cursor, value);
    return cursor != before;
}

bool parse_decimal_segment(const char* text, uint64_t length, uint64_t& out) {
    out = 0;
    if (!text || length == 0) return false;
    for (uint64_t i = 0; i < length; ++i) {
        char c = text[i];
        if (c < '0' || c > '9') return false;
        out = out * 10 + static_cast<uint64_t>(c - '0');
    }
    return out != 0;
}

bool text_equal(const char* left, const char* right) {
    if (!left || !right) return false;
    uint64_t i = 0;
    while (left[i] != 0 && right[i] != 0) {
        if (left[i] != right[i]) return false;
        ++i;
    }
    return left[i] == 0 && right[i] == 0;
}

bool process_info_by_pid(uint64_t pid, hybrid::ProcessInfo& out) {
    auto& manager = hk::userspace::userspace_manager();
    for (uint64_t i = 0; i < manager.process_count(); ++i) {
        hybrid::ProcessInfo process{};
        if (manager.copy_process_info(i, process) && process.pid == pid) {
            out = process;
            return true;
        }
    }
    return false;
}

enum class DynamicProcKind : uint8_t { None, PidDirectory, PidStatus, PidFd, PidFdEntry };

bool descriptor_by_pid_fd(uint64_t pid, uint32_t fd_number, hybrid::FileDescriptorInfo& out) {
    auto& manager = hk::userspace::userspace_manager();
    for (uint64_t i = 0; i < hk::userspace::kMaxProcessFileDescriptors; ++i) {
        hybrid::FileDescriptorInfo fd{};
        if (manager.copy_file_descriptor_info(pid, i, fd) && fd.fd == fd_number) {
            out = fd;
            return true;
        }
    }
    return false;
}

void copy_fd_target_text(char (&out)[64], const hybrid::FileDescriptorInfo& fd) {
    for (uint64_t i = 0; i < sizeof(out); ++i) out[i] = 0;
    if (fd.kind == hybrid::FileDescriptorInfoKind::Vfs) {
        uint64_t i = 0;
        for (; i + 1 < sizeof(out) && fd.path[i] != 0; ++i) out[i] = fd.path[i];
        out[i] = 0;
        return;
    }
    uint64_t cursor = 0;
    append_text(out, sizeof(out), cursor, fd.kind == hybrid::FileDescriptorInfoKind::PipeRead ? "pipe-read:" : "pipe-write:");
    append_decimal(out, sizeof(out), cursor, fd.pipe_id);
}

DynamicProcKind parse_dynamic_proc_path(const char* normalized, uint64_t& pid, uint32_t* fd_number = nullptr) {
    pid = 0;
    if (fd_number) *fd_number = 0;
    constexpr const char* prefix = "/proc/";
    uint64_t cursor = 0;
    while (prefix[cursor] != 0) {
        if (!normalized || normalized[cursor] != prefix[cursor]) return DynamicProcKind::None;
        ++cursor;
    }
    uint64_t digits_start = cursor;
    while (normalized[cursor] >= '0' && normalized[cursor] <= '9') ++cursor;
    if (!parse_decimal_segment(normalized + digits_start, cursor - digits_start, pid)) return DynamicProcKind::None;
    hybrid::ProcessInfo process{};
    if (!process_info_by_pid(pid, process)) return DynamicProcKind::None;
    if (normalized[cursor] == 0) return DynamicProcKind::PidDirectory;
    if (normalized[cursor++] != '/') return DynamicProcKind::None;
    if (text_equal(normalized + cursor, "status")) return DynamicProcKind::PidStatus;
    if (text_equal(normalized + cursor, "fd")) return DynamicProcKind::PidFd;
    if (normalized[cursor] == 'f' && normalized[cursor + 1] == 'd' && normalized[cursor + 2] == '/') {
        cursor += 3;
        uint64_t fd_value = 0;
        uint64_t fd_start = cursor;
        while (normalized[cursor] >= '0' && normalized[cursor] <= '9') ++cursor;
        if (normalized[cursor] != 0 || !parse_decimal_segment(normalized + fd_start, cursor - fd_start, fd_value) ||
            fd_value > 0xffffffffull) {
            return DynamicProcKind::None;
        }
        hybrid::FileDescriptorInfo fd{};
        if (!descriptor_by_pid_fd(pid, static_cast<uint32_t>(fd_value), fd)) return DynamicProcKind::None;
        if (fd_number) *fd_number = static_cast<uint32_t>(fd_value);
        return DynamicProcKind::PidFdEntry;
    }
    return DynamicProcKind::None;
}

void copy_proc_pid_path(char (&out)[64], uint64_t pid, const char* suffix) {
    for (uint64_t i = 0; i < sizeof(out); ++i) out[i] = 0;
    uint64_t cursor = 0;
    append_text(out, sizeof(out), cursor, "/proc/");
    append_decimal_text(out, sizeof(out), cursor, pid);
    if (suffix && suffix[0] != 0) {
        append_char(out, sizeof(out), cursor, '/');
        append_text(out, sizeof(out), cursor, suffix);
    }
}

void render_proc_status(uint64_t pid, char* out, uint64_t capacity) {
    uint64_t cursor = 0;
    out[0] = 0;
    hybrid::ProcessInfo process{};
    if (!process_info_by_pid(pid, process)) return;
    append_text(out, capacity, cursor, "Name:\t");
    append_text(out, capacity, cursor, process.name);
    append_text(out, capacity, cursor, "\nPid:\t");
    append_decimal(out, capacity, cursor, process.pid);
    append_text(out, capacity, cursor, "\nPPid:\t");
    append_decimal(out, capacity, cursor, process.parent_pid);
    append_text(out, capacity, cursor, "\nPGid:\t");
    append_decimal(out, capacity, cursor, process.process_group_id);
    append_text(out, capacity, cursor, "\nState:\t");
    append_text(out, capacity, cursor, proc_process_state_name(process.state));
    append_text(out, capacity, cursor, "\nFDs:\t");
    append_decimal(out, capacity, cursor, process.open_file_count);
    append_text(out, capacity, cursor, "\nThreads:\t1\nVmPages:\t");
    append_decimal(out, capacity, cursor, process.owned_page_count);
    append_text(out, capacity, cursor, "\nSyscalls:\t");
    append_decimal(out, capacity, cursor, process.syscall_count);
    append_text(out, capacity, cursor, "\nLastSyscall:\t");
    append_decimal(out, capacity, cursor, process.last_syscall);
    append_text(out, capacity, cursor, "\nRunTicks:\t");
    append_decimal(out, capacity, cursor, process.run_ticks);
    append_text(out, capacity, cursor, "\nSwitches:\t");
    append_decimal(out, capacity, cursor, process.switch_count);
    append_text(out, capacity, cursor, "\nPreempts:\t");
    append_decimal(out, capacity, cursor, process.preempt_count);
    append_char(out, capacity, cursor, '\n');
}

void render_proc_fd(uint64_t pid, char* out, uint64_t capacity) {
    uint64_t cursor = 0;
    out[0] = 0;
    append_text(out, capacity, cursor, "FD KIND TARGET\n");
    auto& manager = hk::userspace::userspace_manager();
    for (uint64_t i = 0; i < hk::userspace::kMaxProcessFileDescriptors; ++i) {
        hybrid::FileDescriptorInfo fd{};
        if (manager.copy_file_descriptor_info(pid, i, fd)) append_proc_fd_line(out, capacity, cursor, fd);
    }
}

uint64_t render_dynamic_proc_file(const char* normalized, char* out, uint64_t capacity) {
    if (!out || capacity == 0) return 0;
    uint64_t pid = 0;
    DynamicProcKind kind = parse_dynamic_proc_path(normalized, pid);
    if (kind == DynamicProcKind::PidStatus) {
        render_proc_status(pid, out, capacity);
        uint64_t length = 0;
        while (length < capacity && out[length] != 0) ++length;
        return length;
    }
    if (kind == DynamicProcKind::PidFd) {
        render_proc_fd(pid, out, capacity);
        uint64_t length = 0;
        while (length < capacity && out[length] != 0) ++length;
        return length;
    }
    if (kind == DynamicProcKind::PidFdEntry) {
        uint32_t fd_number = 0;
        parse_dynamic_proc_path(normalized, pid, &fd_number);
        hybrid::FileDescriptorInfo fd{};
        if (!descriptor_by_pid_fd(pid, fd_number, fd)) {
            out[0] = 0;
            return 0;
        }
        char target[64]{};
        copy_fd_target_text(target, fd);
        uint64_t cursor = 0;
        append_text(out, capacity, cursor, target);
        append_char(out, capacity, cursor, '\n');
        return cursor;
    }
    out[0] = 0;
    return 0;
}

uint64_t dynamic_proc_file_size(const char* normalized) {
    char scratch[2048];
    return render_dynamic_proc_file(normalized, scratch, sizeof(scratch));
}

uint64_t render_virtual_file(VirtualFileKind kind, char* out, uint64_t capacity) {
    if (!out || capacity == 0) return 0;
    uint64_t cursor = 0;
    out[0] = 0;
    switch (kind) {
    case VirtualFileKind::ProcMeminfo: {
        const auto stats = hk::mm::pmm().stats();
        append_text(out, capacity, cursor, "MemTotal: ");
        append_decimal(out, capacity, cursor, stats.usable_bytes);
        append_text(out, capacity, cursor, " B\nMemFree: ");
        append_decimal(out, capacity, cursor, stats.free_pages * hk::mm::kPageSize);
        append_text(out, capacity, cursor, " B\nMemUsed: ");
        append_decimal(out, capacity, cursor, stats.used_pages * hk::mm::kPageSize);
        append_text(out, capacity, cursor, " B\nPagesTotal: ");
        append_decimal(out, capacity, cursor, stats.total_pages);
        append_text(out, capacity, cursor, "\nPagesFree: ");
        append_decimal(out, capacity, cursor, stats.free_pages);
        append_text(out, capacity, cursor, "\nPagesUsed: ");
        append_decimal(out, capacity, cursor, stats.used_pages);
        append_char(out, capacity, cursor, '\n');
        break;
    }
    case VirtualFileKind::ProcUptime:
        append_text(out, capacity, cursor, "ticks ");
        append_decimal(out, capacity, cursor, hk::timer::ticks());
        append_char(out, capacity, cursor, '\n');
        break;
    case VirtualFileKind::ProcLoadavg: {
        auto& manager = hk::userspace::userspace_manager();
        uint64_t runnable = manager.runnable_thread_count();
        uint64_t live = manager.live_process_count();
        uint64_t total = manager.process_count();
        for (uint32_t i = 0; i < 3; ++i) {
            if (i != 0) append_char(out, capacity, cursor, ' ');
            append_decimal(out, capacity, cursor, runnable);
            append_text(out, capacity, cursor, ".00");
        }
        append_char(out, capacity, cursor, ' ');
        append_decimal(out, capacity, cursor, runnable);
        append_char(out, capacity, cursor, '/');
        append_decimal(out, capacity, cursor, live);
        append_char(out, capacity, cursor, ' ');
        append_decimal(out, capacity, cursor, total);
        append_char(out, capacity, cursor, '\n');
        break;
    }
    case VirtualFileKind::ProcSchedDebug: {
        auto& scheduler = hk::sched::scheduler();
        auto& manager = hk::userspace::userspace_manager();
        const auto diagnostics = manager.diagnostics();
        append_text(out, capacity, cursor, "Mattas scheduler debug\n");
        append_text(out, capacity, cursor, "threads ");
        append_decimal(out, capacity, cursor, scheduler.thread_count());
        append_text(out, capacity, cursor, "\nready ");
        append_decimal(out, capacity, cursor, scheduler.ready_count());
        append_text(out, capacity, cursor, "\nsleeping ");
        append_decimal(out, capacity, cursor, scheduler.sleeping_count());
        append_text(out, capacity, cursor, "\ndead ");
        append_decimal(out, capacity, cursor, scheduler.dead_count());
        append_text(out, capacity, cursor, "\nswitches ");
        append_decimal(out, capacity, cursor, scheduler.switch_count());
        append_text(out, capacity, cursor, "\nyields ");
        append_decimal(out, capacity, cursor, scheduler.yield_count());
        append_text(out, capacity, cursor, "\npreempts ");
        append_decimal(out, capacity, cursor, scheduler.preempt_count());
        append_text(out, capacity, cursor, "\ncurrent_thread ");
        append_decimal(out, capacity, cursor, scheduler.current_thread() ? scheduler.current_thread()->id : 0);
        append_text(out, capacity, cursor, "\ncurrent_cpu ");
        append_decimal(out, capacity, cursor, hk::cpu::runtime().current_cpu_id());
        append_text(out, capacity, cursor, "\nonline_cpus ");
        append_decimal(out, capacity, cursor, hk::cpu::topology().online_count());
        append_text(out, capacity, cursor, "\nuser_processes ");
        append_decimal(out, capacity, cursor, manager.process_count());
        append_text(out, capacity, cursor, "\nuser_live_processes ");
        append_decimal(out, capacity, cursor, manager.live_process_count());
        append_text(out, capacity, cursor, "\nuser_runnable_threads ");
        append_decimal(out, capacity, cursor, manager.runnable_thread_count());
        append_text(out, capacity, cursor, "\nuser_pipe_read_blocks ");
        append_decimal(out, capacity, cursor, diagnostics.pipe_read_blocks);
        append_text(out, capacity, cursor, "\nuser_pipe_write_blocks ");
        append_decimal(out, capacity, cursor, diagnostics.pipe_write_blocks);
        append_text(out, capacity, cursor, "\nuser_wait_blocks ");
        append_decimal(out, capacity, cursor, diagnostics.process_wait_blocks);
        append_text(out, capacity, cursor, "\nuser_wait_any_blocks ");
        append_decimal(out, capacity, cursor, diagnostics.process_wait_any_blocks);
        append_text(out, capacity, cursor, "\nuser_sleep_blocks ");
        append_decimal(out, capacity, cursor, diagnostics.sleep_blocks);
        append_text(out, capacity, cursor, "\nuser_preempt_switches ");
        append_decimal(out, capacity, cursor, diagnostics.preempt_switches);
        append_char(out, capacity, cursor, '\n');
        break;
    }
    case VirtualFileKind::ProcStat: {
        auto& scheduler = hk::sched::scheduler();
        auto& manager = hk::userspace::userspace_manager();
        const auto diagnostics = manager.diagnostics();
        const uint64_t ticks = hk::timer::ticks();
        append_text(out, capacity, cursor, "cpu ");
        append_decimal(out, capacity, cursor, ticks);
        append_text(out, capacity, cursor, " 0 0 ");
        append_decimal(out, capacity, cursor, ticks);
        append_text(out, capacity, cursor, "\nctxt ");
        append_decimal(out, capacity, cursor, scheduler.switch_count());
        append_text(out, capacity, cursor, "\nyields ");
        append_decimal(out, capacity, cursor, scheduler.yield_count());
        append_text(out, capacity, cursor, "\npreempt ");
        append_decimal(out, capacity, cursor, scheduler.preempt_count());
        append_text(out, capacity, cursor, "\nprocesses ");
        append_decimal(out, capacity, cursor, manager.process_count());
        append_text(out, capacity, cursor, "\nprocs_running ");
        append_decimal(out, capacity, cursor, manager.runnable_thread_count());
        append_text(out, capacity, cursor, "\nprocs_live ");
        append_decimal(out, capacity, cursor, manager.live_process_count());
        append_text(out, capacity, cursor, "\nuser_pipe_read_blocks ");
        append_decimal(out, capacity, cursor, diagnostics.pipe_read_blocks);
        append_text(out, capacity, cursor, "\nuser_pipe_write_blocks ");
        append_decimal(out, capacity, cursor, diagnostics.pipe_write_blocks);
        append_text(out, capacity, cursor, "\nuser_pipe_read_wakes ");
        append_decimal(out, capacity, cursor, diagnostics.pipe_read_wakes);
        append_text(out, capacity, cursor, "\nuser_pipe_write_wakes ");
        append_decimal(out, capacity, cursor, diagnostics.pipe_write_wakes);
        append_text(out, capacity, cursor, "\nuser_wait_blocks ");
        append_decimal(out, capacity, cursor, diagnostics.process_wait_blocks);
        append_text(out, capacity, cursor, "\nuser_wait_any_blocks ");
        append_decimal(out, capacity, cursor, diagnostics.process_wait_any_blocks);
        append_text(out, capacity, cursor, "\nuser_wait_wakes ");
        append_decimal(out, capacity, cursor, diagnostics.process_wait_wakes);
        append_text(out, capacity, cursor, "\nuser_sleep_blocks ");
        append_decimal(out, capacity, cursor, diagnostics.sleep_blocks);
        append_text(out, capacity, cursor, "\nuser_sleep_wakes ");
        append_decimal(out, capacity, cursor, diagnostics.sleep_wakes);
        append_text(out, capacity, cursor, "\nuser_preemption_gate_enables ");
        append_decimal(out, capacity, cursor, diagnostics.preemption_gate_enables);
        append_text(out, capacity, cursor, "\nuser_preemption_gate_disables ");
        append_decimal(out, capacity, cursor, diagnostics.preemption_gate_disables);
        append_text(out, capacity, cursor, "\nuser_preempt_switches ");
        append_decimal(out, capacity, cursor, diagnostics.preempt_switches);
        append_char(out, capacity, cursor, '\n');
        break;
    }
    case VirtualFileKind::ProcProcesses: {
        auto& manager = hk::userspace::userspace_manager();
        append_text(out, capacity, cursor, "PID PPID PGID STATE FDS TICKS NAME\n");
        for (uint64_t i = 0; i < manager.process_count(); ++i) {
            hybrid::ProcessInfo process{};
            if (manager.copy_process_info(i, process)) append_proc_process_line(out, capacity, cursor, process);
        }
        break;
    }
    case VirtualFileKind::ProcSelfStatus: {
        auto& manager = hk::userspace::userspace_manager();
        uint64_t pid = manager.current_pid();
        append_text(out, capacity, cursor, "Name:\t");
        hybrid::ProcessInfo selected{};
        bool found = false;
        for (uint64_t i = 0; i < manager.process_count(); ++i) {
            hybrid::ProcessInfo process{};
            if (!manager.copy_process_info(i, process) || process.pid != pid) continue;
            selected = process;
            found = true;
            break;
        }
        append_text(out, capacity, cursor, found ? selected.name : "kernel");
        append_text(out, capacity, cursor, "\nPid:\t");
        append_decimal(out, capacity, cursor, pid);
        append_text(out, capacity, cursor, "\nPPid:\t");
        append_decimal(out, capacity, cursor, found ? selected.parent_pid : 0);
        append_text(out, capacity, cursor, "\nPGid:\t");
        append_decimal(out, capacity, cursor, found ? selected.process_group_id : 0);
        append_text(out, capacity, cursor, "\nState:\t");
        append_text(out, capacity, cursor, found ? proc_process_state_name(selected.state) : "kernel");
        append_text(out, capacity, cursor, "\nFDs:\t");
        append_decimal(out, capacity, cursor, found ? selected.open_file_count : 0);
        append_text(out, capacity, cursor, "\nThreads:\t");
        append_decimal(out, capacity, cursor, manager.user_thread_count());
        append_text(out, capacity, cursor, "\nVmPages:\t");
        append_decimal(out, capacity, cursor, found ? selected.owned_page_count : 0);
        append_text(out, capacity, cursor, "\nRunTicks:\t");
        append_decimal(out, capacity, cursor, found ? selected.run_ticks : 0);
        append_char(out, capacity, cursor, '\n');
        break;
    }
    case VirtualFileKind::ProcSelfFd: {
        auto& manager = hk::userspace::userspace_manager();
        uint64_t pid = manager.current_pid();
        append_text(out, capacity, cursor, "FD KIND TARGET\n");
        if (pid != 0) {
            for (uint64_t i = 0; i < hk::userspace::kMaxProcessFileDescriptors; ++i) {
                hybrid::FileDescriptorInfo fd{};
                if (manager.copy_file_descriptor_info(pid, i, fd)) append_proc_fd_line(out, capacity, cursor, fd);
            }
        }
        break;
    }
    case VirtualFileKind::ProcMounts: {
        auto& fs = vfs();
        for (uint32_t i = 0; i < fs.mount_count(); ++i) {
            hybrid::MountInfo mount{};
            if (fs.copy_mount_info(i, mount)) append_mount_line(out, capacity, cursor, mount);
        }
        break;
    }
    case VirtualFileKind::ProcFilesystems: {
        append_text(out, capacity, cursor, "nodev\tvfs\n");
        append_text(out, capacity, cursor, "nodev\tproc\n");
        append_text(out, capacity, cursor, "nodev\tdevfs\n");
        append_text(out, capacity, cursor, "fat16\n");
        break;
    }
    case VirtualFileKind::ProcVfsStats: {
        const auto vfs_stats = vfs().stats();
        append_text(out, capacity, cursor, "ram_file_creates ");
        append_decimal(out, capacity, cursor, vfs_stats.ram_file_creates);
        append_text(out, capacity, cursor, "\nram_file_create_rejects ");
        append_decimal(out, capacity, cursor, vfs_stats.ram_file_create_rejects);
        append_text(out, capacity, cursor, "\nram_directory_creates ");
        append_decimal(out, capacity, cursor, vfs_stats.ram_directory_creates);
        append_text(out, capacity, cursor, "\nram_directory_create_rejects ");
        append_decimal(out, capacity, cursor, vfs_stats.ram_directory_create_rejects);
        append_text(out, capacity, cursor, "\nram_links ");
        append_decimal(out, capacity, cursor, vfs_stats.ram_links);
        append_text(out, capacity, cursor, "\nram_link_rejects ");
        append_decimal(out, capacity, cursor, vfs_stats.ram_link_rejects);
        append_text(out, capacity, cursor, "\nram_truncates ");
        append_decimal(out, capacity, cursor, vfs_stats.ram_truncates);
        append_text(out, capacity, cursor, "\nram_truncate_rejects ");
        append_decimal(out, capacity, cursor, vfs_stats.ram_truncate_rejects);
        append_text(out, capacity, cursor, "\nram_renames ");
        append_decimal(out, capacity, cursor, vfs_stats.ram_renames);
        append_text(out, capacity, cursor, "\nram_rename_rejects ");
        append_decimal(out, capacity, cursor, vfs_stats.ram_rename_rejects);
        append_text(out, capacity, cursor, "\nram_file_deletes ");
        append_decimal(out, capacity, cursor, vfs_stats.ram_file_deletes);
        append_text(out, capacity, cursor, "\nram_file_delete_rejects ");
        append_decimal(out, capacity, cursor, vfs_stats.ram_file_delete_rejects);
        append_text(out, capacity, cursor, "\nram_directory_deletes ");
        append_decimal(out, capacity, cursor, vfs_stats.ram_directory_deletes);
        append_text(out, capacity, cursor, "\nram_directory_delete_rejects ");
        append_decimal(out, capacity, cursor, vfs_stats.ram_directory_delete_rejects);
        append_text(out, capacity, cursor, "\nram_write_bytes ");
        append_decimal(out, capacity, cursor, vfs_stats.ram_write_bytes);
        append_text(out, capacity, cursor, "\nram_write_clipped_bytes ");
        append_decimal(out, capacity, cursor, vfs_stats.ram_write_clipped_bytes);
        append_char(out, capacity, cursor, '\n');
        break;
    }
    case VirtualFileKind::ProcBlockBootdisk: {
        const auto block_stats = hk::block::boot_disk().stats();
        append_text(out, capacity, cursor, "initialized ");
        append_decimal(out, capacity, cursor, block_stats.initialized ? 1 : 0);
        append_text(out, capacity, cursor, "\nsector_size 512");
        append_text(out, capacity, cursor, "\nsector_reads ");
        append_decimal(out, capacity, cursor, block_stats.sector_reads);
        append_text(out, capacity, cursor, "\nmulti_sector_reads ");
        append_decimal(out, capacity, cursor, block_stats.multi_sector_reads);
        append_text(out, capacity, cursor, "\ncache_hits ");
        append_decimal(out, capacity, cursor, block_stats.cache_hits);
        append_text(out, capacity, cursor, "\ncache_misses ");
        append_decimal(out, capacity, cursor, block_stats.cache_misses);
        append_text(out, capacity, cursor, "\ncache_evictions ");
        append_decimal(out, capacity, cursor, block_stats.cache_evictions);
        append_text(out, capacity, cursor, "\ninvalid_reads ");
        append_decimal(out, capacity, cursor, block_stats.invalid_reads);
        append_text(out, capacity, cursor, "\nnull_buffer_rejects ");
        append_decimal(out, capacity, cursor, block_stats.null_buffer_rejects);
        append_text(out, capacity, cursor, "\nzero_count_rejects ");
        append_decimal(out, capacity, cursor, block_stats.zero_count_rejects);
        append_text(out, capacity, cursor, "\noversized_request_rejects ");
        append_decimal(out, capacity, cursor, block_stats.oversized_request_rejects);
        append_text(out, capacity, cursor, "\nbackend_read_failures ");
        append_decimal(out, capacity, cursor, block_stats.backend_read_failures);
        append_text(out, capacity, cursor, "\ncache_fills ");
        append_decimal(out, capacity, cursor, block_stats.cache_fills);
        append_text(out, capacity, cursor, "\ncached_entries ");
        append_decimal(out, capacity, cursor, block_stats.cached_entries);
        append_text(out, capacity, cursor, "\nlargest_request_sectors ");
        append_decimal(out, capacity, cursor, block_stats.largest_request_sectors);
        append_text(out, capacity, cursor, "\nlast_lba ");
        append_decimal(out, capacity, cursor, block_stats.last_lba);
        append_char(out, capacity, cursor, '\n');
        break;
    }
    case VirtualFileKind::ProcPciSummary: {
        auto& pci = hk::pci::registry();
        append_text(out, capacity, cursor, "scanned_buses ");
        append_decimal(out, capacity, cursor, pci.scanned_buses());
        append_text(out, capacity, cursor, "\ndevices ");
        append_decimal(out, capacity, cursor, pci.count());
        append_text(out, capacity, cursor, "\nstorage_controllers ");
        append_decimal(out, capacity, cursor, pci.storage_controllers());
        append_text(out, capacity, cursor, "\nnetwork_controllers ");
        append_decimal(out, capacity, cursor, pci.network_controllers());
        append_text(out, capacity, cursor, "\ndisplay_controllers ");
        append_decimal(out, capacity, cursor, pci.display_controllers());
        append_text(out, capacity, cursor, "\nbridge_devices ");
        append_decimal(out, capacity, cursor, pci.bridge_devices());
        append_text(out, capacity, cursor, "\nmmio_bars ");
        append_decimal(out, capacity, cursor, pci.mmio_bar_count());
        append_text(out, capacity, cursor, "\nio_bars ");
        append_decimal(out, capacity, cursor, pci.io_bar_count());
        append_text(out, capacity, cursor, "\ndriver_candidates ");
        append_decimal(out, capacity, cursor, pci.driver_candidate_count());
        append_text(out, capacity, cursor, "\nahci_candidates ");
        append_decimal(out, capacity, cursor, pci.ahci_candidates());
        append_text(out, capacity, cursor, "\ne1000_candidates ");
        append_decimal(out, capacity, cursor, pci.e1000_candidates());
        append_text(out, capacity, cursor, "\nvga_candidates ");
        append_decimal(out, capacity, cursor, pci.vga_candidates());
        append_text(out, capacity, cursor, "\ndriver_bindings ");
        append_decimal(out, capacity, cursor, pci.driver_binding_count());
        append_text(out, capacity, cursor, "\nahci_bindings ");
        append_decimal(out, capacity, cursor, pci.ahci_bindings());
        append_text(out, capacity, cursor, "\ne1000_bindings ");
        append_decimal(out, capacity, cursor, pci.e1000_bindings());
        append_text(out, capacity, cursor, "\nvga_bindings ");
        append_decimal(out, capacity, cursor, pci.vga_bindings());
        append_text(out, capacity, cursor, "\necam_regions ");
        append_decimal(out, capacity, cursor, pci.ecam_region_count());
        append_text(out, capacity, cursor, "\necam_verified_devices ");
        append_decimal(out, capacity, cursor, pci.ecam_verified_devices());
        append_text(out, capacity, cursor, "\nconfig_probes ");
        append_decimal(out, capacity, cursor, pci.config_probe_count());
        append_text(out, capacity, cursor, "\nconfig_probe_failures ");
        append_decimal(out, capacity, cursor, pci.config_probe_failures());
        append_text(out, capacity, cursor, "\nconfig_ecam_mismatches ");
        append_decimal(out, capacity, cursor, pci.config_ecam_mismatches());
        append_text(out, capacity, cursor, "\nmalformed_config_rejects ");
        append_decimal(out, capacity, cursor, pci.malformed_config_rejects());
        append_text(out, capacity, cursor, "\npreferred_ecam_reads ");
        append_decimal(out, capacity, cursor, pci.preferred_ecam_reads());
        append_text(out, capacity, cursor, "\nlegacy_config_reads ");
        append_decimal(out, capacity, cursor, pci.legacy_config_reads());
        append_text(out, capacity, cursor, "\necam_fallback_reads ");
        append_decimal(out, capacity, cursor, pci.ecam_fallback_reads());
        append_text(out, capacity, cursor, "\npreferred_ecam_writes ");
        append_decimal(out, capacity, cursor, pci.preferred_ecam_writes());
        append_text(out, capacity, cursor, "\nlegacy_config_writes ");
        append_decimal(out, capacity, cursor, pci.legacy_config_writes());
        append_text(out, capacity, cursor, "\necam_write_fallbacks ");
        append_decimal(out, capacity, cursor, pci.ecam_write_fallbacks());
        append_text(out, capacity, cursor, "\ncommand_enable_attempts ");
        append_decimal(out, capacity, cursor, pci.command_enable_attempts());
        append_text(out, capacity, cursor, "\ncommand_enable_successes ");
        append_decimal(out, capacity, cursor, pci.command_enable_successes());
        append_char(out, capacity, cursor, '\n');
        break;
    }
    case VirtualFileKind::ProcPciDevices: {
        auto& pci = hk::pci::registry();
        append_text(out, capacity, cursor, "BDF VENDOR DEVICE CLASS SUBCLASS PROGIF COMMAND STATUS DRIVER BARS\n");
        const hk::pci::Device* devices = pci.devices();
        for (uint32_t i = 0; i < pci.count(); ++i) append_pci_device_line(out, capacity, cursor, devices[i]);
        break;
    }
    case VirtualFileKind::ProcDriverSummary: {
        auto& drivers = hk::drivers::driver_manager();
        const auto stats = drivers.stats();
        append_text(out, capacity, cursor, "registered_drivers ");
        append_decimal(out, capacity, cursor, stats.registered_drivers);
        append_text(out, capacity, cursor, "\nstarted_drivers ");
        append_decimal(out, capacity, cursor, drivers.started_count());
        append_text(out, capacity, cursor, "\nfailed_drivers ");
        append_decimal(out, capacity, cursor, drivers.failed_count());
        append_text(out, capacity, cursor, "\nregister_attempts ");
        append_decimal(out, capacity, cursor, stats.register_attempts);
        append_text(out, capacity, cursor, "\nfailed_registrations ");
        append_decimal(out, capacity, cursor, stats.failed_registrations);
        append_text(out, capacity, cursor, "\nstart_attempts ");
        append_decimal(out, capacity, cursor, stats.start_attempts);
        append_text(out, capacity, cursor, "\nstart_successes ");
        append_decimal(out, capacity, cursor, stats.start_successes);
        append_text(out, capacity, cursor, "\nstart_failures ");
        append_decimal(out, capacity, cursor, stats.start_failures);
        append_text(out, capacity, cursor, "\nimport_passes ");
        append_decimal(out, capacity, cursor, stats.import_passes);
        append_text(out, capacity, cursor, "\nimported_devices ");
        append_decimal(out, capacity, cursor, stats.imported_devices);
        append_text(out, capacity, cursor, "\nskipped_bindings ");
        append_decimal(out, capacity, cursor, stats.skipped_bindings);
        append_text(out, capacity, cursor, "\nahci_devices ");
        append_decimal(out, capacity, cursor, stats.ahci_devices);
        append_text(out, capacity, cursor, "\ne1000_devices ");
        append_decimal(out, capacity, cursor, stats.e1000_devices);
        append_text(out, capacity, cursor, "\nvga_devices ");
        append_decimal(out, capacity, cursor, stats.vga_devices);
        append_text(out, capacity, cursor, "\nbus_master_required_devices ");
        append_decimal(out, capacity, cursor, stats.bus_master_required_devices);
        append_text(out, capacity, cursor, "\ncommand_bits_union ");
        append_decimal(out, capacity, cursor, stats.command_bits_union);
        append_char(out, capacity, cursor, '\n');
        break;
    }
    case VirtualFileKind::ProcDriverDevices: {
        auto& drivers = hk::drivers::driver_manager();
        append_text(out, capacity, cursor, "DRIVER KIND BDF VENDOR DEVICE REQUIRED_COMMAND_BITS BUS_MASTER STATE\n");
        const hk::drivers::DriverDevice* devices = drivers.devices();
        for (uint64_t i = 0; i < drivers.device_count(); ++i) append_driver_device_line(out, capacity, cursor, devices[i]);
        break;
    }
    case VirtualFileKind::ProcIrqSummary: {
        const auto irq = hk::interrupts::stats();
        auto& lapic = hk::apic::local_apic();
        append_text(out, capacity, cursor, "pic_remaps ");
        append_decimal(out, capacity, cursor, irq.pic_remaps);
        append_text(out, capacity, cursor, "\nuser_preemption_enabled ");
        append_decimal(out, capacity, cursor, hk::timer::user_preemption_enabled() ? 1 : 0);
        append_text(out, capacity, cursor, "\nlapic_timer_active ");
        append_decimal(out, capacity, cursor, hk::timer::lapic_timer_active() ? 1 : 0);
        append_text(out, capacity, cursor, "\nlapic_ticks ");
        append_decimal(out, capacity, cursor, hk::timer::lapic_ticks());
        append_text(out, capacity, cursor, "\nmask_updates ");
        append_decimal(out, capacity, cursor, irq.mask_updates);
        append_text(out, capacity, cursor, "\nlegacy_eoi_count ");
        append_decimal(out, capacity, cursor, irq.legacy_eoi_count);
        append_text(out, capacity, cursor, "\ndispatch_count ");
        append_decimal(out, capacity, cursor, irq.dispatch_count);
        append_text(out, capacity, cursor, "\nexception_dispatch_count ");
        append_decimal(out, capacity, cursor, irq.exception_dispatch_count);
        append_text(out, capacity, cursor, "\nirq_dispatch_count ");
        append_decimal(out, capacity, cursor, irq.irq_dispatch_count);
        append_text(out, capacity, cursor, "\nlapic_dispatch_count ");
        append_decimal(out, capacity, cursor, irq.lapic_dispatch_count);
        append_text(out, capacity, cursor, "\nsyscall_dispatch_count ");
        append_decimal(out, capacity, cursor, irq.syscall_dispatch_count);
        append_text(out, capacity, cursor, "\nioapic_route_prepares ");
        append_decimal(out, capacity, cursor, irq.ioapic_route_prepares);
        append_text(out, capacity, cursor, "\nioapic_route_match_checks ");
        append_decimal(out, capacity, cursor, irq.ioapic_route_match_checks);
        append_text(out, capacity, cursor, "\nioapic_route_match_successes ");
        append_decimal(out, capacity, cursor, irq.ioapic_route_match_successes);
        append_text(out, capacity, cursor, "\ninvalid_vectors ");
        append_decimal(out, capacity, cursor, irq.invalid_vectors);
        append_text(out, capacity, cursor, "\nlast_vector ");
        append_decimal(out, capacity, cursor, irq.last_vector);
        append_text(out, capacity, cursor, "\nlast_irq ");
        append_decimal(out, capacity, cursor, irq.last_irq);
        append_text(out, capacity, cursor, "\nvector_0x20_count ");
        append_decimal(out, capacity, cursor, hk::interrupts::vector_dispatch_count(0x20));
        append_text(out, capacity, cursor, "\nvector_0x21_count ");
        append_decimal(out, capacity, cursor, hk::interrupts::vector_dispatch_count(0x21));
        append_text(out, capacity, cursor, "\nvector_0x40_count ");
        append_decimal(out, capacity, cursor, hk::interrupts::vector_dispatch_count(0x40));
        append_text(out, capacity, cursor, "\nvector_0x41_count ");
        append_decimal(out, capacity, cursor, hk::interrupts::vector_dispatch_count(0x41));
        append_text(out, capacity, cursor, "\nvector_0x80_count ");
        append_decimal(out, capacity, cursor, hk::interrupts::vector_dispatch_count(0x80));
        append_text(out, capacity, cursor, "\nlapic_available ");
        append_decimal(out, capacity, cursor, lapic.available() ? 1 : 0);
        append_text(out, capacity, cursor, "\nlapic_enabled ");
        append_decimal(out, capacity, cursor, lapic.enabled() ? 1 : 0);
        append_text(out, capacity, cursor, "\nlapic_id ");
        append_decimal(out, capacity, cursor, lapic.available() ? lapic.id() : 0);
        append_text(out, capacity, cursor, "\nlapic_version ");
        append_decimal(out, capacity, cursor, lapic.available() ? lapic.version() : 0);
        append_text(out, capacity, cursor, "\nlapic_timer_lvt ");
        append_decimal(out, capacity, cursor, lapic.available() ? lapic.timer_lvt() : 0);
        append_text(out, capacity, cursor, "\nlapic_timer_initial_count ");
        append_decimal(out, capacity, cursor, lapic.available() ? lapic.timer_initial_count() : 0);
        append_text(out, capacity, cursor, "\nlapic_timer_current_count ");
        append_decimal(out, capacity, cursor, lapic.available() ? lapic.timer_current_count() : 0);
        append_text(out, capacity, cursor, "\nlapic_timer_divide_config ");
        append_decimal(out, capacity, cursor, lapic.available() ? lapic.timer_divide_config() : 0);
        append_text(out, capacity, cursor, "\nlapic_timer_probe_delta ");
        append_decimal(out, capacity, cursor, lapic.timer_probe_delta());
        append_char(out, capacity, cursor, '\n');
        break;
    }
    case VirtualFileKind::ProcInterrupts: {
        const auto irq = hk::interrupts::stats();
        append_text(out, capacity, cursor, "VECTOR CPU0 CONTROLLER NAME\n");
        append_interrupt_line(out, capacity, cursor, 0x20, "IO-APIC", "legacy-timer");
        append_interrupt_line(out, capacity, cursor, 0x21, "IO-APIC", "keyboard");
        append_interrupt_line(out, capacity, cursor, 0x40, "LAPIC", "scheduler-timer");
        append_interrupt_line(out, capacity, cursor, 0x41, "LAPIC", "ipi-work");
        append_interrupt_line(out, capacity, cursor, 0x80, "SYSCALL", "int80");
        append_text(out, capacity, cursor, "ERR: ");
        append_decimal(out, capacity, cursor, irq.exception_dispatch_count);
        append_text(out, capacity, cursor, " exceptions\nMIS: ");
        append_decimal(out, capacity, cursor, irq.invalid_vectors);
        append_text(out, capacity, cursor, " invalid-vectors\nEOI: ");
        append_decimal(out, capacity, cursor, irq.legacy_eoi_count);
        append_text(out, capacity, cursor, " legacy-eoi\n");
        break;
    }
    case VirtualFileKind::ProcTtySummary: {
        const auto tty = hk::terminal::stats();
        const auto fb = hk::console().stats();
        append_text(out, capacity, cursor, "input_mode ");
        append_text(out, capacity, cursor, terminal_input_mode_name(hk::terminal::input_mode()));
        append_text(out, capacity, cursor, "\nbuffered_input ");
        append_decimal(out, capacity, cursor, hk::terminal::buffered_input_count());
        append_text(out, capacity, cursor, "\nconsole_viewport_bottom ");
        append_decimal(out, capacity, cursor, fb.viewport_bottom);
        append_text(out, capacity, cursor, "\nconsole_render_calls ");
        append_decimal(out, capacity, cursor, fb.render_calls);
        append_text(out, capacity, cursor, "\nconsole_scroll_relative_ops ");
        append_decimal(out, capacity, cursor, fb.scroll_relative_ops);
        append_text(out, capacity, cursor, "\nconsole_scroll_to_bottom_ops ");
        append_decimal(out, capacity, cursor, fb.scroll_to_bottom_ops);
        append_text(out, capacity, cursor, "\nconsole_input_line_resets ");
        append_decimal(out, capacity, cursor, fb.input_line_resets);
        append_text(out, capacity, cursor, "\nconsole_follow_tail ");
        append_decimal(out, capacity, cursor, fb.follow_tail ? 1 : 0);
        append_text(out, capacity, cursor, "\nconsole_glyph_draws ");
        append_decimal(out, capacity, cursor, fb.glyph_draws);
        append_text(out, capacity, cursor, "\nconsole_cells_written ");
        append_decimal(out, capacity, cursor, fb.cells_written);
        append_text(out, capacity, cursor, "\nwrite_calls ");
        append_decimal(out, capacity, cursor, tty.write_calls);
        append_text(out, capacity, cursor, "\nbytes_written ");
        append_decimal(out, capacity, cursor, tty.bytes_written);
        append_text(out, capacity, cursor, "\nraw_input_bytes ");
        append_decimal(out, capacity, cursor, tty.raw_input_bytes);
        append_text(out, capacity, cursor, "\ncanonical_input_bytes ");
        append_decimal(out, capacity, cursor, tty.canonical_input_bytes);
        append_text(out, capacity, cursor, "\ncanonical_line_commits ");
        append_decimal(out, capacity, cursor, tty.canonical_line_commits);
        append_text(out, capacity, cursor, "\ndropped_input_bytes ");
        append_decimal(out, capacity, cursor, tty.dropped_input_bytes);
        append_text(out, capacity, cursor, "\ninput_high_watermark ");
        append_decimal(out, capacity, cursor, tty.input_high_watermark);
        append_text(out, capacity, cursor, "\nscroll_relative_ops ");
        append_decimal(out, capacity, cursor, tty.scroll_relative_ops);
        append_text(out, capacity, cursor, "\nscroll_to_bottom_ops ");
        append_decimal(out, capacity, cursor, tty.scroll_to_bottom_ops);
        append_text(out, capacity, cursor, "\ninput_line_resets ");
        append_decimal(out, capacity, cursor, tty.input_line_resets);
        append_text(out, capacity, cursor, "\nconsole_visible_columns ");
        append_decimal(out, capacity, cursor, fb.visible_columns);
        append_text(out, capacity, cursor, "\nconsole_visible_rows ");
        append_decimal(out, capacity, cursor, fb.visible_rows);
        append_text(out, capacity, cursor, "\nconsole_cursor_row ");
        append_decimal(out, capacity, cursor, fb.cursor_row);
        append_text(out, capacity, cursor, "\nconsole_cursor_column ");
        append_decimal(out, capacity, cursor, fb.cursor_column);
        append_text(out, capacity, cursor, "\nconsole_oldest_row ");
        append_decimal(out, capacity, cursor, fb.oldest_row);
        append_char(out, capacity, cursor, '\n');
        break;
    }
    case VirtualFileKind::ProcCpuSummary: {
        auto& topo = hk::cpu::topology();
        append_text(out, capacity, cursor, "cpus ");
        append_decimal(out, capacity, cursor, topo.cpu_count());
        append_text(out, capacity, cursor, "\nonline ");
        append_decimal(out, capacity, cursor, topo.online_count());
        append_text(out, capacity, cursor, "\nstartup_attempts ");
        append_decimal(out, capacity, cursor, topo.startup_attempt_count());
        append_text(out, capacity, cursor, "\ncurrent_cpu ");
        append_decimal(out, capacity, cursor, hk::cpu::runtime().current_cpu_id());
        append_text(out, capacity, cursor, "\nsmp_init_ipi_attempts ");
        append_decimal(out, capacity, cursor, hk::smp::init_ipi_attempts());
        append_text(out, capacity, cursor, "\nsmp_init_ipi_delivered ");
        append_decimal(out, capacity, cursor, hk::smp::init_ipi_delivered());
        append_text(out, capacity, cursor, "\nsmp_startup_ipi_delivered ");
        append_decimal(out, capacity, cursor, hk::smp::startup_ipi_delivered());
        append_text(out, capacity, cursor, "\nsmp_ap_checkins ");
        append_decimal(out, capacity, cursor, hk::smp::ap_checkins());
        append_text(out, capacity, cursor, "\nsmp_ipi_work_completed ");
        append_decimal(out, capacity, cursor, hk::smp::ipi_work_completed());
        append_text(out, capacity, cursor, "\nsmp_queued_work_completed ");
        append_decimal(out, capacity, cursor, hk::smp::queued_work_completed());
        append_text(out, capacity, cursor, "\nsmp_tlb_shootdown_completed ");
        append_decimal(out, capacity, cursor, hk::smp::tlb_shootdown_completed());
        append_char(out, capacity, cursor, '\n');
        break;
    }
    case VirtualFileKind::ProcCpuTopology: {
        auto& topo = hk::cpu::topology();
        append_text(out, capacity, cursor, "CPU APIC ACPI ENABLED ONLINE BOOTSTRAP STARTUP STATE DESCRIPTORS LAPIC_TIMER BOOT_WORK IPI_WORK TICKS INTERRUPTS WORK TLB\n");
        for (uint32_t i = 0; i < topo.cpu_count(); ++i) {
            const auto* cpu = topo.cpu(i);
            if (cpu) append_cpu_topology_line(out, capacity, cursor, *cpu);
        }
        break;
    }
    case VirtualFileKind::ProcMmSummary: {
        const auto mem = hk::mm::pmm().stats();
        const auto pmm = hk::mm::pmm().diagnostics();
        const auto vmm = hk::mm::vmm().diagnostics();
        const auto heap = hk::mm::heap().stats();
        append_text(out, capacity, cursor, "pmm_total_pages ");
        append_decimal(out, capacity, cursor, mem.total_pages);
        append_text(out, capacity, cursor, "\npmm_free_pages ");
        append_decimal(out, capacity, cursor, mem.free_pages);
        append_text(out, capacity, cursor, "\npmm_used_pages ");
        append_decimal(out, capacity, cursor, mem.used_pages);
        append_text(out, capacity, cursor, "\npmm_usable_bytes ");
        append_decimal(out, capacity, cursor, mem.usable_bytes);
        append_text(out, capacity, cursor, "\npmm_reserved_bytes ");
        append_decimal(out, capacity, cursor, mem.reserved_bytes);
        append_text(out, capacity, cursor, "\npmm_highest_physical ");
        append_decimal(out, capacity, cursor, mem.highest_physical);
        append_text(out, capacity, cursor, "\npmm_allocate_page_calls ");
        append_decimal(out, capacity, cursor, pmm.allocate_page_calls);
        append_text(out, capacity, cursor, "\npmm_allocate_contiguous_calls ");
        append_decimal(out, capacity, cursor, pmm.allocate_contiguous_calls);
        append_text(out, capacity, cursor, "\npmm_free_page_calls ");
        append_decimal(out, capacity, cursor, pmm.free_page_calls);
        append_text(out, capacity, cursor, "\npmm_free_contiguous_calls ");
        append_decimal(out, capacity, cursor, pmm.free_contiguous_calls);
        append_text(out, capacity, cursor, "\npmm_failed_allocations ");
        append_decimal(out, capacity, cursor, pmm.failed_allocations);
        append_text(out, capacity, cursor, "\npmm_invalid_frees ");
        append_decimal(out, capacity, cursor, pmm.invalid_frees);
        append_text(out, capacity, cursor, "\npmm_peak_used_pages ");
        append_decimal(out, capacity, cursor, pmm.peak_used_pages);
        append_text(out, capacity, cursor, "\npmm_last_allocated_page ");
        append_decimal(out, capacity, cursor, pmm.last_allocated_page);
        append_text(out, capacity, cursor, "\nvmm_active_pml4 ");
        append_decimal(out, capacity, cursor, hk::mm::vmm().active_pml4());
        append_text(out, capacity, cursor, "\nvmm_map_page_calls ");
        append_decimal(out, capacity, cursor, vmm.map_page_calls);
        append_text(out, capacity, cursor, "\nvmm_map_range_calls ");
        append_decimal(out, capacity, cursor, vmm.map_range_calls);
        append_text(out, capacity, cursor, "\nvmm_unmap_page_calls ");
        append_decimal(out, capacity, cursor, vmm.unmap_page_calls);
        append_text(out, capacity, cursor, "\nvmm_mapped_pages ");
        append_decimal(out, capacity, cursor, vmm.mapped_pages);
        append_text(out, capacity, cursor, "\nvmm_unmapped_pages ");
        append_decimal(out, capacity, cursor, vmm.unmapped_pages);
        append_text(out, capacity, cursor, "\nvmm_failed_maps ");
        append_decimal(out, capacity, cursor, vmm.failed_maps);
        append_text(out, capacity, cursor, "\nvmm_failed_unmaps ");
        append_decimal(out, capacity, cursor, vmm.failed_unmaps);
        append_text(out, capacity, cursor, "\nvmm_duplicate_map_rejects ");
        append_decimal(out, capacity, cursor, vmm.duplicate_map_rejects);
        append_text(out, capacity, cursor, "\nvmm_unaligned_map_rejects ");
        append_decimal(out, capacity, cursor, vmm.unaligned_map_rejects);
        append_text(out, capacity, cursor, "\nvmm_absent_unmap_rejects ");
        append_decimal(out, capacity, cursor, vmm.absent_unmap_rejects);
        append_text(out, capacity, cursor, "\nvmm_remote_shootdowns_requested ");
        append_decimal(out, capacity, cursor, vmm.remote_shootdowns_requested);
        append_text(out, capacity, cursor, "\nvmm_last_mapped_virt ");
        append_decimal(out, capacity, cursor, vmm.last_mapped_virt);
        append_text(out, capacity, cursor, "\nvmm_last_unmapped_virt ");
        append_decimal(out, capacity, cursor, vmm.last_unmapped_virt);
        append_text(out, capacity, cursor, "\nheap_start ");
        append_decimal(out, capacity, cursor, heap.heap_start);
        append_text(out, capacity, cursor, "\nheap_end ");
        append_decimal(out, capacity, cursor, heap.heap_end);
        append_text(out, capacity, cursor, "\nheap_bytes ");
        append_decimal(out, capacity, cursor, heap.heap_bytes);
        append_text(out, capacity, cursor, "\nheap_block_count ");
        append_decimal(out, capacity, cursor, heap.block_count);
        append_text(out, capacity, cursor, "\nheap_used_blocks ");
        append_decimal(out, capacity, cursor, heap.used_blocks);
        append_text(out, capacity, cursor, "\nheap_free_blocks ");
        append_decimal(out, capacity, cursor, heap.free_blocks);
        append_text(out, capacity, cursor, "\nheap_used_bytes ");
        append_decimal(out, capacity, cursor, heap.used_bytes);
        append_text(out, capacity, cursor, "\nheap_free_bytes ");
        append_decimal(out, capacity, cursor, heap.free_bytes);
        append_text(out, capacity, cursor, "\nheap_largest_free_block ");
        append_decimal(out, capacity, cursor, heap.largest_free_block);
        append_char(out, capacity, cursor, '\n');
        break;
    }
    case VirtualFileKind::ProcBuddyinfo: {
        auto& pmm = hk::mm::pmm();
        const auto mem = pmm.stats();
        append_text(out, capacity, cursor, "Node 0, zone Normal");
        for (uint64_t order = 0; order <= 10; ++order) {
            append_char(out, capacity, cursor, ' ');
            append_decimal(out, capacity, cursor, pmm.free_run_count(order));
        }
        append_text(out, capacity, cursor, "\n# page_size ");
        append_decimal(out, capacity, cursor, hk::mm::kPageSize);
        append_text(out, capacity, cursor, "\n# total_pages ");
        append_decimal(out, capacity, cursor, mem.total_pages);
        append_text(out, capacity, cursor, "\n# free_pages ");
        append_decimal(out, capacity, cursor, mem.free_pages);
        append_text(out, capacity, cursor, "\n# bitmap_allocator 1\n");
        break;
    }
    case VirtualFileKind::ProcNetSummary: {
        const auto& adapter = hk::drivers::e1000::driver().adapter();
        append_text(out, capacity, cursor, "interfaces ");
        append_decimal(out, capacity, cursor, adapter.present ? 1 : 0);
        append_text(out, capacity, cursor, "\ne1000_present ");
        append_decimal(out, capacity, cursor, adapter.present ? 1 : 0);
        append_text(out, capacity, cursor, "\ne1000_link ");
        append_text(out, capacity, cursor, link_state_name(adapter.link_up));
        append_text(out, capacity, cursor, "\ne1000_speed_mbps ");
        append_decimal(out, capacity, cursor, adapter.link_speed_mbps);
        append_text(out, capacity, cursor, "\ne1000_full_duplex ");
        append_decimal(out, capacity, cursor, adapter.full_duplex ? 1 : 0);
        append_text(out, capacity, cursor, "\ne1000_mac_valid ");
        append_decimal(out, capacity, cursor, adapter.mac_valid ? 1 : 0);
        append_text(out, capacity, cursor, "\ne1000_mac ");
        append_decimal(out, capacity, cursor, adapter.mac_address);
        append_text(out, capacity, cursor, "\ne1000_mmio_mapped ");
        append_decimal(out, capacity, cursor, adapter.mmio_mapped ? 1 : 0);
        append_text(out, capacity, cursor, "\ne1000_command_enabled ");
        append_decimal(out, capacity, cursor, adapter.command_enabled ? 1 : 0);
        append_text(out, capacity, cursor, "\ne1000_rings_allocated ");
        append_decimal(out, capacity, cursor, adapter.rings_allocated ? 1 : 0);
        append_text(out, capacity, cursor, "\ne1000_rings_programmed ");
        append_decimal(out, capacity, cursor, adapter.rings_programmed ? 1 : 0);
        append_text(out, capacity, cursor, "\ne1000_ring_registers_verified ");
        append_decimal(out, capacity, cursor, adapter.ring_registers_verified ? 1 : 0);
        append_text(out, capacity, cursor, "\ne1000_tx_packets ");
        append_decimal(out, capacity, cursor, adapter.tx_packets_submitted);
        append_text(out, capacity, cursor, "\ne1000_tx_completed ");
        append_decimal(out, capacity, cursor, adapter.tx_packets_completed);
        append_text(out, capacity, cursor, "\ne1000_rx_packets ");
        append_decimal(out, capacity, cursor, adapter.rx_packets_received);
        append_text(out, capacity, cursor, "\ne1000_rx_bytes ");
        append_decimal(out, capacity, cursor, adapter.rx_bytes_received);
        append_text(out, capacity, cursor, "\ne1000_tx_busy_failures ");
        append_decimal(out, capacity, cursor, adapter.tx_busy_failures);
        append_text(out, capacity, cursor, "\ne1000_tx_length_rejects ");
        append_decimal(out, capacity, cursor, adapter.tx_length_rejects);
        append_text(out, capacity, cursor, "\ne1000_rx_empty_polls ");
        append_decimal(out, capacity, cursor, adapter.rx_empty_polls);
        append_text(out, capacity, cursor, "\ne1000_rx_small_buffer_drops ");
        append_decimal(out, capacity, cursor, adapter.rx_small_buffer_drops);
        append_char(out, capacity, cursor, '\n');
        break;
    }
    case VirtualFileKind::ProcNetDev: {
        const auto& adapter = hk::drivers::e1000::driver().adapter();
        append_text(out, capacity, cursor, "IFACE LINK SPEED_MBPS MAC_VALID RX_PACKETS RX_BYTES TX_PACKETS TX_COMPLETED RX_DROPS TX_ERRORS\n");
        if (adapter.present) {
            append_text(out, capacity, cursor, "eth0 ");
            append_text(out, capacity, cursor, link_state_name(adapter.link_up));
            append_text(out, capacity, cursor, " ");
            append_decimal(out, capacity, cursor, adapter.link_speed_mbps);
            append_text(out, capacity, cursor, " ");
            append_decimal(out, capacity, cursor, adapter.mac_valid ? 1 : 0);
            append_text(out, capacity, cursor, " ");
            append_decimal(out, capacity, cursor, adapter.rx_packets_received);
            append_text(out, capacity, cursor, " ");
            append_decimal(out, capacity, cursor, adapter.rx_bytes_received);
            append_text(out, capacity, cursor, " ");
            append_decimal(out, capacity, cursor, adapter.tx_packets_submitted);
            append_text(out, capacity, cursor, " ");
            append_decimal(out, capacity, cursor, adapter.tx_packets_completed);
            append_text(out, capacity, cursor, " ");
            append_decimal(out, capacity, cursor, adapter.rx_small_buffer_drops);
            append_text(out, capacity, cursor, " ");
            append_decimal(out, capacity, cursor, adapter.tx_busy_failures + adapter.tx_length_rejects + adapter.tx_null_rejects);
            append_char(out, capacity, cursor, '\n');
        }
        break;
    }
    case VirtualFileKind::ProcKmsg: {
        uint64_t copied = hk::copy_kernel_log(out, capacity, 0);
        cursor = copied;
        if (cursor < capacity) out[cursor] = 0;
        break;
    }
    case VirtualFileKind::ProcModules: {
        append_text(out, capacity, cursor, "NAME SIZE ADDRESS PATH\n");
        if (boot_module_table) {
            for (uint64_t i = 0; i < boot_module_count; ++i) {
                const auto& module = boot_module_table[i];
                if (module.path[0] != 0 && module.base != 0 && module.size != 0) append_module_line(out, capacity, cursor, module);
            }
        }
        break;
    }
    case VirtualFileKind::ProcCmdline:
        append_text(out, capacity, cursor, "BOOT_IMAGE=/boot/kernel.elf root=/dev/ram0 rw console=tty0 boot=uefi user=/user/init.elf");
        append_char(out, capacity, cursor, '\n');
        break;
    case VirtualFileKind::ProcHostname:
        append_text(out, capacity, cursor, kEtcHostname);
        break;
    case VirtualFileKind::ProcOstype:
        append_text(out, capacity, cursor, kProcKernelOstype);
        break;
    case VirtualFileKind::ProcOsrelease:
        append_text(out, capacity, cursor, kProcKernelOsrelease);
        break;
    case VirtualFileKind::ProcVersionString:
        append_text(out, capacity, cursor, kProcVersion);
        break;
    default:
        break;
    }
    return cursor;
}

uint64_t virtual_file_size(VirtualFileKind kind) {
    char scratch[kVirtualFileScratchBytes];
    return render_virtual_file(kind, scratch, sizeof(scratch));
}

bool read_fat_entry(hk::block::BlockDevice& disk, const Fat16Geometry& geometry, uint16_t cluster, uint16_t& next) {
    unsigned char sector[512]{};
    uint32_t offset = static_cast<uint32_t>(cluster) * 2u;
    uint32_t sector_index = geometry.reserved_sectors + offset / 512u;
    if (!disk.read_sector(sector_index, sector)) return false;
    next = read_le16(sector, offset % 512u);
    return true;
}

bool load_fat_file(hk::block::BlockDevice& disk, const Fat16Geometry& geometry, uint16_t first_cluster, uint32_t file_size, uint64_t& base) {
    if (first_cluster < 2 || file_size == 0) return false;
    uint64_t pages = hk::mm::align_up(file_size) / hk::mm::kPageSize;
    base = hk::mm::pmm().allocate_contiguous(pages);
    if (base == 0) return false;
    memset(reinterpret_cast<void*>(base), 0, pages * hk::mm::kPageSize);

    uint32_t copied = 0;
    uint16_t cluster = first_cluster;
    uint32_t guard = 0;
    while (cluster >= 2 && cluster < 0xfff8 && copied < file_size && guard++ < 4096) {
        uint32_t first_sector = geometry.data_sector + (static_cast<uint32_t>(cluster) - 2u) * geometry.sectors_per_cluster;
        for (uint32_t s = 0; s < geometry.sectors_per_cluster && copied < file_size; ++s) {
            unsigned char sector[512]{};
            if (!disk.read_sector(first_sector + s, sector)) return false;
            uint32_t remaining = file_size - copied;
            uint32_t to_copy = remaining < 512u ? remaining : 512u;
            memcpy(reinterpret_cast<void*>(base + copied), sector, to_copy);
            copied += to_copy;
        }
        uint16_t next = 0xffff;
        if (!read_fat_entry(disk, geometry, cluster, next)) return false;
        cluster = next;
    }
    return copied == file_size;
}

void mount_fat_entry(Vfs& fs, hk::block::BlockDevice& disk, const Fat16Geometry& geometry, const char* parent, const unsigned char* entry, uint32_t depth, uint32_t& registered, uint64_t& mounted_bytes);

void mount_fat_directory_cluster(Vfs& fs, hk::block::BlockDevice& disk, const Fat16Geometry& geometry, const char* parent, uint16_t first_cluster, uint32_t depth, uint32_t& registered, uint64_t& mounted_bytes) {
    if (depth > 4 || first_cluster < 2) return;
    uint16_t cluster = first_cluster;
    uint32_t guard = 0;
    while (cluster >= 2 && cluster < 0xfff8 && guard++ < 4096) {
        uint32_t first_sector = geometry.data_sector + (static_cast<uint32_t>(cluster) - 2u) * geometry.sectors_per_cluster;
        for (uint32_t s = 0; s < geometry.sectors_per_cluster; ++s) {
            unsigned char sector[512]{};
            if (!disk.read_sector(first_sector + s, sector)) return;
            for (uint32_t entry_index = 0; entry_index < 16; ++entry_index) {
                const unsigned char* entry = sector + entry_index * 32u;
                if (entry[0] == 0) return;
                mount_fat_entry(fs, disk, geometry, parent, entry, depth, registered, mounted_bytes);
            }
        }
        uint16_t next = 0xffff;
        if (!read_fat_entry(disk, geometry, cluster, next)) return;
        cluster = next;
    }
}

void mount_fat_entry(Vfs& fs, hk::block::BlockDevice& disk, const Fat16Geometry& geometry, const char* parent, const unsigned char* entry, uint32_t depth, uint32_t& registered, uint64_t& mounted_bytes) {
    uint8_t attr = entry[11];
    if (entry[0] == 0xe5 || entry[0] == '.' || (attr & 0x0f) == 0x0f || (attr & 0x08) != 0) return;
    char name[16]{};
    if (!fat_short_name(entry, name)) return;
    char* path = reserve_mounted_path(parent, name);
    if (!path) return;
    uint16_t cluster = read_le16(entry, 26);
    uint32_t size = read_le32(entry, 28);
    if ((attr & 0x10) != 0) {
        if (fs.register_directory(path)) ++registered;
        mount_fat_directory_cluster(fs, disk, geometry, path, cluster, depth + 1, registered, mounted_bytes);
        return;
    }
    uint64_t base = 0;
    if (load_fat_file(disk, geometry, cluster, size, base) && fs.register_disk_file(path, base, size)) {
        ++registered;
        mounted_bytes += size;
    }
}

void mount_fat16_root(Vfs& fs, hk::block::BlockDevice& disk) {
    unsigned char boot_sector[512]{};
    if (!disk.read_sector(0, boot_sector)) return;
    Fat16Geometry geometry{};
    if (!parse_fat16_geometry(boot_sector, geometry)) return;
    fs.register_directory("/mnt");
    fs.register_directory("/mnt/boot");
    hk::log(hk::LogLevel::Info, "FAT16 disk mount /mnt/boot");
    hk::log_hex(hk::LogLevel::Info, "FAT16 root sector", geometry.root_sector);
    hk::log_hex(hk::LogLevel::Info, "FAT16 data sector", geometry.data_sector);

    uint32_t registered = 0;
    uint64_t mounted_bytes = 0;
    for (uint32_t sector_index = 0; sector_index < geometry.root_dir_sectors; ++sector_index) {
        unsigned char sector[512]{};
        if (!disk.read_sector(geometry.root_sector + sector_index, sector)) return;
        for (uint32_t entry_index = 0; entry_index < 16; ++entry_index) {
            const unsigned char* entry = sector + entry_index * 32u;
            if (entry[0] == 0) {
                hk::log_hex(hk::LogLevel::Info, "FAT16 mounted entries", registered);
                fs.register_mount("/mnt/boot", "fat16", "boot-disk0", hybrid::MountReadOnly | hybrid::MountDiskBacked, registered + 1u, mounted_bytes);
                return;
            }
            mount_fat_entry(fs, disk, geometry, "/mnt/boot", entry, 0, registered, mounted_bytes);
        }
    }
    hk::log_hex(hk::LogLevel::Info, "FAT16 mounted entries", registered);
    fs.register_mount("/mnt/boot", "fat16", "boot-disk0", hybrid::MountReadOnly | hybrid::MountDiskBacked, registered + 1u, mounted_bytes);
}

bool path_equal(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

bool normalize_path(char (&out)[64], const char* path) {
    if (!path || path[0] != '/') return false;
    for (uint64_t i = 0; i < sizeof(out); ++i) out[i] = 0;
    out[0] = '/';
    uint64_t out_len = 1;
    uint64_t cursor = 1;
    while (path[cursor] != 0) {
        while (path[cursor] == '/') ++cursor;
        if (path[cursor] == 0) break;
        uint64_t start = cursor;
        while (path[cursor] != 0 && path[cursor] != '/') ++cursor;
        uint64_t length = cursor - start;
        if (length == 1 && path[start] == '.') continue;
        if (length == 2 && path[start] == '.' && path[start + 1] == '.') {
            if (out_len > 1) {
                while (out_len > 1 && out[out_len - 1] != '/') --out_len;
                if (out_len > 1 && out[out_len - 1] == '/') --out_len;
            }
            out[out_len] = 0;
            continue;
        }
        if (out_len != 1) {
            if (out_len + 1 >= sizeof(out)) return false;
            out[out_len++] = '/';
        }
        if (out_len + length >= sizeof(out)) return false;
        for (uint64_t i = 0; i < length; ++i) out[out_len++] = path[start + i];
        out[out_len] = 0;
    }
    for (uint64_t i = out_len + 1; i < sizeof(out); ++i) out[i] = 0;
    return true;
}

bool path_has_prefix_child(const char* path, const char* directory) {
    if (!path || !directory || path_equal(path, directory)) return false;
    uint64_t i = 0;
    while (directory[i] != 0) {
        if (path[i] != directory[i]) return false;
        ++i;
    }
    return directory[i - 1] == '/' || path[i] == '/';
}

bool path_is_direct_child(const char* path, const char* directory) {
    if (!path_has_prefix_child(path, directory)) return false;
    uint64_t start = 0;
    while (directory[start] != 0) ++start;
    if (start > 1) {
        if (path[start] != '/') return false;
        ++start;
    }
    if (path[start] == 0) return false;
    for (uint64_t i = start; path[i] != 0; ++i) {
        if (path[i] == '/') return false;
    }
    return true;
}

const char* basename_of(const char* path) {
    if (!path) return "";
    const char* name = path;
    for (uint64_t i = 0; path[i] != 0; ++i) {
        if (path[i] == '/' && path[i + 1] != 0) name = path + i + 1;
    }
    return name;
}

const char* module_name_from_path(const char* path) {
    const char* name = basename_of(path);
    return name && name[0] != 0 ? name : "module";
}

void append_module_line(char* out, uint64_t capacity, uint64_t& cursor, const hybrid::BootModule& module) {
    append_text(out, capacity, cursor, module_name_from_path(module.path));
    append_text(out, capacity, cursor, " ");
    append_decimal(out, capacity, cursor, module.size);
    append_text(out, capacity, cursor, " ");
    append_decimal(out, capacity, cursor, module.base);
    append_text(out, capacity, cursor, " ");
    append_text(out, capacity, cursor, module.path);
    append_char(out, capacity, cursor, '\n');
}

void copy_path(char (&out)[64], const char* path) {
    uint64_t i = 0;
    if (path) {
        for (; i + 1 < sizeof(out) && path[i] != 0; ++i) out[i] = path[i];
    }
    out[i] = 0;
    for (++i; i < sizeof(out); ++i) out[i] = 0;
}

template <uint64_t N>
void copy_text(char (&out)[N], const char* text) {
    uint64_t i = 0;
    if (text) {
        for (; i + 1 < N && text[i] != 0; ++i) out[i] = text[i];
    }
    out[i] = 0;
    for (++i; i < N; ++i) out[i] = 0;
}

bool copy_path_checked(char (&out)[64], const char* path) {
    if (!normalize_path(out, path)) return false;
    return out[1] != 0;
}

bool parent_path(const char* path, char (&out)[64]) {
    char normalized[64]{};
    if (!normalize_path(normalized, path) || normalized[1] == 0) return false;
    uint64_t length = 0;
    while (length + 1 < sizeof(out) && normalized[length] != 0) ++length;
    if (normalized[length] != 0) return false;
    uint64_t slash = length;
    while (slash > 0 && normalized[slash] != '/') --slash;
    if (slash == 0) {
        out[0] = '/';
        out[1] = 0;
        for (uint64_t i = 2; i < sizeof(out); ++i) out[i] = 0;
        return true;
    }
    for (uint64_t i = 0; i < slash; ++i) out[i] = normalized[i];
    out[slash] = 0;
    for (uint64_t i = slash + 1; i < sizeof(out); ++i) out[i] = 0;
    return true;
}

hybrid::VfsNodeType abi_type(NodeType type) {
    switch (type) {
    case NodeType::Directory:
        return hybrid::VfsNodeType::Directory;
    case NodeType::MemoryFile:
        return hybrid::VfsNodeType::MemoryFile;
    case NodeType::CharacterDevice:
        return hybrid::VfsNodeType::CharacterDevice;
    case NodeType::VirtualFile:
        return hybrid::VfsNodeType::VirtualFile;
    default:
        return hybrid::VfsNodeType::Empty;
    }
}

uint32_t abi_flags(const Node& node) {
    uint32_t flags = 0;
    if (node.type == NodeType::MemoryFile) flags |= hybrid::VfsNodeReadable | hybrid::VfsNodeMemoryBacked;
    if (node.type == NodeType::Directory) flags |= hybrid::VfsNodeDirectory;
    if (node.type == NodeType::CharacterDevice) {
        flags |= hybrid::VfsNodeCharacterDevice;
        if (node.device_kind == DeviceKind::Zero || node.device_kind == DeviceKind::Tty ||
            node.device_kind == DeviceKind::Console) {
            flags |= hybrid::VfsNodeReadable;
        }
        if (node.device_kind == DeviceKind::Null || node.device_kind == DeviceKind::Zero ||
            node.device_kind == DeviceKind::Tty || node.device_kind == DeviceKind::Console) {
            flags |= hybrid::VfsNodeWritable;
        }
    }
    if (node.type == NodeType::VirtualFile) flags |= hybrid::VfsNodeReadable | hybrid::VfsNodeVirtual;
    if (node.writable) flags |= hybrid::VfsNodeWritable;
    if (node.disk_backed) {
        flags |= hybrid::VfsNodeReadable | hybrid::VfsNodeDiskBacked;
        flags &= ~hybrid::VfsNodeMemoryBacked;
    }
    return flags;
}

uint64_t link_count_for(const Node* nodes, uint32_t count, const Node& node) {
    if (!node.ram_file) return 1;
    uint64_t links = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (nodes[i].type == NodeType::MemoryFile && nodes[i].ram_file == node.ram_file) ++links;
    }
    return links == 0 ? 1 : links;
}

uint64_t node_size_for(const Node& node) {
    if (node.type == NodeType::VirtualFile) return virtual_file_size(node.virtual_kind);
    return node.ram_file ? node.ram_file->size : node.size;
}
}

Vfs& vfs() {
    static Vfs instance;
    return instance;
}

void Vfs::initialize(const hybrid::BootInfo& boot) {
    count_ = 0;
    next_handle_id_ = 1;
    stats_ = {};
    for (auto& handle : handles_) handle = FileHandle{};
    for (auto& file : ram_files_) file = RamFile{};
    for (auto& directory : ram_directories_) directory = RamDirectory{};
    for (auto& link : ram_links_) link = RamLink{};
    for (auto& mount : mounts_) mount = MountRecord{};
    mounted_fat_path_count = 0;
    for (auto& path : mounted_fat_paths) path[0] = 0;
    boot_module_table = reinterpret_cast<const hybrid::BootModule*>(boot.boot_modules);
    boot_module_count = boot.boot_module_count <= hybrid::kMaxBootModules ? boot.boot_module_count : 0;
    register_directory("/");
    register_directory("/boot");
    register_directory("/user");
    register_directory("/bin");
    register_directory("/etc");
    register_directory("/proc");
    register_directory("/proc/block");
    register_directory("/proc/driver");
    register_directory("/proc/pci");
    register_directory("/proc/irq");
    register_directory("/proc/tty");
    register_directory("/proc/cpu");
    register_directory("/proc/mm");
    register_directory("/proc/net");
    register_directory("/proc/self");
    register_directory("/proc/fs");
    register_directory("/proc/sys");
    register_directory("/proc/sys/kernel");
    register_directory("/dev");
    register_directory("/tmp", true);
    register_directory("/disk");
    register_character_device("/dev/null", DeviceKind::Null);
    register_character_device("/dev/zero", DeviceKind::Zero);
    register_character_device("/dev/tty", DeviceKind::Tty);
    register_character_device("/dev/console", DeviceKind::Console);
    register_mount("/", "vfs", "boot-modules", hybrid::MountWritable | hybrid::MountMemoryBacked, count_, 0);
    register_memory_file("/etc/os-release", reinterpret_cast<uint64_t>(kEtcOsRelease), sizeof(kEtcOsRelease) - 1);
    register_memory_file("/etc/hostname", reinterpret_cast<uint64_t>(kEtcHostname), sizeof(kEtcHostname) - 1);
    register_memory_file("/proc/version", reinterpret_cast<uint64_t>(kProcVersion), sizeof(kProcVersion) - 1);
    register_memory_file("/proc/cpuinfo", reinterpret_cast<uint64_t>(kProcCpuInfo), sizeof(kProcCpuInfo) - 1);
    register_virtual_file("/proc/meminfo", VirtualFileKind::ProcMeminfo);
    register_virtual_file("/proc/uptime", VirtualFileKind::ProcUptime);
    register_virtual_file("/proc/loadavg", VirtualFileKind::ProcLoadavg);
    register_virtual_file("/proc/sched_debug", VirtualFileKind::ProcSchedDebug);
    register_virtual_file("/proc/stat", VirtualFileKind::ProcStat);
    register_virtual_file("/proc/processes", VirtualFileKind::ProcProcesses);
    register_virtual_file("/proc/modules", VirtualFileKind::ProcModules);
    register_virtual_file("/proc/mounts", VirtualFileKind::ProcMounts);
    register_virtual_file("/proc/filesystems", VirtualFileKind::ProcFilesystems);
    register_virtual_file("/proc/fs/vfs", VirtualFileKind::ProcVfsStats);
    register_virtual_file("/proc/block/bootdisk", VirtualFileKind::ProcBlockBootdisk);
    register_virtual_file("/proc/driver/summary", VirtualFileKind::ProcDriverSummary);
    register_virtual_file("/proc/driver/devices", VirtualFileKind::ProcDriverDevices);
    register_virtual_file("/proc/pci/summary", VirtualFileKind::ProcPciSummary);
    register_virtual_file("/proc/pci/devices", VirtualFileKind::ProcPciDevices);
    register_virtual_file("/proc/irq/summary", VirtualFileKind::ProcIrqSummary);
    register_virtual_file("/proc/interrupts", VirtualFileKind::ProcInterrupts);
    register_virtual_file("/proc/tty/summary", VirtualFileKind::ProcTtySummary);
    register_virtual_file("/proc/cpu/summary", VirtualFileKind::ProcCpuSummary);
    register_virtual_file("/proc/cpu/topology", VirtualFileKind::ProcCpuTopology);
    register_virtual_file("/proc/mm/summary", VirtualFileKind::ProcMmSummary);
    register_virtual_file("/proc/buddyinfo", VirtualFileKind::ProcBuddyinfo);
    register_virtual_file("/proc/kmsg", VirtualFileKind::ProcKmsg);
    register_virtual_file("/proc/net/summary", VirtualFileKind::ProcNetSummary);
    register_virtual_file("/proc/net/dev", VirtualFileKind::ProcNetDev);
    register_virtual_file("/proc/cmdline", VirtualFileKind::ProcCmdline);
    register_virtual_file("/proc/sys/kernel/hostname", VirtualFileKind::ProcHostname);
    register_virtual_file("/proc/sys/kernel/ostype", VirtualFileKind::ProcOstype);
    register_virtual_file("/proc/sys/kernel/osrelease", VirtualFileKind::ProcOsrelease);
    register_virtual_file("/proc/sys/kernel/version", VirtualFileKind::ProcVersionString);
    register_virtual_file("/proc/self/status", VirtualFileKind::ProcSelfStatus);
    register_virtual_file("/proc/self/fd", VirtualFileKind::ProcSelfFd);
    if (boot.kernel_physical_base != 0 && boot.kernel_physical_end > boot.kernel_physical_base) {
        register_memory_file("/boot/kernel.elf", boot.kernel_physical_base, boot.kernel_physical_end - boot.kernel_physical_base);
    }
    for (uint64_t i = 0; i < boot_module_count; ++i) {
        if (boot_module_table[i].path[0] == '/' && boot_module_table[i].base != 0 && boot_module_table[i].size != 0) {
            register_memory_file(boot_module_table[i].path, boot_module_table[i].base, boot_module_table[i].size);
        }
    }
    const auto& ahci = hk::drivers::ahci::driver().controller();
    if (ahci.read_lba0_success && ahci.read_lba0_buffer != 0) {
        register_memory_file("/disk/bootsector.bin", ahci.read_lba0_buffer, 512);
        auto& disk = hk::block::boot_disk();
        if (disk.initialize_ahci()) mount_fat16_root(*this, disk);
    }
    register_mount("/", "vfs", "boot-modules", hybrid::MountWritable | hybrid::MountMemoryBacked, count_, total_memory_file_bytes());
}

bool Vfs::add_node(const Node& node) {
    if (!node.path || node.type == NodeType::Empty || count_ >= kMaxVfsNodes || find(node.path)) return false;
    nodes_[count_++] = node;
    return true;
}

bool Vfs::register_directory(const char* path, bool writable) {
    return add_node(Node{path, NodeType::Directory, 0, 0, writable, false, nullptr, DeviceKind::None, VirtualFileKind::None});
}

bool Vfs::register_memory_file(const char* path, uint64_t base, uint64_t size) {
    if (base == 0 || size == 0) return false;
    return add_node(Node{path, NodeType::MemoryFile, base, size, false, false, nullptr, DeviceKind::None, VirtualFileKind::None});
}

bool Vfs::register_disk_file(const char* path, uint64_t base, uint64_t size) {
    if (base == 0 || size == 0) return false;
    return add_node(Node{path, NodeType::MemoryFile, base, size, false, true, nullptr, DeviceKind::None, VirtualFileKind::None});
}

bool Vfs::register_character_device(const char* path, DeviceKind kind) {
    if (!path || kind == DeviceKind::None) return false;
    return add_node(Node{path, NodeType::CharacterDevice, 0, 0, true, false, nullptr, kind, VirtualFileKind::None});
}

bool Vfs::register_virtual_file(const char* path, VirtualFileKind kind) {
    if (!path || kind == VirtualFileKind::None) return false;
    return add_node(Node{path, NodeType::VirtualFile, 0, 0, false, false, nullptr, DeviceKind::None, kind});
}

bool Vfs::register_mount(const char* path, const char* fs_type, const char* source, uint32_t flags, uint64_t node_count, uint64_t total_bytes) {
    char normalized[64]{};
    if (!normalize_path(normalized, path)) return false;
    for (auto& mount : mounts_) {
        if (mount.used && path_equal(mount.path, normalized)) {
            mount.flags = flags;
            mount.node_count = node_count;
            mount.total_bytes = total_bytes;
            copy_text(mount.fs_type, fs_type);
            copy_text(mount.source, source);
            return true;
        }
    }
    for (auto& mount : mounts_) {
        if (mount.used) continue;
        mount.used = true;
        mount.flags = flags;
        mount.node_count = node_count;
        mount.total_bytes = total_bytes;
        copy_path(mount.path, normalized);
        copy_text(mount.fs_type, fs_type);
        copy_text(mount.source, source);
        return true;
    }
    return false;
}

bool Vfs::create_ram_file(const char* path) {
    char normalized[64]{};
    if (!copy_path_checked(normalized, path) || find(normalized)) {
        ++stats_.ram_file_create_rejects;
        return false;
    }
    char parent[64]{};
    if (!parent_path(normalized, parent)) {
        ++stats_.ram_file_create_rejects;
        return false;
    }
    const Node* parent_node = find(parent);
    if (!parent_node || parent_node->type != NodeType::Directory) {
        ++stats_.ram_file_create_rejects;
        return false;
    }
    for (auto& file : ram_files_) {
        if (file.used) continue;
        if (!copy_path_checked(file.path, normalized)) {
            ++stats_.ram_file_create_rejects;
            return false;
        }
        file.size = 0;
        for (uint64_t i = 0; i < sizeof(file.data); ++i) file.data[i] = 0;
        file.used = true;
        if (add_node(Node{file.path, NodeType::MemoryFile, reinterpret_cast<uint64_t>(file.data), 0, true, false, &file, DeviceKind::None, VirtualFileKind::None})) {
            ++stats_.ram_file_creates;
            return true;
        }
        file = RamFile{};
        ++stats_.ram_file_create_rejects;
        return false;
    }
    ++stats_.ram_file_create_rejects;
    return false;
}

bool Vfs::link_ram_file(const char* existing_path, const char* new_path) {
    char existing[64]{};
    char linked[64]{};
    if (!normalize_path(existing, existing_path) || !copy_path_checked(linked, new_path) || find(linked)) {
        ++stats_.ram_link_rejects;
        return false;
    }
    const Node* source = find(existing);
    if (!source || source->type != NodeType::MemoryFile || !source->writable || !source->ram_file || source->disk_backed) {
        ++stats_.ram_link_rejects;
        return false;
    }
    char parent[64]{};
    if (!parent_path(linked, parent)) {
        ++stats_.ram_link_rejects;
        return false;
    }
    const Node* parent_node = find(parent);
    if (!parent_node || parent_node->type != NodeType::Directory) {
        ++stats_.ram_link_rejects;
        return false;
    }
    for (auto& link : ram_links_) {
        if (link.used) continue;
        copy_path(link.path, linked);
        link.target = source->ram_file;
        link.used = true;
        if (add_node(Node{link.path, NodeType::MemoryFile, reinterpret_cast<uint64_t>(link.target->data), link.target->size, true, false, link.target, DeviceKind::None, VirtualFileKind::None})) {
            ++stats_.ram_links;
            return true;
        }
        link = RamLink{};
        ++stats_.ram_link_rejects;
        return false;
    }
    ++stats_.ram_link_rejects;
    return false;
}

bool Vfs::truncate_ram_file(const char* path, uint64_t size) {
    char normalized[64]{};
    if (!normalize_path(normalized, path) || size > kMaxRamFileBytes) {
        ++stats_.ram_truncate_rejects;
        return false;
    }
    const Node* found = find(normalized);
    if (!found || found->type != NodeType::MemoryFile || !found->writable || !found->ram_file || found->disk_backed) {
        ++stats_.ram_truncate_rejects;
        return false;
    }
    RamFile* file = found->ram_file;
    if (size > file->size) {
        for (uint64_t i = file->size; i < size; ++i) file->data[i] = 0;
    }
    file->size = size;
    for (uint32_t i = 0; i < count_; ++i) {
        if (nodes_[i].ram_file != file) continue;
        nodes_[i].base = reinterpret_cast<uint64_t>(file->data);
        nodes_[i].size = file->size;
    }
    for (auto& handle : handles_) {
        if (handle.open && handle.node && handle.node->ram_file == file && handle.offset > size) handle.offset = size;
    }
    ++stats_.ram_truncates;
    return true;
}

bool Vfs::rename_ram_node(const char* old_path, const char* new_path) {
    char old_normalized[64]{};
    char new_normalized[64]{};
    if (!normalize_path(old_normalized, old_path) || !copy_path_checked(new_normalized, new_path)) {
        ++stats_.ram_rename_rejects;
        return false;
    }
    if (path_equal(old_normalized, new_normalized) || find(new_normalized)) {
        ++stats_.ram_rename_rejects;
        return false;
    }
    char parent[64]{};
    if (!parent_path(new_normalized, parent)) {
        ++stats_.ram_rename_rejects;
        return false;
    }
    const Node* parent_node = find(parent);
    if (!parent_node || parent_node->type != NodeType::Directory) {
        ++stats_.ram_rename_rejects;
        return false;
    }

    for (uint32_t i = 0; i < count_; ++i) {
        Node& node = nodes_[i];
        if (!path_equal(node.path, old_normalized) || !node.writable || node.disk_backed) continue;
        if (node.type == NodeType::MemoryFile && node.ram_file) {
            for (auto& link : ram_links_) {
                if (link.used && path_equal(link.path, old_normalized)) {
                    copy_path(link.path, new_normalized);
                    node.path = link.path;
                    ++stats_.ram_renames;
                    return true;
                }
            }
            if (!node.ram_file->used || !path_equal(node.ram_file->path, old_normalized)) {
                ++stats_.ram_rename_rejects;
                return false;
            }
            copy_path(node.ram_file->path, new_normalized);
            node.path = node.ram_file->path;
            ++stats_.ram_renames;
            return true;
        }
        if (node.type == NodeType::Directory && node.ram_file == nullptr) {
            for (uint32_t child = 0; child < count_; ++child) {
                if (child != i && path_has_prefix_child(nodes_[child].path, old_normalized)) {
                    ++stats_.ram_rename_rejects;
                    return false;
                }
            }
            for (auto& directory : ram_directories_) {
                if (!directory.used || !path_equal(directory.path, old_normalized)) continue;
                copy_path(directory.path, new_normalized);
                node.path = directory.path;
                ++stats_.ram_renames;
                return true;
            }
        }
    }
    ++stats_.ram_rename_rejects;
    return false;
}

bool Vfs::delete_ram_file(const char* path) {
    char normalized[64]{};
    if (!normalize_path(normalized, path)) {
        ++stats_.ram_file_delete_rejects;
        return false;
    }
    for (uint32_t i = 0; i < count_; ++i) {
        Node& node = nodes_[i];
        if (!path_equal(node.path, normalized) || !node.writable || !node.ram_file) continue;
        RamFile* target = node.ram_file;
        for (auto& handle : handles_) if (handle.open && handle.node && handle.node->ram_file == target) {
            ++stats_.ram_file_delete_rejects;
            return false;
        }
        for (auto& link : ram_links_) {
            if (link.used && path_equal(link.path, normalized)) {
                link = RamLink{};
                break;
            }
        }
        for (uint32_t j = i; j + 1 < count_; ++j) nodes_[j] = nodes_[j + 1];
        nodes_[count_ - 1] = Node{};
        --count_;
        bool still_linked = false;
        for (uint32_t j = 0; j < count_; ++j) {
            if (nodes_[j].type == NodeType::MemoryFile && nodes_[j].ram_file == target) {
                still_linked = true;
                break;
            }
        }
        if (!still_linked) *target = RamFile{};
        ++stats_.ram_file_deletes;
        return true;
    }
    ++stats_.ram_file_delete_rejects;
    return false;
}

bool Vfs::create_ram_directory(const char* path) {
    char normalized[64]{};
    if (!copy_path_checked(normalized, path) || find(normalized)) {
        ++stats_.ram_directory_create_rejects;
        return false;
    }
    char parent[64]{};
    if (!parent_path(normalized, parent)) {
        ++stats_.ram_directory_create_rejects;
        return false;
    }
    const Node* parent_node = find(parent);
    if (!parent_node || parent_node->type != NodeType::Directory) {
        ++stats_.ram_directory_create_rejects;
        return false;
    }
    for (auto& directory : ram_directories_) {
        if (directory.used) continue;
        if (!copy_path_checked(directory.path, normalized)) {
            ++stats_.ram_directory_create_rejects;
            return false;
        }
        directory.used = true;
        if (add_node(Node{directory.path, NodeType::Directory, 0, 0, true, false, nullptr, DeviceKind::None, VirtualFileKind::None})) {
            ++stats_.ram_directory_creates;
            return true;
        }
        directory = RamDirectory{};
        ++stats_.ram_directory_create_rejects;
        return false;
    }
    ++stats_.ram_directory_create_rejects;
    return false;
}

bool Vfs::delete_ram_directory(const char* path) {
    char normalized[64]{};
    if (!normalize_path(normalized, path) || normalized[1] == 0) {
        ++stats_.ram_directory_delete_rejects;
        return false;
    }
    for (uint32_t i = 0; i < count_; ++i) {
        Node& node = nodes_[i];
        if (!path_equal(node.path, normalized) || node.type != NodeType::Directory || !node.writable) continue;
        for (uint32_t child = 0; child < count_; ++child) {
            if (path_has_prefix_child(nodes_[child].path, normalized)) {
                ++stats_.ram_directory_delete_rejects;
                return false;
            }
        }
        for (auto& directory : ram_directories_) {
            if (directory.used && path_equal(directory.path, normalized)) {
                directory = RamDirectory{};
                break;
            }
        }
        for (uint32_t j = i; j + 1 < count_; ++j) nodes_[j] = nodes_[j + 1];
        nodes_[count_ - 1] = Node{};
        --count_;
        ++stats_.ram_directory_deletes;
        return true;
    }
    ++stats_.ram_directory_delete_rejects;
    return false;
}

const Node* Vfs::find(const char* path) const {
    char normalized[64]{};
    if (!normalize_path(normalized, path)) return nullptr;
    for (uint32_t i = 0; i < count_; ++i) {
        if (path_equal(nodes_[i].path, normalized)) return &nodes_[i];
    }
    return nullptr;
}

size_t Vfs::read(const char* path, uint64_t offset, void* buffer, size_t size) const {
    if (!buffer || size == 0) return 0;
    char normalized[64]{};
    if (normalize_path(normalized, path)) {
        char generated[kVirtualFileScratchBytes];
        uint64_t generated_size = render_dynamic_proc_file(normalized, generated, sizeof(generated));
        if (generated_size != 0) {
            if (offset >= generated_size) return 0;
            uint64_t available = generated_size - offset;
            size_t to_copy = size < available ? size : static_cast<size_t>(available);
            memcpy(buffer, generated + offset, to_copy);
            return to_copy;
        }
    }
    const Node* node = find(path);
    if (!node || (node->type != NodeType::MemoryFile && node->type != NodeType::VirtualFile)) return 0;
    if (node->type == NodeType::VirtualFile) {
        char generated[kVirtualFileScratchBytes];
        uint64_t generated_size = render_virtual_file(node->virtual_kind, generated, sizeof(generated));
        if (offset >= generated_size) return 0;
        uint64_t available = generated_size - offset;
        size_t to_copy = size < available ? size : static_cast<size_t>(available);
        memcpy(buffer, generated + offset, to_copy);
        return to_copy;
    }
    uint64_t node_size = node_size_for(*node);
    if (offset >= node_size) return 0;
    uint64_t available = node_size - offset;
    size_t to_copy = size < available ? size : static_cast<size_t>(available);
    const void* source = node->ram_file ? static_cast<const void*>(node->ram_file->data + offset) : reinterpret_cast<const void*>(node->base + offset);
    memcpy(buffer, source, to_copy);
    return to_copy;
}

FileHandle* Vfs::handle_for(uint32_t handle) {
    if (handle == 0) return nullptr;
    for (auto& entry : handles_) {
        if (entry.open && entry.id == handle) return &entry;
    }
    return nullptr;
}

const FileHandle* Vfs::handle_for(uint32_t handle) const {
    if (handle == 0) return nullptr;
    for (const auto& entry : handles_) {
        if (entry.open && entry.id == handle) return &entry;
    }
    return nullptr;
}

uint32_t Vfs::open(const char* path) {
    char normalized[64]{};
    bool dynamic_virtual = false;
    const Node* node = find(path);
    if (!node) {
        if (!normalize_path(normalized, path) || dynamic_proc_file_size(normalized) == 0) return 0;
        dynamic_virtual = true;
    } else if (node->type != NodeType::MemoryFile && node->type != NodeType::CharacterDevice && node->type != NodeType::VirtualFile) {
        return 0;
    }
    for (auto& handle : handles_) {
        if (!handle.open) {
            uint32_t id = next_handle_id_++;
            if (id == 0) id = next_handle_id_++;
            handle = FileHandle{id, node, 0, 1, true, dynamic_virtual, {}};
            if (dynamic_virtual) copy_path(handle.path, normalized);
            else if (node && node->path) copy_path(handle.path, node->path);
            return id;
        }
    }
    return 0;
}

size_t Vfs::read_handle(uint32_t handle, void* buffer, size_t size) {
    FileHandle* entry = handle_for(handle);
    if (!entry || !buffer || size == 0) return 0;
    if (entry->dynamic_virtual) {
        char generated[kVirtualFileScratchBytes];
        uint64_t generated_size = render_dynamic_proc_file(entry->path, generated, sizeof(generated));
        if (entry->offset >= generated_size) return 0;
        uint64_t available = generated_size - entry->offset;
        size_t to_copy = size < available ? size : static_cast<size_t>(available);
        memcpy(buffer, generated + entry->offset, to_copy);
        entry->offset += to_copy;
        return to_copy;
    }
    if (!entry->node) return 0;
    if (entry->node->type == NodeType::CharacterDevice) {
        if (entry->node->device_kind == DeviceKind::Zero) {
            memset(buffer, 0, size);
            entry->offset += size;
            return size;
        }
        if (entry->node->device_kind == DeviceKind::Tty || entry->node->device_kind == DeviceKind::Console) {
            size_t bytes = hk::terminal::read_input(static_cast<char*>(buffer), size);
            entry->offset += bytes;
            return bytes;
        }
        return 0;
    }
    if (entry->node->type == NodeType::VirtualFile) {
        char generated[kVirtualFileScratchBytes];
        uint64_t generated_size = render_virtual_file(entry->node->virtual_kind, generated, sizeof(generated));
        if (entry->offset >= generated_size) return 0;
        uint64_t available = generated_size - entry->offset;
        size_t to_copy = size < available ? size : static_cast<size_t>(available);
        memcpy(buffer, generated + entry->offset, to_copy);
        entry->offset += to_copy;
        return to_copy;
    }
    uint64_t node_size = node_size_for(*entry->node);
    if (entry->offset >= node_size) return 0;
    uint64_t available = node_size - entry->offset;
    size_t to_copy = size < available ? size : static_cast<size_t>(available);
    const void* source = entry->node->ram_file
        ? static_cast<const void*>(entry->node->ram_file->data + entry->offset)
        : reinterpret_cast<const void*>(entry->node->base + entry->offset);
    memcpy(buffer, source, to_copy);
    entry->offset += to_copy;
    return to_copy;
}

size_t Vfs::write_handle(uint32_t handle, const void* buffer, size_t size) {
    FileHandle* entry = handle_for(handle);
    if (!entry || !entry->node || !buffer || size == 0) return 0;
    if (entry->node->type == NodeType::CharacterDevice) {
        if (entry->node->device_kind == DeviceKind::Null || entry->node->device_kind == DeviceKind::Zero) {
            entry->offset += size;
            return size;
        }
        if (entry->node->device_kind == DeviceKind::Tty || entry->node->device_kind == DeviceKind::Console) {
            size_t written = hk::terminal::write(static_cast<const char*>(buffer), size);
            entry->offset += written;
            return written;
        }
        return 0;
    }
    if (!entry->node->writable || !entry->node->ram_file) return 0;
    RamFile* file = entry->node->ram_file;
    if (entry->offset >= kMaxRamFileBytes) return 0;
    uint64_t available = kMaxRamFileBytes - entry->offset;
    size_t to_copy = size < available ? size : static_cast<size_t>(available);
    if (to_copy < size) stats_.ram_write_clipped_bytes += size - to_copy;
    memcpy(file->data + entry->offset, buffer, to_copy);
    entry->offset += to_copy;
    stats_.ram_write_bytes += to_copy;
    if (entry->offset > file->size) file->size = entry->offset;
    Node* node = const_cast<Node*>(entry->node);
    node->base = reinterpret_cast<uint64_t>(file->data);
    node->size = file->size;
    return to_copy;
}

bool Vfs::seek_handle(uint32_t handle, uint64_t offset) {
    FileHandle* entry = handle_for(handle);
    if (entry && entry->dynamic_virtual) {
        uint64_t node_size = dynamic_proc_file_size(entry->path);
        if (offset > node_size) return false;
        entry->offset = offset;
        return true;
    }
    uint64_t node_size = entry && entry->node ? node_size_for(*entry->node) : 0;
    if (!entry || !entry->node || offset > node_size) return false;
    entry->offset = offset;
    return true;
}

bool Vfs::retain_handle(uint32_t handle) {
    FileHandle* entry = handle_for(handle);
    if (!entry || entry->ref_count == 0xffffffffu) return false;
    ++entry->ref_count;
    return true;
}

uint64_t Vfs::handle_offset(uint32_t handle) const {
    const FileHandle* entry = handle_for(handle);
    return entry ? entry->offset : 0;
}

bool Vfs::close(uint32_t handle) {
    FileHandle* entry = handle_for(handle);
    if (!entry) return false;
    if (entry->ref_count > 1) {
        --entry->ref_count;
        return true;
    }
    *entry = FileHandle{};
    return true;
}

uint32_t Vfs::open_handle_count() const {
    uint32_t total = 0;
    for (const auto& handle : handles_) if (handle.open) ++total;
    return total;
}

uint32_t Vfs::mount_count() const {
    uint32_t total = 0;
    for (const auto& mount : mounts_) if (mount.used) ++total;
    return total;
}

uint32_t Vfs::memory_file_count() const {
    uint32_t total = 0;
    for (uint32_t i = 0; i < count_; ++i) if (nodes_[i].type == NodeType::MemoryFile) ++total;
    return total;
}

uint64_t Vfs::total_memory_file_bytes() const {
    uint64_t total = 0;
    for (uint32_t i = 0; i < count_; ++i) {
        if (nodes_[i].type != NodeType::MemoryFile) continue;
        total += nodes_[i].ram_file ? nodes_[i].ram_file->size : nodes_[i].size;
    }
    return total;
}

bool Vfs::copy_node_info(uint32_t index, hybrid::VfsNodeInfo& out) const {
    if (index >= count_) return false;
    const Node& node = nodes_[index];
    if (node.type == NodeType::Empty || node.path == nullptr) return false;
    out.type = abi_type(node.type);
    out.flags = abi_flags(node);
    out.base = node.base;
    out.size = node_size_for(node);
    out.links = link_count_for(nodes_, count_, node);
    copy_path(out.path, node.path);
    return true;
}

bool Vfs::copy_directory_entry(const char* path, uint32_t index, hybrid::VfsDirectoryEntryInfo& out) const {
    char normalized[64]{};
    if (!normalize_path(normalized, path)) return false;
    const Node* directory = find(normalized);
    uint64_t proc_pid = 0;
    DynamicProcKind proc_kind = parse_dynamic_proc_path(normalized, proc_pid);
    if ((!directory || directory->type != NodeType::Directory) &&
        proc_kind != DynamicProcKind::PidDirectory &&
        proc_kind != DynamicProcKind::PidFd) {
        return false;
    }
    uint32_t seen = 0;
    if (directory && directory->type == NodeType::Directory) {
        for (uint32_t i = 0; i < count_; ++i) {
            const Node& node = nodes_[i];
            if (node.type == NodeType::Empty || node.path == nullptr || !path_is_direct_child(node.path, normalized)) continue;
            if (seen++ != index) continue;
            out.type = abi_type(node.type);
            out.flags = abi_flags(node);
            out.size = node_size_for(node);
            out.links = link_count_for(nodes_, count_, node);
            copy_text(out.name, basename_of(node.path));
            copy_path(out.path, node.path);
            return true;
        }
    }
    if (path_equal(normalized, "/proc")) {
        auto& manager = hk::userspace::userspace_manager();
        for (uint64_t i = 0; i < manager.process_count(); ++i) {
            hybrid::ProcessInfo process{};
            if (!manager.copy_process_info(i, process)) continue;
            if (seen++ != index) continue;
            char entry_path[64]{};
            copy_proc_pid_path(entry_path, process.pid, nullptr);
            out.type = hybrid::VfsNodeType::Directory;
            out.flags = hybrid::VfsNodeDirectory;
            out.size = 0;
            out.links = 1;
            copy_text(out.name, basename_of(entry_path));
            copy_path(out.path, entry_path);
            return true;
        }
    }
    if (proc_kind == DynamicProcKind::PidDirectory) {
        const char* names[] = {"status", "fd"};
        if (index >= 2) return false;
        char entry_path[64]{};
        copy_proc_pid_path(entry_path, proc_pid, names[index]);
        out.type = index == 1 ? hybrid::VfsNodeType::Directory : hybrid::VfsNodeType::VirtualFile;
        out.flags = index == 1 ? hybrid::VfsNodeDirectory : (hybrid::VfsNodeReadable | hybrid::VfsNodeVirtual);
        out.size = dynamic_proc_file_size(entry_path);
        out.links = 1;
        copy_text(out.name, names[index]);
        copy_path(out.path, entry_path);
        return true;
    }
    if (proc_kind == DynamicProcKind::PidFd) {
        auto& manager = hk::userspace::userspace_manager();
        uint32_t seen_fd = 0;
        for (uint64_t i = 0; i < hk::userspace::kMaxProcessFileDescriptors; ++i) {
            hybrid::FileDescriptorInfo fd{};
            if (!manager.copy_file_descriptor_info(proc_pid, i, fd)) continue;
            if (seen_fd++ != index) continue;
            char entry_path[64]{};
            uint64_t cursor = 0;
            append_text(entry_path, sizeof(entry_path), cursor, "/proc/");
            append_decimal(entry_path, sizeof(entry_path), cursor, proc_pid);
            append_text(entry_path, sizeof(entry_path), cursor, "/fd/");
            append_decimal(entry_path, sizeof(entry_path), cursor, fd.fd);
            char target[64]{};
            copy_fd_target_text(target, fd);
            out.type = hybrid::VfsNodeType::VirtualFile;
            out.flags = hybrid::VfsNodeReadable | hybrid::VfsNodeVirtual;
            out.size = dynamic_proc_file_size(entry_path);
            if (out.size == 0) {
                uint64_t target_len = 0;
                while (target[target_len] != 0) ++target_len;
                out.size = target_len;
            }
            out.links = 1;
            copy_text(out.name, basename_of(entry_path));
            copy_path(out.path, entry_path);
            return true;
        }
    }
    return false;
}

bool Vfs::copy_mount_info(uint32_t index, hybrid::MountInfo& out) const {
    uint32_t seen = 0;
    for (const auto& mount : mounts_) {
        if (!mount.used) continue;
        if (seen++ != index) continue;
        out.flags = mount.flags;
        out.reserved = 0;
        out.node_count = mount.node_count;
        out.total_bytes = mount.total_bytes;
        copy_path(out.path, mount.path);
        copy_text(out.fs_type, mount.fs_type);
        copy_text(out.source, mount.source);
        return true;
    }
    return false;
}

bool Vfs::stat(const char* path, hybrid::VfsStatInfo& out) const {
    char normalized[64]{};
    if (normalize_path(normalized, path)) {
        uint64_t proc_pid = 0;
        DynamicProcKind proc_kind = parse_dynamic_proc_path(normalized, proc_pid);
        if (proc_kind == DynamicProcKind::PidDirectory) {
            out.type = hybrid::VfsNodeType::Directory;
            out.flags = hybrid::VfsNodeDirectory;
            out.size = 0;
            out.links = 1;
            copy_path(out.path, normalized);
            return true;
        }
        if (proc_kind == DynamicProcKind::PidFd) {
            out.type = hybrid::VfsNodeType::Directory;
            out.flags = hybrid::VfsNodeDirectory;
            out.size = dynamic_proc_file_size(normalized);
            out.links = 1;
            copy_path(out.path, normalized);
            return true;
        }
        if (proc_kind == DynamicProcKind::PidStatus || proc_kind == DynamicProcKind::PidFdEntry) {
            out.type = hybrid::VfsNodeType::VirtualFile;
            out.flags = hybrid::VfsNodeReadable | hybrid::VfsNodeVirtual;
            out.size = dynamic_proc_file_size(normalized);
            out.links = 1;
            copy_path(out.path, normalized);
            return out.size != 0;
        }
    }
    const Node* node = find(path);
    if (!node || node->type == NodeType::Empty || node->path == nullptr) return false;
    out.type = abi_type(node->type);
    out.flags = abi_flags(*node);
    out.size = node_size_for(*node);
    out.links = link_count_for(nodes_, count_, *node);
    copy_path(out.path, node->path);
    return true;
}

bool self_test() {
    if (vfs().node_count() < 12 || vfs().memory_file_count() < 9) return false;
    const uint32_t open_handle_baseline = vfs().open_handle_count();
    if (!vfs().find("/") || !vfs().find("/boot") || !vfs().find("/user") || !vfs().find("/bin") ||
        !vfs().find("/etc") || !vfs().find("/proc") || !vfs().find("/dev") || !vfs().find("/tmp") || !vfs().find("/disk")) return false;
    const Node* dev_null = vfs().find("/dev/null");
    const Node* dev_zero = vfs().find("/dev/zero");
    const Node* dev_tty = vfs().find("/dev/tty");
    const Node* dev_console = vfs().find("/dev/console");
    if (!dev_null || dev_null->type != NodeType::CharacterDevice || dev_null->device_kind != DeviceKind::Null ||
        !dev_zero || dev_zero->type != NodeType::CharacterDevice || dev_zero->device_kind != DeviceKind::Zero ||
        !dev_tty || dev_tty->type != NodeType::CharacterDevice || dev_tty->device_kind != DeviceKind::Tty ||
        !dev_console || dev_console->type != NodeType::CharacterDevice || dev_console->device_kind != DeviceKind::Console) {
        return false;
    }
    const Node* os_release = vfs().find("/etc/os-release");
    if (!os_release || os_release->type != NodeType::MemoryFile || os_release->size < 16) return false;
    const Node* proc_version = vfs().find("/proc/version");
    if (!proc_version || proc_version->type != NodeType::MemoryFile || proc_version->size < 16) return false;
    const Node* proc_cpuinfo = vfs().find("/proc/cpuinfo");
    if (!proc_cpuinfo || proc_cpuinfo->type != NodeType::MemoryFile || proc_cpuinfo->size < 16) return false;
    const Node* proc_block = vfs().find("/proc/block");
    const Node* proc_block_bootdisk = vfs().find("/proc/block/bootdisk");
    const Node* proc_driver = vfs().find("/proc/driver");
    const Node* proc_driver_summary = vfs().find("/proc/driver/summary");
    const Node* proc_driver_devices = vfs().find("/proc/driver/devices");
    const Node* proc_pci = vfs().find("/proc/pci");
    const Node* proc_pci_summary = vfs().find("/proc/pci/summary");
    const Node* proc_pci_devices = vfs().find("/proc/pci/devices");
    const Node* proc_irq = vfs().find("/proc/irq");
    const Node* proc_irq_summary = vfs().find("/proc/irq/summary");
    const Node* proc_interrupts = vfs().find("/proc/interrupts");
    const Node* proc_tty = vfs().find("/proc/tty");
    const Node* proc_tty_summary = vfs().find("/proc/tty/summary");
    const Node* proc_cpu = vfs().find("/proc/cpu");
    const Node* proc_cpu_summary = vfs().find("/proc/cpu/summary");
    const Node* proc_cpu_topology = vfs().find("/proc/cpu/topology");
    const Node* proc_mm = vfs().find("/proc/mm");
    const Node* proc_mm_summary = vfs().find("/proc/mm/summary");
    const Node* proc_buddyinfo = vfs().find("/proc/buddyinfo");
    const Node* proc_kmsg = vfs().find("/proc/kmsg");
    const Node* proc_net = vfs().find("/proc/net");
    const Node* proc_net_summary = vfs().find("/proc/net/summary");
    const Node* proc_net_dev = vfs().find("/proc/net/dev");
    const Node* proc_self = vfs().find("/proc/self");
    const Node* proc_meminfo = vfs().find("/proc/meminfo");
    const Node* proc_uptime = vfs().find("/proc/uptime");
    const Node* proc_loadavg = vfs().find("/proc/loadavg");
    const Node* proc_sched_debug = vfs().find("/proc/sched_debug");
    const Node* proc_stat = vfs().find("/proc/stat");
    const Node* proc_processes = vfs().find("/proc/processes");
    const Node* proc_modules = vfs().find("/proc/modules");
    const Node* proc_mounts = vfs().find("/proc/mounts");
    const Node* proc_filesystems = vfs().find("/proc/filesystems");
    const Node* proc_fs = vfs().find("/proc/fs");
    const Node* proc_vfs = vfs().find("/proc/fs/vfs");
    const Node* proc_cmdline = vfs().find("/proc/cmdline");
    const Node* proc_sys = vfs().find("/proc/sys");
    const Node* proc_sys_kernel = vfs().find("/proc/sys/kernel");
    const Node* proc_hostname = vfs().find("/proc/sys/kernel/hostname");
    const Node* proc_ostype = vfs().find("/proc/sys/kernel/ostype");
    const Node* proc_osrelease = vfs().find("/proc/sys/kernel/osrelease");
    const Node* proc_kernel_version = vfs().find("/proc/sys/kernel/version");
    const Node* proc_self_status = vfs().find("/proc/self/status");
    const Node* proc_self_fd = vfs().find("/proc/self/fd");
    if (!proc_block || proc_block->type != NodeType::Directory ||
        !proc_driver || proc_driver->type != NodeType::Directory ||
        !proc_pci || proc_pci->type != NodeType::Directory ||
        !proc_irq || proc_irq->type != NodeType::Directory ||
        !proc_tty || proc_tty->type != NodeType::Directory ||
        !proc_cpu || proc_cpu->type != NodeType::Directory ||
        !proc_mm || proc_mm->type != NodeType::Directory ||
        !proc_net || proc_net->type != NodeType::Directory ||
        !proc_self || proc_self->type != NodeType::Directory) return false;
    if (!proc_fs || proc_fs->type != NodeType::Directory ||
        !proc_sys || proc_sys->type != NodeType::Directory ||
        !proc_sys_kernel || proc_sys_kernel->type != NodeType::Directory) return false;
    if (!proc_meminfo || proc_meminfo->type != NodeType::VirtualFile || proc_meminfo->virtual_kind != VirtualFileKind::ProcMeminfo ||
        !proc_uptime || proc_uptime->type != NodeType::VirtualFile || proc_uptime->virtual_kind != VirtualFileKind::ProcUptime ||
        !proc_loadavg || proc_loadavg->type != NodeType::VirtualFile || proc_loadavg->virtual_kind != VirtualFileKind::ProcLoadavg ||
        !proc_sched_debug || proc_sched_debug->type != NodeType::VirtualFile || proc_sched_debug->virtual_kind != VirtualFileKind::ProcSchedDebug ||
        !proc_stat || proc_stat->type != NodeType::VirtualFile || proc_stat->virtual_kind != VirtualFileKind::ProcStat ||
        !proc_processes || proc_processes->type != NodeType::VirtualFile || proc_processes->virtual_kind != VirtualFileKind::ProcProcesses ||
        !proc_modules || proc_modules->type != NodeType::VirtualFile || proc_modules->virtual_kind != VirtualFileKind::ProcModules ||
        !proc_mounts || proc_mounts->type != NodeType::VirtualFile || proc_mounts->virtual_kind != VirtualFileKind::ProcMounts ||
        !proc_filesystems || proc_filesystems->type != NodeType::VirtualFile || proc_filesystems->virtual_kind != VirtualFileKind::ProcFilesystems ||
        !proc_vfs || proc_vfs->type != NodeType::VirtualFile || proc_vfs->virtual_kind != VirtualFileKind::ProcVfsStats ||
        !proc_block_bootdisk || proc_block_bootdisk->type != NodeType::VirtualFile || proc_block_bootdisk->virtual_kind != VirtualFileKind::ProcBlockBootdisk ||
        !proc_driver_summary || proc_driver_summary->type != NodeType::VirtualFile || proc_driver_summary->virtual_kind != VirtualFileKind::ProcDriverSummary ||
        !proc_driver_devices || proc_driver_devices->type != NodeType::VirtualFile || proc_driver_devices->virtual_kind != VirtualFileKind::ProcDriverDevices ||
        !proc_pci_summary || proc_pci_summary->type != NodeType::VirtualFile || proc_pci_summary->virtual_kind != VirtualFileKind::ProcPciSummary ||
        !proc_pci_devices || proc_pci_devices->type != NodeType::VirtualFile || proc_pci_devices->virtual_kind != VirtualFileKind::ProcPciDevices ||
        !proc_irq_summary || proc_irq_summary->type != NodeType::VirtualFile || proc_irq_summary->virtual_kind != VirtualFileKind::ProcIrqSummary ||
        !proc_interrupts || proc_interrupts->type != NodeType::VirtualFile || proc_interrupts->virtual_kind != VirtualFileKind::ProcInterrupts ||
        !proc_tty_summary || proc_tty_summary->type != NodeType::VirtualFile || proc_tty_summary->virtual_kind != VirtualFileKind::ProcTtySummary ||
        !proc_cpu_summary || proc_cpu_summary->type != NodeType::VirtualFile || proc_cpu_summary->virtual_kind != VirtualFileKind::ProcCpuSummary ||
        !proc_cpu_topology || proc_cpu_topology->type != NodeType::VirtualFile || proc_cpu_topology->virtual_kind != VirtualFileKind::ProcCpuTopology ||
        !proc_mm_summary || proc_mm_summary->type != NodeType::VirtualFile || proc_mm_summary->virtual_kind != VirtualFileKind::ProcMmSummary ||
        !proc_buddyinfo || proc_buddyinfo->type != NodeType::VirtualFile || proc_buddyinfo->virtual_kind != VirtualFileKind::ProcBuddyinfo ||
        !proc_kmsg || proc_kmsg->type != NodeType::VirtualFile || proc_kmsg->virtual_kind != VirtualFileKind::ProcKmsg ||
        !proc_net_summary || proc_net_summary->type != NodeType::VirtualFile || proc_net_summary->virtual_kind != VirtualFileKind::ProcNetSummary ||
        !proc_net_dev || proc_net_dev->type != NodeType::VirtualFile || proc_net_dev->virtual_kind != VirtualFileKind::ProcNetDev ||
        !proc_cmdline || proc_cmdline->type != NodeType::VirtualFile || proc_cmdline->virtual_kind != VirtualFileKind::ProcCmdline ||
        !proc_hostname || proc_hostname->type != NodeType::VirtualFile || proc_hostname->virtual_kind != VirtualFileKind::ProcHostname ||
        !proc_ostype || proc_ostype->type != NodeType::VirtualFile || proc_ostype->virtual_kind != VirtualFileKind::ProcOstype ||
        !proc_osrelease || proc_osrelease->type != NodeType::VirtualFile || proc_osrelease->virtual_kind != VirtualFileKind::ProcOsrelease ||
        !proc_kernel_version || proc_kernel_version->type != NodeType::VirtualFile || proc_kernel_version->virtual_kind != VirtualFileKind::ProcVersionString ||
        !proc_self_status || proc_self_status->type != NodeType::VirtualFile || proc_self_status->virtual_kind != VirtualFileKind::ProcSelfStatus ||
        !proc_self_fd || proc_self_fd->type != NodeType::VirtualFile || proc_self_fd->virtual_kind != VirtualFileKind::ProcSelfFd) {
        return false;
    }
    char proc_buffer[32];
    if (vfs().read("/proc/meminfo", 0, proc_buffer, 9) != 9 || proc_buffer[0] != 'M' || proc_buffer[3] != 'T') return false;
    uint32_t proc_handle = vfs().open("/proc/uptime");
    if (proc_handle == 0) return false;
    char uptime_buffer[8];
    if (vfs().read_handle(proc_handle, uptime_buffer, 6) != 6 || uptime_buffer[0] != 't' || uptime_buffer[5] != ' ') return false;
    if (!vfs().close(proc_handle)) return false;
    if (vfs().read("/proc/loadavg", 0, proc_buffer, 4) != 4 || proc_buffer[1] != '.' || proc_buffer[2] != '0') return false;
    if (vfs().read("/proc/sched_debug", 0, proc_buffer, 8) != 8 || proc_buffer[0] != 'M' || proc_buffer[7] != 's') return false;
    if (vfs().read("/proc/processes", 0, proc_buffer, 7) != 7 || proc_buffer[0] != 'P' || proc_buffer[4] != 'P') return false;
    if (vfs().read("/proc/modules", 0, proc_buffer, 6) != 6 || proc_buffer[0] != 'N' || proc_buffer[5] != 'S') return false;
    if (vfs().read("/proc/stat", 0, proc_buffer, 4) != 4 || proc_buffer[0] != 'c' || proc_buffer[3] != ' ') return false;
    if (vfs().read("/proc/mounts", 0, proc_buffer, 6) != 6 || proc_buffer[0] != 'b' || proc_buffer[5] != 'm') return false;
    if (vfs().read("/proc/filesystems", 0, proc_buffer, 7) != 7 || proc_buffer[0] != 'n' || proc_buffer[5] != '\t') return false;
    if (vfs().read("/proc/fs/vfs", 0, proc_buffer, 8) != 8 || proc_buffer[0] != 'r' || proc_buffer[4] != 'f') return false;
    if (vfs().read("/proc/block/bootdisk", 0, proc_buffer, 11) != 11 ||
        proc_buffer[0] != 'i' || proc_buffer[10] != 'd') return false;
    if (vfs().read("/proc/driver/summary", 0, proc_buffer, 10) != 10 ||
        proc_buffer[0] != 'r' || proc_buffer[9] != 'd') return false;
    if (vfs().read("/proc/driver/devices", 0, proc_buffer, 6) != 6 ||
        proc_buffer[0] != 'D' || proc_buffer[5] != 'R') return false;
    if (vfs().read("/proc/pci/summary", 0, proc_buffer, 7) != 7 ||
        proc_buffer[0] != 's' || proc_buffer[6] != 'd') return false;
    if (vfs().read("/proc/pci/devices", 0, proc_buffer, 3) != 3 ||
        proc_buffer[0] != 'B' || proc_buffer[2] != 'F') return false;
    if (vfs().read("/proc/irq/summary", 0, proc_buffer, 10) != 10 ||
        proc_buffer[0] != 'p' || proc_buffer[9] != 's') return false;
    if (vfs().read("/proc/interrupts", 0, proc_buffer, 10) != 10 ||
        proc_buffer[0] != 'V' || proc_buffer[7] != 'C') return false;
    if (vfs().read("/proc/tty/summary", 0, proc_buffer, 10) != 10 ||
        proc_buffer[0] != 'i' || proc_buffer[5] != '_') return false;
    if (vfs().read("/proc/cpu/summary", 0, proc_buffer, 5) != 5 ||
        proc_buffer[0] != 'c' || proc_buffer[4] != ' ') return false;
    if (vfs().read("/proc/cpu/topology", 0, proc_buffer, 7) != 7 ||
        proc_buffer[0] != 'C' || proc_buffer[6] != 'I') return false;
    if (vfs().read("/proc/mm/summary", 0, proc_buffer, 9) != 9 ||
        proc_buffer[0] != 'p' || proc_buffer[4] != 't') return false;
    if (vfs().read("/proc/buddyinfo", 0, proc_buffer, 6) != 6 ||
        proc_buffer[0] != 'N' || proc_buffer[5] != '0') return false;
    if (vfs().read("/proc/kmsg", 0, proc_buffer, 7) != 7) return false;
    if (vfs().read("/proc/net/summary", 0, proc_buffer, 10) != 10 ||
        proc_buffer[0] != 'i' || proc_buffer[9] != 's') return false;
    if (vfs().read("/proc/net/dev", 0, proc_buffer, 6) != 6 ||
        proc_buffer[0] != 'I' || proc_buffer[5] != ' ') return false;
    if (vfs().read("/proc/cmdline", 0, proc_buffer, 11) != 11 || proc_buffer[0] != 'B' || proc_buffer[10] != '=') return false;
    if (vfs().read("/proc/sys/kernel/hostname", 0, proc_buffer, 6) != 6 ||
        proc_buffer[0] != 'i' || proc_buffer[4] != 's' || proc_buffer[5] != '\n') return false;
    if (vfs().read("/proc/sys/kernel/ostype", 0, proc_buffer, 6) != 6 ||
        proc_buffer[0] != 'I' || proc_buffer[4] != 'S' || proc_buffer[5] != '\n') return false;
    if (vfs().read("/proc/sys/kernel/osrelease", 0, proc_buffer, 14) != 14 ||
        proc_buffer[0] != '0' || proc_buffer[13] != '\n') return false;
    if (vfs().read("/proc/sys/kernel/version", 0, proc_buffer, 22) != 22 ||
        proc_buffer[0] != 'M' || proc_buffer[21] != 'x') return false;
    if (vfs().read("/proc/self/status", 0, proc_buffer, 6) != 6 || proc_buffer[0] != 'N' || proc_buffer[4] != ':') return false;
    if (vfs().read("/proc/self/fd", 0, proc_buffer, 7) != 7 || proc_buffer[0] != 'F' || proc_buffer[3] != 'K') return false;
    hybrid::VfsStatInfo proc_init_stat{};
    if (!vfs().stat("/proc/1", proc_init_stat) || proc_init_stat.type != hybrid::VfsNodeType::Directory) return false;
    if (vfs().read("/proc/1/status", 0, proc_buffer, 6) != 6 || proc_buffer[0] != 'N' || proc_buffer[4] != ':') return false;
    if (vfs().read("/proc/1/fd", 0, proc_buffer, 7) != 7 || proc_buffer[0] != 'F' || proc_buffer[3] != 'K') return false;
    hk::log(hk::LogLevel::Info, "VFS proc virtual file self-test");
    const Node* disk_boot = vfs().find("/disk/bootsector.bin");
    if (!disk_boot || disk_boot->type != NodeType::MemoryFile || disk_boot->size != 512) return false;
    unsigned char sector_sig[2]{};
    if (vfs().read("/disk/bootsector.bin", 510, sector_sig, sizeof(sector_sig)) != sizeof(sector_sig)) return false;
    if (sector_sig[0] != 0x55 || sector_sig[1] != 0xaa) return false;
    const Node* mount_root = vfs().find("/mnt");
    const Node* mount_boot = vfs().find("/mnt/boot");
    const Node* mounted_kernel = vfs().find("/mnt/boot/kernel.elf");
    const Node* mounted_bin = vfs().find("/mnt/boot/bin");
    const Node* mounted_hello = vfs().find("/mnt/boot/bin/hello.elf");
    const Node* mounted_user = vfs().find("/mnt/boot/user");
    const Node* mounted_init = vfs().find("/mnt/boot/user/init.elf");
    if (!mount_root || mount_root->type != NodeType::Directory ||
        !mount_boot || mount_boot->type != NodeType::Directory ||
        !mounted_kernel || mounted_kernel->type != NodeType::MemoryFile ||
        !mounted_kernel->disk_backed || mounted_kernel->size < 4 ||
        !mounted_bin || mounted_bin->type != NodeType::Directory ||
        !mounted_hello || mounted_hello->type != NodeType::MemoryFile ||
        !mounted_hello->disk_backed || mounted_hello->size < 4 ||
        !mounted_user || mounted_user->type != NodeType::Directory ||
        !mounted_init || mounted_init->type != NodeType::MemoryFile ||
        !mounted_init->disk_backed || mounted_init->size < 4) {
        return false;
    }
    unsigned char mounted_magic[4]{};
    if (vfs().read("/mnt/boot/kernel.elf", 0, mounted_magic, sizeof(mounted_magic)) != sizeof(mounted_magic)) return false;
    if (mounted_magic[0] != 0x7f || mounted_magic[1] != 'E' || mounted_magic[2] != 'L' || mounted_magic[3] != 'F') return false;
    if (vfs().read("/mnt/boot/bin/hello.elf", 0, mounted_magic, sizeof(mounted_magic)) != sizeof(mounted_magic)) return false;
    if (mounted_magic[0] != 0x7f || mounted_magic[1] != 'E' || mounted_magic[2] != 'L' || mounted_magic[3] != 'F') return false;
    const Node* init = vfs().find("/user/init.elf");
    if (!init || init->type != NodeType::MemoryFile || init->size < 4) return false;
    const Node* hello = vfs().find("/bin/hello.elf");
    if (!hello || hello->type != NodeType::MemoryFile || hello->size < 4) return false;
    const Node* args = vfs().find("/bin/args.elf");
    if (!args || args->type != NodeType::MemoryFile || args->size < 4) return false;
    const Node* cat = vfs().find("/bin/cat.elf");
    if (!cat || cat->type != NodeType::MemoryFile || cat->size < 4) return false;
    const Node* ls = vfs().find("/bin/ls.elf");
    if (!ls || ls->type != NodeType::MemoryFile || ls->size < 4) return false;
    const Node* uname = vfs().find("/bin/uname.elf");
    if (!uname || uname->type != NodeType::MemoryFile || uname->size < 4) return false;
    const Node* free = vfs().find("/bin/free.elf");
    if (!free || free->type != NodeType::MemoryFile || free->size < 4) return false;
    const Node* uptime = vfs().find("/bin/uptime.elf");
    if (!uptime || uptime->type != NodeType::MemoryFile || uptime->size < 4) return false;
    const Node* date_cmd = vfs().find("/bin/date.elf");
    if (!date_cmd || date_cmd->type != NodeType::MemoryFile || date_cmd->size < 4) return false;
    const Node* dmesg = vfs().find("/bin/dmesg.elf");
    if (!dmesg || dmesg->type != NodeType::MemoryFile || dmesg->size < 4) return false;
    const Node* kmsg = vfs().find("/bin/kmsg.elf");
    if (!kmsg || kmsg->type != NodeType::MemoryFile || kmsg->size < 4) return false;
    const Node* loadavg = vfs().find("/bin/loadavg.elf");
    if (!loadavg || loadavg->type != NodeType::MemoryFile || loadavg->size < 4) return false;
    const Node* ps = vfs().find("/bin/ps.elf");
    if (!ps || ps->type != NodeType::MemoryFile || ps->size < 4) return false;
    const Node* pwd = vfs().find("/bin/pwd.elf");
    if (!pwd || pwd->type != NodeType::MemoryFile || pwd->size < 4) return false;
    const Node* env = vfs().find("/bin/env.elf");
    if (!env || env->type != NodeType::MemoryFile || env->size < 4) return false;
    const Node* sysinfo = vfs().find("/bin/sysinfo.elf");
    if (!sysinfo || sysinfo->type != NodeType::MemoryFile || sysinfo->size < 4) return false;
    const Node* ctx_cmd = vfs().find("/bin/ctx.elf");
    if (!ctx_cmd || ctx_cmd->type != NodeType::MemoryFile || ctx_cmd->size < 4) return false;
    const Node* echo = vfs().find("/bin/echo.elf");
    if (!echo || echo->type != NodeType::MemoryFile || echo->size < 4) return false;
    const Node* sleep = vfs().find("/bin/sleep.elf");
    if (!sleep || sleep->type != NodeType::MemoryFile || sleep->size < 4) return false;
    const Node* true_cmd = vfs().find("/bin/true.elf");
    if (!true_cmd || true_cmd->type != NodeType::MemoryFile || true_cmd->size < 4) return false;
    const Node* false_cmd = vfs().find("/bin/false.elf");
    if (!false_cmd || false_cmd->type != NodeType::MemoryFile || false_cmd->size < 4) return false;
    const Node* touch_cmd = vfs().find("/bin/touch.elf");
    if (!touch_cmd || touch_cmd->type != NodeType::MemoryFile || touch_cmd->size < 4) return false;
    const Node* append_cmd = vfs().find("/bin/append.elf");
    if (!append_cmd || append_cmd->type != NodeType::MemoryFile || append_cmd->size < 4) return false;
    const Node* rm_cmd = vfs().find("/bin/rm.elf");
    if (!rm_cmd || rm_cmd->type != NodeType::MemoryFile || rm_cmd->size < 4) return false;
    const Node* cp_cmd = vfs().find("/bin/cp.elf");
    if (!cp_cmd || cp_cmd->type != NodeType::MemoryFile || cp_cmd->size < 4) return false;
    const Node* mv_cmd = vfs().find("/bin/mv.elf");
    if (!mv_cmd || mv_cmd->type != NodeType::MemoryFile || mv_cmd->size < 4) return false;
    const Node* wc_cmd = vfs().find("/bin/wc.elf");
    if (!wc_cmd || wc_cmd->type != NodeType::MemoryFile || wc_cmd->size < 4) return false;
    const Node* grep_cmd = vfs().find("/bin/grep.elf");
    if (!grep_cmd || grep_cmd->type != NodeType::MemoryFile || grep_cmd->size < 4) return false;
    const Node* tee_cmd = vfs().find("/bin/tee.elf");
    if (!tee_cmd || tee_cmd->type != NodeType::MemoryFile || tee_cmd->size < 4) return false;
    const Node* mkdir_cmd = vfs().find("/bin/mkdir.elf");
    if (!mkdir_cmd || mkdir_cmd->type != NodeType::MemoryFile || mkdir_cmd->size < 4) return false;
    const Node* rmdir_cmd = vfs().find("/bin/rmdir.elf");
    if (!rmdir_cmd || rmdir_cmd->type != NodeType::MemoryFile || rmdir_cmd->size < 4) return false;
    const Node* err_cmd = vfs().find("/bin/err.elf");
    if (!err_cmd || err_cmd->type != NodeType::MemoryFile || err_cmd->size < 4) return false;
    const Node* stat_cmd = vfs().find("/bin/stat.elf");
    if (!stat_cmd || stat_cmd->type != NodeType::MemoryFile || stat_cmd->size < 4) return false;
    const Node* whoami_cmd = vfs().find("/bin/whoami.elf");
    if (!whoami_cmd || whoami_cmd->type != NodeType::MemoryFile || whoami_cmd->size < 4) return false;
    const Node* basename_cmd = vfs().find("/bin/basename.elf");
    if (!basename_cmd || basename_cmd->type != NodeType::MemoryFile || basename_cmd->size < 4) return false;
    const Node* dirname_cmd = vfs().find("/bin/dirname.elf");
    if (!dirname_cmd || dirname_cmd->type != NodeType::MemoryFile || dirname_cmd->size < 4) return false;
    const Node* head_cmd = vfs().find("/bin/head.elf");
    if (!head_cmd || head_cmd->type != NodeType::MemoryFile || head_cmd->size < 4) return false;
    const Node* tail_cmd = vfs().find("/bin/tail.elf");
    if (!tail_cmd || tail_cmd->type != NodeType::MemoryFile || tail_cmd->size < 4) return false;
    const Node* test_cmd = vfs().find("/bin/test.elf");
    if (!test_cmd || test_cmd->type != NodeType::MemoryFile || test_cmd->size < 4) return false;
    const Node* sort_cmd = vfs().find("/bin/sort.elf");
    if (!sort_cmd || sort_cmd->type != NodeType::MemoryFile || sort_cmd->size < 4) return false;
    const Node* uniq_cmd = vfs().find("/bin/uniq.elf");
    if (!uniq_cmd || uniq_cmd->type != NodeType::MemoryFile || uniq_cmd->size < 4) return false;
    const Node* sh_cmd = vfs().find("/bin/sh.elf");
    if (!sh_cmd || sh_cmd->type != NodeType::MemoryFile || sh_cmd->size < 4) return false;
    const Node* lsof_cmd = vfs().find("/bin/lsof.elf");
    if (!lsof_cmd || lsof_cmd->type != NodeType::MemoryFile || lsof_cmd->size < 4) return false;
    const Node* fdinh_cmd = vfs().find("/bin/fdinh.elf");
    if (!fdinh_cmd || fdinh_cmd->type != NodeType::MemoryFile || fdinh_cmd->size < 4) return false;
    const Node* blk_cmd = vfs().find("/bin/blk.elf");
    if (!blk_cmd || blk_cmd->type != NodeType::MemoryFile || blk_cmd->size < 4) return false;
    const Node* mount_cmd = vfs().find("/bin/mount.elf");
    if (!mount_cmd || mount_cmd->type != NodeType::MemoryFile || mount_cmd->size < 4) return false;
    const Node* df_cmd = vfs().find("/bin/df.elf");
    if (!df_cmd || df_cmd->type != NodeType::MemoryFile || df_cmd->size < 4) return false;
    const Node* du_cmd = vfs().find("/bin/du.elf");
    if (!du_cmd || du_cmd->type != NodeType::MemoryFile || du_cmd->size < 4) return false;
    const Node* pipeinfo_cmd = vfs().find("/bin/pipeinfo.elf");
    if (!pipeinfo_cmd || pipeinfo_cmd->type != NodeType::MemoryFile || pipeinfo_cmd->size < 4) return false;
    const Node* kill_cmd = vfs().find("/bin/kill.elf");
    if (!kill_cmd || kill_cmd->type != NodeType::MemoryFile || kill_cmd->size < 4) return false;
    const Node* killall_cmd = vfs().find("/bin/killall.elf");
    if (!killall_cmd || killall_cmd->type != NodeType::MemoryFile || killall_cmd->size < 4) return false;
    const Node* pgrep_cmd = vfs().find("/bin/pgrep.elf");
    if (!pgrep_cmd || pgrep_cmd->type != NodeType::MemoryFile || pgrep_cmd->size < 4) return false;
    const Node* burst_cmd = vfs().find("/bin/burst.elf");
    if (!burst_cmd || burst_cmd->type != NodeType::MemoryFile || burst_cmd->size < 4) return false;
    const Node* loop_cmd = vfs().find("/bin/loop.elf");
    if (!loop_cmd || loop_cmd->type != NodeType::MemoryFile || loop_cmd->size < 4) return false;
    hybrid::VfsStatInfo stat_info{};
    if (!vfs().stat("/tmp", stat_info) || stat_info.type != hybrid::VfsNodeType::Directory ||
        (stat_info.flags & hybrid::VfsNodeDirectory) == 0 || (stat_info.flags & hybrid::VfsNodeWritable) == 0) {
        return false;
    }
    if (!vfs().stat("/bin/stat.elf", stat_info) || stat_info.type != hybrid::VfsNodeType::MemoryFile ||
        (stat_info.flags & hybrid::VfsNodeMemoryBacked) == 0 || (stat_info.flags & hybrid::VfsNodeWritable) != 0) {
        return false;
    }
    if (!vfs().stat("/proc/meminfo", stat_info) || stat_info.type != hybrid::VfsNodeType::VirtualFile ||
        (stat_info.flags & hybrid::VfsNodeVirtual) == 0 || (stat_info.flags & hybrid::VfsNodeWritable) != 0 ||
        stat_info.size < 32) {
        return false;
    }
    if (!vfs().stat("/proc/self/status", stat_info) || stat_info.type != hybrid::VfsNodeType::VirtualFile ||
        (stat_info.flags & hybrid::VfsNodeVirtual) == 0 || stat_info.size < 32) {
        return false;
    }
    if (!vfs().stat("/proc/self/fd", stat_info) || stat_info.type != hybrid::VfsNodeType::VirtualFile ||
        (stat_info.flags & hybrid::VfsNodeVirtual) == 0 || stat_info.size < 12) {
        return false;
    }
    if (!vfs().stat("/mnt/boot/kernel.elf", stat_info) || stat_info.type != hybrid::VfsNodeType::MemoryFile ||
        (stat_info.flags & hybrid::VfsNodeDiskBacked) == 0 || (stat_info.flags & hybrid::VfsNodeMemoryBacked) != 0 ||
        stat_info.size < 4) {
        return false;
    }
    if (!vfs().stat("/mnt/boot/bin/hello.elf", stat_info) || stat_info.type != hybrid::VfsNodeType::MemoryFile ||
        (stat_info.flags & hybrid::VfsNodeDiskBacked) == 0 || (stat_info.flags & hybrid::VfsNodeMemoryBacked) != 0 ||
        stat_info.size < 4) {
        return false;
    }
    if (vfs().mount_count() < 2) return false;
    hybrid::MountInfo root_mount{};
    hybrid::MountInfo boot_mount{};
    if (!vfs().copy_mount_info(0, root_mount) || root_mount.path[0] != '/' ||
        root_mount.fs_type[0] != 'v' || (root_mount.flags & hybrid::MountMemoryBacked) == 0 ||
        root_mount.node_count < 12) {
        return false;
    }
    if (!vfs().copy_mount_info(1, boot_mount) || boot_mount.path[0] != '/' ||
        boot_mount.fs_type[0] != 'f' || (boot_mount.flags & hybrid::MountDiskBacked) == 0 ||
        boot_mount.node_count < 4 || boot_mount.total_bytes < mounted_kernel->size) {
        return false;
    }
    hk::log(hk::LogLevel::Info, "VFS mount table self-test");
    if (!vfs().find("/bin/./../bin//hello.elf")) return false;
    if (!vfs().stat("/mnt/boot/./bin/../kernel.elf", stat_info) ||
        stat_info.type != hybrid::VfsNodeType::MemoryFile ||
        stat_info.path[0] != '/' || stat_info.path[1] != 'm' ||
        (stat_info.flags & hybrid::VfsNodeDiskBacked) == 0) {
        return false;
    }
    hk::log(hk::LogLevel::Info, "VFS normalized path self-test");
    unsigned char magic[4]{};
    if (vfs().read("/user/./../user//init.elf", 0, magic, sizeof(magic)) != sizeof(magic)) return false;
    if (magic[0] != 0x7f || magic[1] != 'E' || magic[2] != 'L' || magic[3] != 'F') return false;
    uint32_t handle = vfs().open("/user/../user/init.elf");
    if (handle == 0 || vfs().open_handle_count() != open_handle_baseline + 1) return false;
    unsigned char handle_magic[4]{};
    if (vfs().read_handle(handle, handle_magic, sizeof(handle_magic)) != sizeof(handle_magic)) return false;
    if (handle_magic[0] != 0x7f || handle_magic[1] != 'E' || handle_magic[2] != 'L' || handle_magic[3] != 'F') return false;
    unsigned char next_byte = 0;
    if (vfs().read_handle(handle, &next_byte, sizeof(next_byte)) != sizeof(next_byte)) return false;
    if (!vfs().seek_handle(handle, 0)) return false;
    unsigned char rewind_magic[4]{};
    if (vfs().read_handle(handle, rewind_magic, sizeof(rewind_magic)) != sizeof(rewind_magic)) return false;
    if (rewind_magic[0] != 0x7f || rewind_magic[1] != 'E' || rewind_magic[2] != 'L' || rewind_magic[3] != 'F') return false;
    if (vfs().seek_handle(handle, init->size + 1)) return false;
    if (!vfs().close(handle) || vfs().open_handle_count() != open_handle_baseline) return false;
    uint32_t null_handle = vfs().open("/dev/null");
    uint32_t zero_handle = vfs().open("/dev/zero");
    if (null_handle == 0 || zero_handle == 0) return false;
    static const char null_write[] = "discarded";
    if (vfs().write_handle(null_handle, null_write, sizeof(null_write) - 1) != sizeof(null_write) - 1) return false;
    unsigned char zeroes[16];
    for (uint64_t i = 0; i < sizeof(zeroes); ++i) zeroes[i] = 0xff;
    if (vfs().read_handle(zero_handle, zeroes, sizeof(zeroes)) != sizeof(zeroes)) return false;
    for (uint64_t i = 0; i < sizeof(zeroes); ++i) if (zeroes[i] != 0) return false;
    if (!vfs().close(null_handle) || !vfs().close(zero_handle) || vfs().open_handle_count() != open_handle_baseline) return false;
    hk::log(hk::LogLevel::Info, "VFS character device self-test");
    VfsStats vfs_stats_before = vfs().stats();
    if (vfs().create_ram_file("/tmp")) return false;
    if (vfs().create_ram_file("/missing/selftest.txt")) return false;
    if (vfs().link_ram_file("/bin/hello.elf", "/tmp/readonly.link")) return false;
    if (vfs().truncate_ram_file("/bin/hello.elf", 1)) return false;
    if (vfs().rename_ram_node("/bin/hello.elf", "/tmp/readonly-renamed.elf")) return false;
    if (vfs().delete_ram_file("/bin/hello.elf")) return false;
    if (vfs().create_ram_directory("/missing/selfdir")) return false;
    if (vfs().delete_ram_directory("/")) return false;
    if (!vfs().create_ram_file("/tmp/./selftest.txt")) return false;
    if (!vfs().link_ram_file("/tmp/selftest.txt", "/tmp/./selftest.link")) return false;
    uint32_t ram = vfs().open("/tmp/../tmp/selftest.txt");
    if (ram == 0) return false;
    if (vfs().delete_ram_file("/tmp/selftest.txt")) return false;
    static const char text[] = "ramfs selftest\n";
    if (vfs().write_handle(ram, text, sizeof(text) - 1) != sizeof(text) - 1) return false;
    if (!vfs().seek_handle(ram, 0)) return false;
    char text_read[sizeof(text)]{};
    if (vfs().read_handle(ram, text_read, sizeof(text) - 1) != sizeof(text) - 1) return false;
    if (text_read[0] != 'r' || text_read[5] != ' ' || text_read[6] != 's') return false;
    if (!vfs().close(ram)) return false;
    hybrid::VfsStatInfo link_stat{};
    if (!vfs().stat("/tmp/selftest.link", link_stat) || link_stat.links != 2 || link_stat.size != sizeof(text) - 1) return false;
    uint32_t linked = vfs().open("/tmp/selftest.link");
    if (linked == 0) return false;
    static const char linked_text[] = "linked";
    if (!vfs().seek_handle(linked, 0)) return false;
    if (vfs().write_handle(linked, linked_text, sizeof(linked_text) - 1) != sizeof(linked_text) - 1) return false;
    if (!vfs().close(linked)) return false;
    char linked_read[8]{};
    if (vfs().read("/tmp/selftest.txt", 0, linked_read, sizeof(linked_text) - 1) != sizeof(linked_text) - 1) return false;
    if (linked_read[0] != 'l' || linked_read[5] != 'd') return false;
    if (!vfs().truncate_ram_file("/tmp/selftest.link", 2)) return false;
    hybrid::VfsStatInfo trunc_source{};
    hybrid::VfsStatInfo trunc_link{};
    if (!vfs().stat("/tmp/selftest.txt", trunc_source) || !vfs().stat("/tmp/selftest.link", trunc_link)) return false;
    if (trunc_source.size != 2 || trunc_link.size != 2 || trunc_source.links != 2 || trunc_link.links != 2) return false;
    if (vfs().truncate_ram_file("/tmp/selftest.txt", kMaxRamFileBytes + 1)) return false;
    if (!vfs().truncate_ram_file("/tmp/selftest.txt", 6)) return false;
    hybrid::VfsStatInfo trunc_extended{};
    if (!vfs().stat("/tmp/selftest.link", trunc_extended) || trunc_extended.size != 6 || trunc_extended.links != 2) return false;
    if (!vfs().rename_ram_node("/tmp/selftest.link", "/tmp/selftest.renamed")) return false;
    if (vfs().find("/tmp/selftest.link") || !vfs().find("/tmp/selftest.renamed")) return false;
    hybrid::VfsStatInfo renamed_link{};
    if (!vfs().stat("/tmp/selftest.renamed", renamed_link) || renamed_link.size != 6 || renamed_link.links != 2) return false;
    if (!vfs().delete_ram_file("/tmp/selftest.renamed")) return false;
    if (!vfs().stat("/tmp/selftest.txt", link_stat) || link_stat.links != 1) return false;
    if (!vfs().delete_ram_file("/tmp/./selftest.txt")) return false;
    if (!vfs().create_ram_file("/tmp/clip.bin")) return false;
    if (!vfs().truncate_ram_file("/tmp/clip.bin", kMaxRamFileBytes - 2)) return false;
    uint32_t clip = vfs().open("/tmp/clip.bin");
    if (clip == 0 || !vfs().seek_handle(clip, kMaxRamFileBytes - 2)) return false;
    static const char clip_text[] = "clip";
    if (vfs().write_handle(clip, clip_text, sizeof(clip_text) - 1) != 2) return false;
    if (!vfs().close(clip) || !vfs().delete_ram_file("/tmp/clip.bin")) return false;
    if (!vfs().create_ram_directory("/tmp/./selfdir")) return false;
    if (!vfs().create_ram_file("/tmp/selfdir/../selfdir/nested.txt")) return false;
    if (vfs().delete_ram_directory("/tmp/selfdir/.")) return false;
    if (!vfs().delete_ram_file("/tmp/selfdir/./nested.txt")) return false;
    if (!vfs().delete_ram_directory("/tmp/../tmp/selfdir")) return false;
    VfsStats vfs_stats_after = vfs().stats();
    if (vfs_stats_after.ram_file_creates < vfs_stats_before.ram_file_creates + 3 ||
        vfs_stats_after.ram_file_create_rejects < vfs_stats_before.ram_file_create_rejects + 2 ||
        vfs_stats_after.ram_directory_creates < vfs_stats_before.ram_directory_creates + 1 ||
        vfs_stats_after.ram_directory_create_rejects < vfs_stats_before.ram_directory_create_rejects + 1 ||
        vfs_stats_after.ram_links < vfs_stats_before.ram_links + 1 ||
        vfs_stats_after.ram_link_rejects < vfs_stats_before.ram_link_rejects + 1 ||
        vfs_stats_after.ram_truncates < vfs_stats_before.ram_truncates + 3 ||
        vfs_stats_after.ram_truncate_rejects < vfs_stats_before.ram_truncate_rejects + 2 ||
        vfs_stats_after.ram_renames < vfs_stats_before.ram_renames + 1 ||
        vfs_stats_after.ram_rename_rejects < vfs_stats_before.ram_rename_rejects + 1 ||
        vfs_stats_after.ram_file_deletes < vfs_stats_before.ram_file_deletes + 4 ||
        vfs_stats_after.ram_file_delete_rejects < vfs_stats_before.ram_file_delete_rejects + 2 ||
        vfs_stats_after.ram_directory_deletes < vfs_stats_before.ram_directory_deletes + 1 ||
        vfs_stats_after.ram_directory_delete_rejects < vfs_stats_before.ram_directory_delete_rejects + 2 ||
        vfs_stats_after.ram_write_bytes < vfs_stats_before.ram_write_bytes + sizeof(text) - 1 + sizeof(linked_text) - 1 + 2 ||
        vfs_stats_after.ram_write_clipped_bytes < vfs_stats_before.ram_write_clipped_bytes + 2) {
        return false;
    }
    hk::log_hex(hk::LogLevel::Info, "VFS RAM file creates", vfs_stats_after.ram_file_creates);
    hk::log_hex(hk::LogLevel::Info, "VFS RAM file create rejects", vfs_stats_after.ram_file_create_rejects);
    hk::log_hex(hk::LogLevel::Info, "VFS RAM directory creates", vfs_stats_after.ram_directory_creates);
    hk::log_hex(hk::LogLevel::Info, "VFS RAM directory create rejects", vfs_stats_after.ram_directory_create_rejects);
    hk::log_hex(hk::LogLevel::Info, "VFS RAM links", vfs_stats_after.ram_links);
    hk::log_hex(hk::LogLevel::Info, "VFS RAM link rejects", vfs_stats_after.ram_link_rejects);
    hk::log_hex(hk::LogLevel::Info, "VFS RAM truncates", vfs_stats_after.ram_truncates);
    hk::log_hex(hk::LogLevel::Info, "VFS RAM truncate rejects", vfs_stats_after.ram_truncate_rejects);
    hk::log_hex(hk::LogLevel::Info, "VFS RAM renames", vfs_stats_after.ram_renames);
    hk::log_hex(hk::LogLevel::Info, "VFS RAM rename rejects", vfs_stats_after.ram_rename_rejects);
    hk::log_hex(hk::LogLevel::Info, "VFS RAM file deletes", vfs_stats_after.ram_file_deletes);
    hk::log_hex(hk::LogLevel::Info, "VFS RAM file delete rejects", vfs_stats_after.ram_file_delete_rejects);
    hk::log_hex(hk::LogLevel::Info, "VFS RAM directory deletes", vfs_stats_after.ram_directory_deletes);
    hk::log_hex(hk::LogLevel::Info, "VFS RAM directory delete rejects", vfs_stats_after.ram_directory_delete_rejects);
    hk::log_hex(hk::LogLevel::Info, "VFS RAM write bytes", vfs_stats_after.ram_write_bytes);
    hk::log_hex(hk::LogLevel::Info, "VFS RAM clipped write bytes", vfs_stats_after.ram_write_clipped_bytes);
    hybrid::VfsNodeInfo root{};
    hybrid::VfsNodeInfo boot_kernel{};
    if (!vfs().copy_node_info(0, root) || root.type != hybrid::VfsNodeType::Directory || root.path[0] != '/') return false;
    bool found_kernel = false;
    for (uint32_t i = 0; i < vfs().node_count(); ++i) {
        if (!vfs().copy_node_info(i, boot_kernel)) return false;
        if (boot_kernel.type == hybrid::VfsNodeType::MemoryFile && boot_kernel.size >= 4 && boot_kernel.path[0] == '/' && boot_kernel.base != 0) {
            found_kernel = true;
        }
    }
    if (!found_kernel) return false;
    hybrid::VfsDirectoryEntryInfo root_entry{};
    hybrid::VfsDirectoryEntryInfo bin_entry{};
    if (!vfs().copy_directory_entry("/", 0, root_entry) || root_entry.path[0] != '/' || root_entry.name[0] == 0) return false;
    bool saw_bin_hello = false;
    for (uint32_t i = 0; i < 96; ++i) {
        hybrid::VfsDirectoryEntryInfo entry{};
        if (!vfs().copy_directory_entry("/bin", i, entry)) break;
        if (entry.type == hybrid::VfsNodeType::MemoryFile &&
            entry.name[0] == 'h' && entry.name[1] == 'e' && entry.name[2] == 'l' &&
            entry.path[0] == '/' && entry.path[1] == 'b' && entry.path[5] == 'h') {
            saw_bin_hello = true;
        }
        if (entry.path[0] == '/' && entry.path[1] == 'b' && entry.path[2] == 'i' &&
            entry.path[3] == 'n' && entry.path[4] == '/' && entry.name[0] != 0) {
            bin_entry = entry;
        }
    }
    if (!saw_bin_hello || bin_entry.name[0] == 0) return false;
    hk::log(hk::LogLevel::Info, "VFS directory entry self-test");
    return true;
}

} // namespace hk::fs
