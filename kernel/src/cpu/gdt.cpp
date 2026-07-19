#include "hk/cpu/gdt.hpp"
#include <stdint.h>

namespace {

struct [[gnu::packed]] GdtPtr { uint16_t limit; uint64_t base; };
struct [[gnu::packed]] Tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t io_map_base;
};

constexpr uint32_t kMaxCpuGdts = 32;
constexpr uint64_t kKernelCodeDescriptor = 0x00af9a000000ffff;
constexpr uint64_t kKernelDataDescriptor = 0x00af92000000ffff;
constexpr uint64_t kUserDataDescriptor = 0x00aff2000000ffff;
constexpr uint64_t kUserCodeDescriptor = 0x00affa000000ffff;

struct CpuDescriptorState {
    Tss tss;
    alignas(16) uint8_t double_fault_stack[16 * 1024];
    alignas(16) uint8_t interrupt_stack[16 * 1024];
    alignas(8) uint64_t gdt[7];
};
alignas(16) CpuDescriptorState cpu_descriptors[kMaxCpuGdts]{};

void install_tss_descriptor(CpuDescriptorState& state) {
    uint64_t base = reinterpret_cast<uint64_t>(&state.tss);
    uint64_t limit = sizeof(Tss) - 1;
    state.gdt[5] = (limit & 0xffff)
        | ((base & 0xffffff) << 16)
        | (0x89ull << 40)
        | (((limit >> 16) & 0xf) << 48)
        | (((base >> 24) & 0xff) << 56);
    state.gdt[6] = base >> 32;
}

void prepare_state(CpuDescriptorState& state, uint64_t rsp0) {
    state = {};
    state.gdt[0] = 0;
    state.gdt[1] = kKernelCodeDescriptor;
    state.gdt[2] = kKernelDataDescriptor;
    state.gdt[3] = kUserDataDescriptor;
    state.gdt[4] = kUserCodeDescriptor;
    state.tss.rsp0 = rsp0;
    state.tss.ist1 = reinterpret_cast<uint64_t>(state.double_fault_stack + sizeof(state.double_fault_stack));
    state.tss.ist2 = reinterpret_cast<uint64_t>(state.interrupt_stack + sizeof(state.interrupt_stack));
    state.tss.io_map_base = sizeof(Tss);
    install_tss_descriptor(state);
}

void load_state(CpuDescriptorState& state) {
    GdtPtr ptr{static_cast<uint16_t>(sizeof(state.gdt) - 1), reinterpret_cast<uint64_t>(state.gdt)};
    asm volatile("lgdt %0\n\t"
                 "pushq $0x08\n\t"
                 "leaq 1f(%%rip), %%rax\n\t"
                 "pushq %%rax\n\t"
                 "lretq\n\t"
                 "1:\n\t"
                 "mov $0x10, %%ax\n\t"
                 "mov %%ax, %%ds\n\t"
                 "mov %%ax, %%es\n\t"
                 "mov %%ax, %%ss\n\t"
                 "mov %%ax, %%fs\n\t"
                 "mov %%ax, %%gs"
                 : : "m"(ptr) : "rax", "memory");
    uint16_t selector = hk::cpu::kTssSelector;
    asm volatile("ltr %0" : : "m"(selector) : "memory");
}

} // namespace

namespace hk::cpu {

void initialize_gdt() {
    uint64_t rsp;
    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    prepare_state(cpu_descriptors[0], rsp);
    load_state(cpu_descriptors[0]);
}

bool prepare_cpu_gdt(uint32_t cpu_id, uint64_t rsp0) {
    if (cpu_id >= kMaxCpuGdts) return false;
    prepare_state(cpu_descriptors[cpu_id], rsp0);
    return true;
}

bool load_prepared_cpu_gdt(uint32_t cpu_id) {
    if (cpu_id >= kMaxCpuGdts) return false;
    load_state(cpu_descriptors[cpu_id]);
    return true;
}

void set_tss_rsp0(uint64_t rsp0) {
    cpu_descriptors[0].tss.rsp0 = rsp0;
}

} // namespace hk::cpu
