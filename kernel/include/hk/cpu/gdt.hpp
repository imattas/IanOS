#pragma once
#include <stdint.h>
namespace hk::cpu {
constexpr uint16_t kKernelCodeSelector = 0x08;
constexpr uint16_t kKernelDataSelector = 0x10;
constexpr uint16_t kUserDataSelector = 0x1b;
constexpr uint16_t kUserCodeSelector = 0x23;
constexpr uint16_t kTssSelector = 0x28;
void initialize_gdt();
bool prepare_cpu_gdt(uint32_t cpu_id, uint64_t rsp0);
bool load_prepared_cpu_gdt(uint32_t cpu_id);
void set_tss_rsp0(uint64_t rsp0);
}
