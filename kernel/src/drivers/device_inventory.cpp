#include "hk/drivers/device_inventory.hpp"
#include "hk/drivers/ahci.hpp"
#include "hk/drivers/e1000.hpp"
#include "hk/drivers/vga.hpp"
#include "hk/log.hpp"

namespace hk::drivers {
namespace {
hybrid::DeviceClass to_public(DeviceClass value) {
    switch (value) {
    case DeviceClass::Storage: return hybrid::DeviceClass::Storage;
    case DeviceClass::Network: return hybrid::DeviceClass::Network;
    case DeviceClass::Display: return hybrid::DeviceClass::Display;
    case DeviceClass::Unknown: break;
    }
    return hybrid::DeviceClass::Unknown;
}

hybrid::DeviceResourceType to_public(ResourceType value) {
    switch (value) {
    case ResourceType::Mmio: return hybrid::DeviceResourceType::Mmio;
    case ResourceType::Io: return hybrid::DeviceResourceType::Io;
    case ResourceType::None: break;
    }
    return hybrid::DeviceResourceType::None;
}

DeviceClass from_public(hybrid::DeviceClass value) {
    switch (value) {
    case hybrid::DeviceClass::Storage: return DeviceClass::Storage;
    case hybrid::DeviceClass::Network: return DeviceClass::Network;
    case hybrid::DeviceClass::Display: return DeviceClass::Display;
    case hybrid::DeviceClass::Unknown: break;
    }
    return DeviceClass::Unknown;
}
}

DeviceInventory& inventory() {
    static DeviceInventory instance;
    return instance;
}

bool DeviceInventory::add(const DeviceInventoryEntry& entry) {
    if (count_ >= 16 || entry.driver_name == nullptr || entry.resource_count == 0) return false;
    entries_[count_++] = entry;
    return true;
}

void DeviceInventory::rebuild() {
    count_ = 0;
    if (ahci::driver().present()) {
        const auto& c = ahci::driver().controller();
        DeviceInventoryEntry entry{
            DeviceClass::Storage,
            "ahci",
            c.bus,
            c.device,
            c.function,
            c.vendor_id,
            c.device_id,
            c.required_command_bits,
            {{ResourceType::Mmio, c.abar, c.abar_size}, {}, {}},
            1,
        };
        add(entry);
    }
    if (e1000::driver().present()) {
        const auto& a = e1000::driver().adapter();
        DeviceInventoryEntry entry{
            DeviceClass::Network,
            "e1000",
            a.bus,
            a.device,
            a.function,
            a.vendor_id,
            a.device_id,
            a.required_command_bits,
            {},
            0,
        };
        if (a.mmio_base != 0 && a.mmio_size != 0 && entry.resource_count < 3) {
            entry.resources[entry.resource_count++] = DeviceResource{ResourceType::Mmio, a.mmio_base, a.mmio_size};
        }
        if (a.io_base != 0 && a.io_size != 0 && entry.resource_count < 3) {
            entry.resources[entry.resource_count++] = DeviceResource{ResourceType::Io, a.io_base, a.io_size};
        }
        add(entry);
    }
    if (vga::driver().present()) {
        const auto& a = vga::driver().adapter();
        DeviceInventoryEntry entry{
            DeviceClass::Display,
            "vga",
            a.bus,
            a.device,
            a.function,
            a.vendor_id,
            a.device_id,
            a.required_command_bits,
            {{ResourceType::Mmio, a.mmio_base, a.mmio_size}, {}, {}},
            1,
        };
        add(entry);
    }

    hk::log_hex(hk::LogLevel::Info, "Device inventory count", count_);
    hk::log_hex(hk::LogLevel::Info, "Device inventory storage", storage_count());
    hk::log_hex(hk::LogLevel::Info, "Device inventory network", network_count());
    hk::log_hex(hk::LogLevel::Info, "Device inventory display", display_count());
    hk::log_hex(hk::LogLevel::Info, "Device inventory resources", resource_count());
    for (uint32_t i = 0; i < count_; ++i) {
        const auto& entry = entries_[i];
        hk::log_hex(hk::LogLevel::Info, "Device inventory class", static_cast<uint64_t>(entry.device_class));
        hk::log_hex(hk::LogLevel::Info, "Device inventory bdf", (static_cast<uint64_t>(entry.bus) << 16) | (static_cast<uint64_t>(entry.device) << 8) | entry.function);
        hk::log_hex(hk::LogLevel::Info, "Device inventory id", (static_cast<uint64_t>(entry.vendor_id) << 16) | entry.device_id);
        hk::log_hex(hk::LogLevel::Info, "Device inventory command bits", entry.required_command_bits);
        hk::log_hex(hk::LogLevel::Info, "Device inventory resource count", entry.resource_count);
    }
    hybrid::DeviceInfo info{};
    if (copy_info(0, info)) {
        hk::log_hex(hk::LogLevel::Info, "Device info class", static_cast<uint64_t>(info.device_class));
        hk::log_hex(hk::LogLevel::Info, "Device info id", (static_cast<uint64_t>(info.vendor_id) << 16) | info.device_id);
        hk::log_hex(hk::LogLevel::Info, "Device info first resource", info.resources[0].base);
    }
    hybrid::DeviceInfo storage{};
    hybrid::DeviceInfo network{};
    hybrid::DeviceInfo display{};
    if (copy_info_by_class(hybrid::DeviceClass::Storage, 0, storage)) {
        hk::log_hex(hk::LogLevel::Info, "Device info storage resource", storage.resources[0].base);
    }
    if (copy_info_by_class(hybrid::DeviceClass::Network, 0, network)) {
        hk::log_hex(hk::LogLevel::Info, "Device info network resource", network.resources[0].base);
    }
    if (copy_info_by_class(hybrid::DeviceClass::Display, 0, display)) {
        hk::log_hex(hk::LogLevel::Info, "Device info display resource", display.resources[0].base);
    }
}

uint32_t DeviceInventory::storage_count() const {
    uint32_t total = 0;
    for (uint32_t i = 0; i < count_; ++i) if (entries_[i].device_class == DeviceClass::Storage) ++total;
    return total;
}

uint32_t DeviceInventory::network_count() const {
    uint32_t total = 0;
    for (uint32_t i = 0; i < count_; ++i) if (entries_[i].device_class == DeviceClass::Network) ++total;
    return total;
}

uint32_t DeviceInventory::display_count() const {
    uint32_t total = 0;
    for (uint32_t i = 0; i < count_; ++i) if (entries_[i].device_class == DeviceClass::Display) ++total;
    return total;
}

uint32_t DeviceInventory::resource_count() const {
    uint32_t total = 0;
    for (uint32_t i = 0; i < count_; ++i) total += entries_[i].resource_count;
    return total;
}

bool DeviceInventory::copy_info(uint32_t index, hybrid::DeviceInfo& out) const {
    if (index >= count_) return false;
    const auto& entry = entries_[index];
    out = {};
    out.device_class = to_public(entry.device_class);
    out.bus = entry.bus;
    out.device = entry.device;
    out.function = entry.function;
    out.resource_count = entry.resource_count;
    out.vendor_id = entry.vendor_id;
    out.device_id = entry.device_id;
    out.required_command_bits = entry.required_command_bits;
    for (uint8_t i = 0; i < entry.resource_count && i < 3; ++i) {
        out.resources[i] = hybrid::DeviceResourceInfo{
            to_public(entry.resources[i].type),
            0,
            entry.resources[i].base,
            entry.resources[i].size,
        };
    }
    return true;
}

bool DeviceInventory::copy_info_by_class(hybrid::DeviceClass device_class, uint32_t ordinal, hybrid::DeviceInfo& out) const {
    DeviceClass internal_class = from_public(device_class);
    if (internal_class == DeviceClass::Unknown) return false;
    uint32_t seen = 0;
    for (uint32_t i = 0; i < count_; ++i) {
        if (entries_[i].device_class != internal_class) continue;
        if (seen == ordinal) return copy_info(i, out);
        ++seen;
    }
    return false;
}

bool inventory_self_test() {
    const auto& inv = inventory();
    if (inv.count() < 3 || inv.storage_count() == 0 || inv.network_count() == 0 || inv.display_count() == 0) return false;
    if (inv.resource_count() < 3) return false;
    const auto* entries = inv.entries();
    for (uint32_t i = 0; i < inv.count(); ++i) {
        const auto& entry = entries[i];
        if (entry.device_class == DeviceClass::Unknown || entry.driver_name == nullptr || entry.resource_count == 0) return false;
        if (entry.required_command_bits == 0) return false;
        for (uint8_t r = 0; r < entry.resource_count; ++r) {
            if (entry.resources[r].type == ResourceType::None || entry.resources[r].base == 0 || entry.resources[r].size == 0) return false;
        }
        hybrid::DeviceInfo info{};
        if (!inv.copy_info(i, info)) return false;
        if (info.device_class == hybrid::DeviceClass::Unknown || info.resource_count != entry.resource_count) return false;
        if (info.vendor_id != entry.vendor_id || info.device_id != entry.device_id) return false;
        hybrid::DeviceInfo by_class{};
        if (!inv.copy_info_by_class(info.device_class, 0, by_class)) return false;
        if (by_class.device_class != info.device_class || by_class.resource_count == 0) return false;
    }
    hybrid::DeviceInfo missing{};
    if (inv.copy_info(inv.count(), missing)) return false;
    if (inv.copy_info_by_class(hybrid::DeviceClass::Unknown, 0, missing)) return false;
    return true;
}

} // namespace hk::drivers
