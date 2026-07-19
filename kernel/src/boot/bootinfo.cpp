#include "hk/boot/bootinfo.hpp"

namespace hk::boot {
namespace {
hybrid::FramebufferInfo retained_framebuffer{};
hybrid::BootInfo retained_boot{};
}

void retain_boot_info(const hybrid::BootInfo& boot) {
    retained_boot = boot;
    retained_framebuffer = boot.framebuffer;
}

const hybrid::FramebufferInfo& framebuffer_info() {
    return retained_framebuffer;
}

const hybrid::BootInfo& retained_boot_info() {
    return retained_boot;
}

ValidationResult validate_boot_info(const hybrid::BootInfo* boot) {
    if (!boot) return {false, "BootInfo pointer is null"};
    if (boot->magic != hybrid::kBootInfoMagic) return {false, "BootInfo magic mismatch"};
    if (boot->version != hybrid::kBootInfoVersion) return {false, "BootInfo version mismatch"};
    if (boot->size < sizeof(hybrid::BootInfo)) return {false, "BootInfo size is too small"};
    if (boot->memory_map == 0) return {false, "memory map address is zero"};
    if (boot->memory_map_entries == 0 || boot->memory_map_entries > 4096) return {false, "memory map entry count is invalid"};
    if (boot->memory_map_descriptor_size < sizeof(hybrid::MemoryRegion)) return {false, "memory map descriptor size is invalid"};
    if (boot->framebuffer.base == 0) return {false, "framebuffer base is zero"};
    if (boot->framebuffer.width == 0 || boot->framebuffer.height == 0) return {false, "framebuffer dimensions are invalid"};
    if (boot->framebuffer.pixels_per_scanline < boot->framebuffer.width) return {false, "framebuffer pitch is invalid"};
    if (boot->framebuffer.bytes_per_pixel < 4) return {false, "framebuffer pixel size is unsupported"};
    if (boot->kernel_physical_base == 0 || boot->kernel_physical_end <= boot->kernel_physical_base) return {false, "kernel physical range is invalid"};
    if (boot->kernel_entry < boot->kernel_physical_base || boot->kernel_entry >= boot->kernel_physical_end) return {false, "kernel entry is outside kernel image"};
    if (boot->user_init_base == 0 || boot->user_init_size < 64) return {false, "user init image is missing"};
    if (boot->boot_modules == 0 || boot->boot_module_count == 0 || boot->boot_module_count > hybrid::kMaxBootModules) return {false, "boot module table is invalid"};
    return {true, "ok"};
}

} // namespace hk::boot
