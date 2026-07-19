#pragma once
#include "hybrid/boot_info.hpp"

namespace hk::boot {

struct ValidationResult {
    bool ok;
    const char* reason;
};

ValidationResult validate_boot_info(const hybrid::BootInfo* boot);
void retain_boot_info(const hybrid::BootInfo& boot);
const hybrid::FramebufferInfo& framebuffer_info();
const hybrid::BootInfo& retained_boot_info();

} // namespace hk::boot
