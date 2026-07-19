#pragma once

#include <stddef.h>
#include <stdint.h>

namespace hybrid {

constexpr uint32_t kBootInfoMagic = 0x484B524E; // HKRN
constexpr uint32_t kBootInfoVersion = 2;
constexpr uint32_t kBootFlagRunBootScript = 1u << 0;
constexpr uint32_t kBootFlagRecovery = 1u << 1;
constexpr uint32_t kBootFlagDebug = 1u << 2;
constexpr uint64_t kMaxBootModules = 160;

enum class MemoryType : uint32_t {
    Reserved = 0,
    Usable = 1,
    BootServices = 2,
    RuntimeServices = 3,
    AcpiReclaim = 4,
    AcpiNvs = 5,
    Mmio = 6,
    Bad = 7,
};

struct [[gnu::packed]] FramebufferInfo {
    uint64_t base;
    uint32_t width;
    uint32_t height;
    uint32_t pixels_per_scanline;
    uint32_t bytes_per_pixel;
    uint32_t format;
    uint32_t red_mask;
    uint32_t green_mask;
    uint32_t blue_mask;
    uint32_t reserved_mask;
};

struct [[gnu::packed]] MemoryRegion {
    uint64_t base;
    uint64_t length;
    MemoryType type;
    uint32_t attributes;
};

struct [[gnu::packed]] BootModule {
    uint64_t base;
    uint64_t size;
    char path[64];
};

struct [[gnu::packed]] BootInfo {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t flags;
    uint64_t rsdp;
    FramebufferInfo framebuffer;
    uint64_t memory_map;
    uint64_t memory_map_entries;
    uint64_t memory_map_descriptor_size;
    uint64_t kernel_physical_base;
    uint64_t kernel_physical_end;
    uint64_t kernel_entry;
    uint64_t user_init_base;
    uint64_t user_init_size;
    uint64_t boot_modules;
    uint64_t boot_module_count;
    uint64_t hhdm_offset;
};

static_assert(sizeof(FramebufferInfo) == 44);
static_assert(sizeof(MemoryRegion) == 24);
static_assert(sizeof(BootModule) == 80);
static_assert(offsetof(BootInfo, rsdp) == 16);
static_assert(offsetof(BootInfo, framebuffer) == 24);
static_assert(sizeof(BootInfo) == 156);

} // namespace hybrid
