#include "hk/interrupts/irq.hpp"
#include "hk/acpi/acpi.hpp"
#include "hk/apic/apic.hpp"
#include "hk/apic/io_apic.hpp"

namespace {
void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

uint8_t inb(uint16_t port) {
    uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

constexpr uint16_t kPic1Command = 0x20;
constexpr uint16_t kPic1Data = 0x21;
constexpr uint16_t kPic2Command = 0xa0;
constexpr uint16_t kPic2Data = 0xa1;
hk::interrupts::InterruptStats interrupt_stats{};
uint64_t vector_counts[256]{};
}

namespace hk::interrupts {

void initialize_pic() {
    ++interrupt_stats.pic_remaps;
    uint8_t mask1 = inb(kPic1Data);
    uint8_t mask2 = inb(kPic2Data);
    outb(kPic1Command, 0x11);
    outb(kPic2Command, 0x11);
    outb(kPic1Data, 0x20);
    outb(kPic2Data, 0x28);
    outb(kPic1Data, 0x04);
    outb(kPic2Data, 0x02);
    outb(kPic1Data, 0x01);
    outb(kPic2Data, 0x01);
    outb(kPic1Data, mask1);
    outb(kPic2Data, mask2);
}

void set_irq_mask(uint8_t irq, bool masked) {
    ++interrupt_stats.mask_updates;
    uint16_t port = irq < 8 ? kPic1Data : kPic2Data;
    uint8_t bit = static_cast<uint8_t>(1u << (irq & 7));
    uint8_t value = inb(port);
    value = masked ? static_cast<uint8_t>(value | bit) : static_cast<uint8_t>(value & ~bit);
    outb(port, value);
}

void send_eoi(uint8_t irq) {
    ++interrupt_stats.legacy_eoi_count;
    interrupt_stats.last_irq = irq;
    if (irq >= 8) outb(kPic2Command, 0x20);
    outb(kPic1Command, 0x20);
}

void note_vector_dispatch(uint64_t vector) {
    ++interrupt_stats.dispatch_count;
    if (vector < 256) {
        ++vector_counts[vector];
        interrupt_stats.last_vector = static_cast<uint8_t>(vector);
    } else {
        ++interrupt_stats.invalid_vectors;
        interrupt_stats.last_vector = 0xff;
        return;
    }
    if (vector < 32) ++interrupt_stats.exception_dispatch_count;
    else if (vector < 48) {
        ++interrupt_stats.irq_dispatch_count;
        interrupt_stats.last_irq = static_cast<uint8_t>(vector - 32);
    } else if (vector == 0x40 || vector == 0x41) ++interrupt_stats.lapic_dispatch_count;
    else if (vector == 0x80) ++interrupt_stats.syscall_dispatch_count;
}

uint32_t legacy_irq_to_gsi(uint8_t irq) {
    const auto& platform = hk::acpi::platform();
    for (uint32_t i = 0; i < platform.override_count; ++i) {
        const auto& override = platform.overrides[i];
        if (override.bus == 0 && override.source == irq) return override.gsi;
    }
    return irq;
}

uint16_t legacy_irq_flags(uint8_t irq) {
    const auto& platform = hk::acpi::platform();
    for (uint32_t i = 0; i < platform.override_count; ++i) {
        const auto& override = platform.overrides[i];
        if (override.bus == 0 && override.source == irq) return override.flags;
    }
    return 0;
}

bool prepare_ioapic_route(uint8_t irq, uint8_t vector, bool masked) {
    ++interrupt_stats.ioapic_route_prepares;
    uint32_t gsi = legacy_irq_to_gsi(irq);
    auto& ioapic = hk::apic::io_apic();
    if (!ioapic.enabled() || !ioapic.handles_gsi(gsi)) return false;
    uint16_t flags = legacy_irq_flags(irq);
    bool active_low = (flags & 0x3) == 0x3;
    bool level_triggered = (flags & 0xc) == 0xc;
    return ioapic.set_redirection(gsi, hk::apic::IoApicRoute{vector, static_cast<uint8_t>(hk::apic::local_apic().id()), masked, active_low, level_triggered});
}

bool ioapic_route_matches(uint8_t irq, uint8_t vector, bool masked) {
    ++interrupt_stats.ioapic_route_match_checks;
    uint32_t gsi = legacy_irq_to_gsi(irq);
    auto& ioapic = hk::apic::io_apic();
    if (!ioapic.enabled() || !ioapic.handles_gsi(gsi)) return false;
    uint64_t route = ioapic.redirection(gsi);
    if ((route & 0xff) != vector) return false;
    if (((route & (1ull << 16)) != 0) != masked) return false;
    uint16_t flags = legacy_irq_flags(irq);
    bool expect_active_low = (flags & 0x3) == 0x3;
    bool expect_level = (flags & 0xc) == 0xc;
    if (((route & (1ull << 13)) != 0) != expect_active_low) return false;
    if (((route & (1ull << 15)) != 0) != expect_level) return false;
    ++interrupt_stats.ioapic_route_match_successes;
    return true;
}

InterruptStats stats() {
    return interrupt_stats;
}

uint64_t vector_dispatch_count(uint8_t vector) {
    return vector_counts[vector];
}

} // namespace hk::interrupts
