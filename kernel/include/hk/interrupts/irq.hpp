#pragma once
#include <stdint.h>

namespace hk::interrupts {

struct InterruptStats {
    uint64_t pic_remaps;
    uint64_t mask_updates;
    uint64_t legacy_eoi_count;
    uint64_t dispatch_count;
    uint64_t exception_dispatch_count;
    uint64_t irq_dispatch_count;
    uint64_t lapic_dispatch_count;
    uint64_t syscall_dispatch_count;
    uint64_t ioapic_route_prepares;
    uint64_t ioapic_route_match_checks;
    uint64_t ioapic_route_match_successes;
    uint64_t invalid_vectors;
    uint8_t last_vector;
    uint8_t last_irq;
};

void initialize_pic();
void set_irq_mask(uint8_t irq, bool masked);
void send_eoi(uint8_t irq);
void note_vector_dispatch(uint64_t vector);
uint32_t legacy_irq_to_gsi(uint8_t irq);
uint16_t legacy_irq_flags(uint8_t irq);
bool prepare_ioapic_route(uint8_t irq, uint8_t vector, bool masked);
bool ioapic_route_matches(uint8_t irq, uint8_t vector, bool masked);
InterruptStats stats();
uint64_t vector_dispatch_count(uint8_t vector);

} // namespace hk::interrupts
