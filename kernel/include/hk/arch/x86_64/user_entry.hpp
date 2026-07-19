#pragma once
#include <stdint.h>

namespace hk::arch::x86_64 {
[[noreturn]] void enter_user(uint64_t rip, uint64_t rsp, uint64_t cr3, uint64_t rflags, uint16_t cs, uint16_t ss);
}
