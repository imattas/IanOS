#pragma once
#include <stdint.h>

namespace hk::timer {

void initialize_pit(uint32_t frequency_hz);
bool initialize_lapic_timer(uint32_t initial_count);
void stop_lapic_timer();
bool start_lapic_system_tick(uint32_t initial_count);
uint64_t ticks();
uint32_t frequency();
bool preemption_enabled();
void set_preemption_enabled(bool enabled);
bool user_preemption_enabled();
void set_user_preemption_enabled(bool enabled);
void sleep_for_ticks(uint64_t ticks);
bool lapic_timer_active();
uint64_t lapic_ticks();

} // namespace hk::timer
