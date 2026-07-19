#include "hk/timer/pit.hpp"
#include "hk/apic/apic.hpp"
#include "hk/interrupts/irq.hpp"
#include "hk/log.hpp"
#include "hk/sched/scheduler.hpp"

namespace {
void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

volatile uint64_t pit_ticks = 0;
volatile uint64_t lapic_tick_count = 0;
uint32_t pit_frequency = 0;
volatile bool pit_preemption = false;
volatile bool user_preemption = false;
volatile bool lapic_active = false;
}

namespace hk::timer {

void initialize_pit(uint32_t frequency_hz) {
    if (frequency_hz == 0) frequency_hz = 100;
    uint32_t divisor = 1193182u / frequency_hz;
    if (divisor == 0) divisor = 1;
    if (divisor > 0xffff) divisor = 0xffff;
    pit_frequency = 1193182u / divisor;
    outb(0x43, 0x36);
    outb(0x40, static_cast<uint8_t>(divisor & 0xff));
    outb(0x40, static_cast<uint8_t>((divisor >> 8) & 0xff));
    hk::interrupts::set_irq_mask(0, false);
    hk::log_hex(hk::LogLevel::Info, "PIT frequency", pit_frequency);
}

bool initialize_lapic_timer(uint32_t initial_count) {
    if (initial_count == 0) initial_count = 0x100000;
    if (!hk::apic::local_apic().configure_timer(0x40, initial_count, 0x3, true, false)) {
        hk::log(hk::LogLevel::Warn, "Local APIC scheduler timer enable failed");
        return false;
    }
    lapic_active = true;
    hk::log(hk::LogLevel::Info, "Local APIC scheduler timer enabled");
    hk::log_hex(hk::LogLevel::Info, "Local APIC scheduler timer LVT", hk::apic::local_apic().timer_lvt());
    hk::log_hex(hk::LogLevel::Info, "Local APIC scheduler timer initial", hk::apic::local_apic().timer_initial_count());
    return true;
}

void stop_lapic_timer() {
    if (hk::apic::local_apic().enabled()) {
        hk::apic::local_apic().configure_timer(0x40, 0, 0x3, false, true);
    }
    lapic_active = false;
    hk::log(hk::LogLevel::Info, "Local APIC scheduler timer stopped");
}

bool start_lapic_system_tick(uint32_t initial_count) {
    if (initial_count == 0) initial_count = 0x400000;
    if (!hk::apic::local_apic().configure_timer(0x40, initial_count, 0x3, true, false)) {
        hk::log(hk::LogLevel::Warn, "Local APIC system tick enable failed");
        return false;
    }
    lapic_active = true;
    hk::log(hk::LogLevel::Info, "Local APIC system tick enabled");
    return true;
}

uint64_t ticks() { return pit_ticks + lapic_tick_count; }
uint32_t frequency() { return pit_frequency; }
bool preemption_enabled() { return pit_preemption; }
void set_preemption_enabled(bool enabled) { pit_preemption = enabled; }
bool user_preemption_enabled() { return user_preemption; }
void set_user_preemption_enabled(bool enabled) { user_preemption = enabled; }
void sleep_for_ticks(uint64_t delta) {
    hk::sched::sleep_current_until(ticks() + delta);
}
bool lapic_timer_active() { return lapic_active; }
uint64_t lapic_ticks() { return lapic_tick_count; }

} // namespace hk::timer

extern "C" void pit_note_tick() {
    pit_ticks = pit_ticks + 1;
}

extern "C" void lapic_note_tick() {
    lapic_tick_count = lapic_tick_count + 1;
}
