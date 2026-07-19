#pragma once
#include <stdint.h>
namespace hk::apic {
class LocalApic {
public:
    void initialize(uint32_t mmio_base);
    bool enable_current_cpu();
    bool available() const { return available_; }
    bool enabled() const { return enabled_; }
    uint32_t id() const;
    uint32_t version() const;
    bool configure_timer(uint8_t vector, uint32_t initial_count, uint8_t divide_config, bool periodic, bool masked);
    uint32_t timer_lvt() const;
    uint32_t timer_initial_count() const;
    uint32_t timer_current_count() const;
    uint32_t timer_divide_config() const;
    bool probe_timer_countdown(uint32_t initial_count, uint32_t spin_iterations);
    uint32_t timer_probe_initial() const { return timer_probe_initial_; }
    uint32_t timer_probe_current() const { return timer_probe_current_; }
    uint32_t timer_probe_delta() const { return timer_probe_delta_; }
    bool send_init_ipi(uint8_t apic_id);
    bool send_startup_ipi(uint8_t apic_id, uint8_t vector);
    bool send_fixed_ipi(uint8_t apic_id, uint8_t vector);
    void eoi();
private:
    bool available_ = false;
    bool enabled_ = false;
    volatile uint32_t* base_ = nullptr;
    uint32_t timer_probe_initial_ = 0;
    uint32_t timer_probe_current_ = 0;
    uint32_t timer_probe_delta_ = 0;
    uint32_t read(uint32_t reg) const;
    void write(uint32_t reg, uint32_t value);
};
LocalApic& local_apic();
}
