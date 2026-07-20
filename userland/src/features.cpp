#include "hybrid/user.hpp"

namespace {

struct FeatureName {
    uint64_t flag;
    const char* name;
};

constexpr FeatureName kFeatures[] = {
    {hybrid::KernelFeatureUefiBoot, "uefi_boot"},
    {hybrid::KernelFeatureFramebufferConsole, "framebuffer_console"},
    {hybrid::KernelFeatureSerialLog, "serial_log"},
    {hybrid::KernelFeatureGdt, "gdt"},
    {hybrid::KernelFeatureIdt, "idt"},
    {hybrid::KernelFeatureSyscalls, "syscalls"},
    {hybrid::KernelFeaturePmmBitmap, "pmm_bitmap"},
    {hybrid::KernelFeatureVmmPageTables, "vmm_page_tables"},
    {hybrid::KernelFeatureKernelHeap, "kernel_heap"},
    {hybrid::KernelFeatureVfs, "vfs"},
    {hybrid::KernelFeatureRamFs, "ramfs"},
    {hybrid::KernelFeatureProcFs, "procfs"},
    {hybrid::KernelFeatureDevFs, "devfs"},
    {hybrid::KernelFeatureFat16Mount, "fat16_mount"},
    {hybrid::KernelFeatureElfUserspace, "elf_userspace"},
    {hybrid::KernelFeatureScheduler, "scheduler"},
    {hybrid::KernelFeaturePreemption, "preemption"},
    {hybrid::KernelFeaturePipes, "pipes"},
    {hybrid::KernelFeatureJobControl, "job_control"},
    {hybrid::KernelFeatureSmp, "smp"},
    {hybrid::KernelFeatureLocalApic, "local_apic"},
    {hybrid::KernelFeatureIoApic, "io_apic"},
    {hybrid::KernelFeaturePci, "pci"},
    {hybrid::KernelFeatureAhci, "ahci"},
    {hybrid::KernelFeatureE1000, "e1000"},
    {hybrid::KernelFeaturePs2Keyboard, "ps2_keyboard"},
    {hybrid::KernelFeatureTtyScrollback, "tty_scrollback"},
    {hybrid::KernelFeatureRecoveryMode, "recovery_mode"},
    {hybrid::KernelFeatureDebugBoot, "debug_boot"},
    {hybrid::KernelFeatureBlockCache, "block_cache"},
    {hybrid::KernelFeatureProcfsTasks, "procfs_tasks"},
    {hybrid::KernelFeatureProcfsIo, "procfs_io"},
};

void write_value(const char* label, uint64_t value) {
    hybrid::user::write_hex_line("[features] ", label, value);
}

void write_feature(const char* name, bool enabled) {
    hybrid::user::write_text_line("[features] ", enabled ? "on " : "off ", name);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::FeatureInfo info{};
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetFeatureInfo, reinterpret_cast<uint64_t>(&info));
    if (result.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[features] ", "error ", result.error);
        hybrid::user::exit(1);
    }

    write_value("flags ", info.flags);
    write_value("experimental ", info.experimental_flags);
    write_value("stable_count ", info.stable_count);
    write_value("experimental_count ", info.experimental_count);
    for (uint64_t i = 0; i < sizeof(kFeatures) / sizeof(kFeatures[0]); ++i) {
        write_feature(kFeatures[i].name, (info.flags & kFeatures[i].flag) != 0);
    }

    constexpr uint64_t required =
        hybrid::KernelFeatureUefiBoot |
        hybrid::KernelFeatureSyscalls |
        hybrid::KernelFeatureVfs |
        hybrid::KernelFeatureElfUserspace |
        hybrid::KernelFeatureScheduler |
        hybrid::KernelFeaturePci |
        hybrid::KernelFeatureProcfsTasks |
        hybrid::KernelFeatureProcfsIo;
    bool ok = (info.flags & required) == required &&
        info.stable_count == sizeof(kFeatures) / sizeof(kFeatures[0]) &&
        info.experimental_count == 0;
    hybrid::user::exit(ok ? 0 : 2);
}
