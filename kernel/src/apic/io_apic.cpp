#include "hk/apic/io_apic.hpp"
#include "hk/mm/vmm.hpp"
#include "hk/log.hpp"

namespace hk::apic {
namespace {
constexpr uint64_t kIoApicVirt = 0xffff800000101000ull;
constexpr uint8_t kRegId = 0x00;
constexpr uint8_t kRegVersion = 0x01;
constexpr uint8_t kRegRedirectionBase = 0x10;
constexpr uint64_t kRedirectionMask = 1ull << 16;
constexpr uint64_t kRedirectionActiveLow = 1ull << 13;
constexpr uint64_t kRedirectionLevelTriggered = 1ull << 15;
}

IoApic& io_apic() {
    static IoApic controller;
    return controller;
}

void IoApic::initialize(uint32_t mmio_base, uint32_t gsi_base) {
    if (mmio_base == 0) {
        hk::log(hk::LogLevel::Warn, "IOAPIC MMIO base missing");
        return;
    }
    auto mapped = hk::mm::vmm().map_page(kIoApicVirt, mmio_base, hk::mm::PageWrite | hk::mm::PageCacheDisable | hk::mm::PageGlobal);
    if (!mapped.ok) {
        hk::log(hk::LogLevel::Warn, mapped.error);
        return;
    }
    base_ = reinterpret_cast<volatile uint32_t*>(kIoApicVirt);
    gsi_base_ = gsi_base;
    uint32_t ver = version();
    redirection_entries_ = ((ver >> 16) & 0xff) + 1;
    enabled_ = true;
    mask_all();
    hk::log_hex(hk::LogLevel::Info, "IOAPIC ID", id());
    hk::log_hex(hk::LogLevel::Info, "IOAPIC version", ver & 0xff);
    hk::log_hex(hk::LogLevel::Info, "IOAPIC redirection entries", redirection_entries_);
    hk::log_hex(hk::LogLevel::Info, "IOAPIC GSI base", gsi_base_);
}

uint32_t IoApic::read(uint8_t reg) const {
    if (!base_) return 0;
    base_[0] = reg;
    return base_[4];
}

void IoApic::write(uint8_t reg, uint32_t value) {
    if (!base_) return;
    base_[0] = reg;
    base_[4] = value;
}

uint32_t IoApic::id() const {
    return (read(kRegId) >> 24) & 0xf;
}

uint32_t IoApic::version() const {
    return read(kRegVersion);
}

bool IoApic::handles_gsi(uint32_t gsi) const {
    return enabled_ && gsi >= gsi_base_ && gsi < gsi_base_ + redirection_entries_;
}

uint64_t IoApic::redirection(uint32_t gsi) const {
    if (!handles_gsi(gsi)) return 0;
    uint8_t index = static_cast<uint8_t>(gsi - gsi_base_);
    uint32_t low = read(static_cast<uint8_t>(kRegRedirectionBase + index * 2));
    uint32_t high = read(static_cast<uint8_t>(kRegRedirectionBase + index * 2 + 1));
    return (static_cast<uint64_t>(high) << 32) | low;
}

bool IoApic::set_redirection(uint32_t gsi, const IoApicRoute& route) {
    if (!handles_gsi(gsi) || route.vector < 0x20) return false;
    uint8_t index = static_cast<uint8_t>(gsi - gsi_base_);
    uint64_t entry = route.vector;
    if (route.masked) entry |= kRedirectionMask;
    if (route.active_low) entry |= kRedirectionActiveLow;
    if (route.level_triggered) entry |= kRedirectionLevelTriggered;
    entry |= static_cast<uint64_t>(route.apic_id) << 56;
    write(static_cast<uint8_t>(kRegRedirectionBase + index * 2 + 1), static_cast<uint32_t>(entry >> 32));
    write(static_cast<uint8_t>(kRegRedirectionBase + index * 2), static_cast<uint32_t>(entry));
    return true;
}

bool IoApic::mask_gsi(uint32_t gsi) {
    if (!handles_gsi(gsi)) return false;
    uint64_t entry = redirection(gsi) | kRedirectionMask;
    uint8_t index = static_cast<uint8_t>(gsi - gsi_base_);
    write(static_cast<uint8_t>(kRegRedirectionBase + index * 2 + 1), static_cast<uint32_t>(entry >> 32));
    write(static_cast<uint8_t>(kRegRedirectionBase + index * 2), static_cast<uint32_t>(entry));
    return true;
}

void IoApic::mask_all() {
    if (!enabled_) return;
    for (uint32_t i = 0; i < redirection_entries_; ++i) {
        uint32_t gsi = gsi_base_ + i;
        uint64_t entry = redirection(gsi) | kRedirectionMask;
        write(static_cast<uint8_t>(kRegRedirectionBase + i * 2 + 1), static_cast<uint32_t>(entry >> 32));
        write(static_cast<uint8_t>(kRegRedirectionBase + i * 2), static_cast<uint32_t>(entry));
    }
}

} // namespace hk::apic
