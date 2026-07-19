#include "hk/apic/apic.hpp"
#include "hk/cpu/features.hpp"
#include "hk/mm/vmm.hpp"
#include "hk/log.hpp"

namespace hk::apic {
namespace {
constexpr uint64_t kLocalApicVirt = 0xffff800000100000ull;
constexpr uint32_t kRegId = 0x20;
constexpr uint32_t kRegVersion = 0x30;
constexpr uint32_t kRegEoi = 0xb0;
constexpr uint32_t kRegSpurious = 0xf0;
constexpr uint32_t kRegIcrLow = 0x300;
constexpr uint32_t kRegIcrHigh = 0x310;
constexpr uint32_t kRegLvtTimer = 0x320;
constexpr uint32_t kRegTimerInitialCount = 0x380;
constexpr uint32_t kRegTimerCurrentCount = 0x390;
constexpr uint32_t kRegTimerDivide = 0x3e0;
constexpr uint32_t kLvtMasked = 1u << 16;
constexpr uint32_t kLvtPeriodic = 1u << 17;
constexpr uint32_t kIcrDeliveryPending = 1u << 12;
constexpr uint32_t kIcrDeliveryModeInit = 5u << 8;
constexpr uint32_t kIcrDeliveryModeStartup = 6u << 8;
constexpr uint32_t kIcrLevelAssert = 1u << 14;
constexpr uint32_t kIcrTriggerLevel = 1u << 15;

void short_delay() {
    for (uint32_t i = 0; i < 10000; ++i) asm volatile("pause");
}
}

LocalApic& local_apic() {
    static LocalApic lapic;
    return lapic;
}

void LocalApic::initialize(uint32_t mmio_base) {
    available_ = hk::cpu::detect_features().apic;
    if (!available_ || mmio_base == 0) {
        hk::log(hk::LogLevel::Warn, "Local APIC unavailable or no MMIO base");
        return;
    }
    auto mapped = hk::mm::vmm().map_page(kLocalApicVirt, mmio_base, hk::mm::PageWrite | hk::mm::PageCacheDisable | hk::mm::PageGlobal);
    if (!mapped.ok) {
        hk::log(hk::LogLevel::Warn, "Local APIC MMIO map failed");
        return;
    }
    base_ = reinterpret_cast<volatile uint32_t*>(kLocalApicVirt);
    uint32_t spurious = read(kRegSpurious);
    write(kRegSpurious, spurious | 0x100 | 0xff);
    enabled_ = true;
    hk::log_hex(hk::LogLevel::Info, "Local APIC ID", id());
    hk::log_hex(hk::LogLevel::Info, "Local APIC version", version());
}

bool LocalApic::enable_current_cpu() {
    if (!base_) return false;
    uint32_t spurious = read(kRegSpurious);
    write(kRegSpurious, spurious | 0x100 | 0xff);
    enabled_ = true;
    return (read(kRegSpurious) & 0x100) != 0;
}

uint32_t LocalApic::read(uint32_t reg) const {
    return base_ ? base_[reg / 4] : 0;
}

void LocalApic::write(uint32_t reg, uint32_t value) {
    if (base_) base_[reg / 4] = value;
}

uint32_t LocalApic::id() const {
    return read(kRegId) >> 24;
}

uint32_t LocalApic::version() const {
    return read(kRegVersion) & 0xff;
}

bool LocalApic::configure_timer(uint8_t vector, uint32_t initial_count, uint8_t divide_config, bool periodic, bool masked) {
    if (!enabled_ || vector < 0x20) return false;
    uint32_t lvt = vector;
    if (periodic) lvt |= kLvtPeriodic;
    if (masked) lvt |= kLvtMasked;
    write(kRegTimerDivide, divide_config & 0xf);
    write(kRegLvtTimer, lvt);
    write(kRegTimerInitialCount, initial_count);
    return (read(kRegLvtTimer) & 0xff) == vector && read(kRegTimerInitialCount) == initial_count;
}

uint32_t LocalApic::timer_lvt() const {
    return read(kRegLvtTimer);
}

uint32_t LocalApic::timer_initial_count() const {
    return read(kRegTimerInitialCount);
}

uint32_t LocalApic::timer_current_count() const {
    return read(kRegTimerCurrentCount);
}

uint32_t LocalApic::timer_divide_config() const {
    return read(kRegTimerDivide);
}

bool LocalApic::probe_timer_countdown(uint32_t initial_count, uint32_t spin_iterations) {
    timer_probe_initial_ = 0;
    timer_probe_current_ = 0;
    timer_probe_delta_ = 0;
    if (!configure_timer(0x40, initial_count, 0x3, false, true)) return false;
    for (uint32_t i = 0; i < spin_iterations; ++i) {
        asm volatile("pause");
    }
    uint32_t current = timer_current_count();
    if (current >= initial_count) return false;
    timer_probe_initial_ = initial_count;
    timer_probe_current_ = current;
    timer_probe_delta_ = initial_count - current;
    return timer_probe_delta_ != 0;
}

bool LocalApic::send_init_ipi(uint8_t apic_id) {
    if (!enabled_) return false;
    for (uint32_t i = 0; i < 100000 && (read(kRegIcrLow) & kIcrDeliveryPending) != 0; ++i) asm volatile("pause");
    write(kRegIcrHigh, static_cast<uint32_t>(apic_id) << 24);
    write(kRegIcrLow, kIcrDeliveryModeInit | kIcrLevelAssert | kIcrTriggerLevel);
    for (uint32_t i = 0; i < 100000; ++i) {
        if ((read(kRegIcrLow) & kIcrDeliveryPending) == 0) {
            short_delay();
            write(kRegIcrHigh, static_cast<uint32_t>(apic_id) << 24);
            write(kRegIcrLow, kIcrDeliveryModeInit | kIcrTriggerLevel);
            for (uint32_t j = 0; j < 100000; ++j) {
                if ((read(kRegIcrLow) & kIcrDeliveryPending) == 0) {
                    short_delay();
                    return true;
                }
                asm volatile("pause");
            }
            return false;
        }
        asm volatile("pause");
    }
    return false;
}

bool LocalApic::send_startup_ipi(uint8_t apic_id, uint8_t vector) {
    if (!enabled_ || vector == 0) return false;
    for (uint32_t i = 0; i < 100000 && (read(kRegIcrLow) & kIcrDeliveryPending) != 0; ++i) asm volatile("pause");
    write(kRegIcrHigh, static_cast<uint32_t>(apic_id) << 24);
    write(kRegIcrLow, kIcrDeliveryModeStartup | vector);
    for (uint32_t i = 0; i < 100000; ++i) {
        if ((read(kRegIcrLow) & kIcrDeliveryPending) == 0) {
            short_delay();
            return true;
        }
        asm volatile("pause");
    }
    return false;
}

bool LocalApic::send_fixed_ipi(uint8_t apic_id, uint8_t vector) {
    if (!enabled_ || vector < 0x20) return false;
    for (uint32_t i = 0; i < 100000 && (read(kRegIcrLow) & kIcrDeliveryPending) != 0; ++i) asm volatile("pause");
    write(kRegIcrHigh, static_cast<uint32_t>(apic_id) << 24);
    write(kRegIcrLow, vector);
    for (uint32_t i = 0; i < 100000; ++i) {
        if ((read(kRegIcrLow) & kIcrDeliveryPending) == 0) return true;
        asm volatile("pause");
    }
    return false;
}

void LocalApic::eoi() {
    write(kRegEoi, 0);
}
}
