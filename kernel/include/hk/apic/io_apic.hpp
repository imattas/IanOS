#pragma once
#include <stdint.h>

namespace hk::apic {

struct IoApicRoute {
    uint8_t vector;
    uint8_t apic_id;
    bool masked;
    bool active_low;
    bool level_triggered;
};

class IoApic {
public:
    void initialize(uint32_t mmio_base, uint32_t gsi_base);
    bool enabled() const { return enabled_; }
    uint32_t id() const;
    uint32_t version() const;
    uint32_t redirection_entries() const { return redirection_entries_; }
    uint32_t gsi_base() const { return gsi_base_; }
    bool handles_gsi(uint32_t gsi) const;
    bool set_redirection(uint32_t gsi, const IoApicRoute& route);
    uint64_t redirection(uint32_t gsi) const;
    bool mask_gsi(uint32_t gsi);
    void mask_all();
private:
    bool enabled_ = false;
    volatile uint32_t* base_ = nullptr;
    uint32_t gsi_base_ = 0;
    uint32_t redirection_entries_ = 0;
    uint32_t read(uint8_t reg) const;
    void write(uint8_t reg, uint32_t value);
};

IoApic& io_apic();

} // namespace hk::apic
