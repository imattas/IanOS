#include "hk/cpu/idt.hpp"

extern "C" void load_idt(void*);
extern "C" uint64_t exception_stub_table[];
extern "C" uint64_t irq_stub_table[];
extern "C" void syscall_interrupt_stub();
extern "C" void apic_timer_interrupt_stub();
extern "C" void smp_ipi_interrupt_stub();

namespace {
struct [[gnu::packed]] IdtEntry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
};

struct [[gnu::packed]] IdtPtr { uint16_t limit; uint64_t base; };
alignas(16) IdtEntry idt[256]{};

void set_gate(uint8_t vector, uint64_t handler, uint8_t attributes = 0x8e, uint8_t ist = 0) {
    idt[vector].offset_low = handler & 0xffff;
    idt[vector].selector = 0x08;
    idt[vector].ist = ist & 0x7;
    idt[vector].type_attr = attributes;
    idt[vector].offset_mid = (handler >> 16) & 0xffff;
    idt[vector].offset_high = (handler >> 32) & 0xffffffff;
    idt[vector].zero = 0;
}
}

namespace hk::cpu {
void initialize_idt() {
    for (uint8_t i = 0; i < 32; ++i) set_gate(i, exception_stub_table[i]);
    set_gate(8, exception_stub_table[8], 0x8e, 1);
    for (uint8_t i = 0; i < 16; ++i) set_gate(static_cast<uint8_t>(32 + i), irq_stub_table[i]);
    set_gate(0x40, reinterpret_cast<uint64_t>(apic_timer_interrupt_stub), 0x8e);
    set_gate(0x41, reinterpret_cast<uint64_t>(smp_ipi_interrupt_stub), 0x8e);
    set_gate(0x80, reinterpret_cast<uint64_t>(syscall_interrupt_stub), 0xee);
    IdtPtr ptr{static_cast<uint16_t>(sizeof(idt) - 1), reinterpret_cast<uint64_t>(idt)};
    load_idt(&ptr);
}
}
