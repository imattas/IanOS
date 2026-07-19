#include "hk/drivers/driver_manager.hpp"
#include "hk/log.hpp"

namespace hk::drivers {
DriverManager& driver_manager() {
    static DriverManager manager;
    return manager;
}

bool DriverManager::register_driver(const char* name, bool (*start)()) {
    ++stats_.register_attempts;
    if (count_ >= 32 || start == nullptr || name == nullptr) {
        ++stats_.failed_registrations;
        return false;
    }
    drivers_[count_++] = Driver{name, start, DriverState::Registered};
    ++stats_.registered_drivers;
    return true;
}

void DriverManager::start_all() {
    for (size_t i = 0; i < count_; ++i) {
        ++stats_.start_attempts;
        drivers_[i].state = drivers_[i].start() ? DriverState::Started : DriverState::Failed;
        if (drivers_[i].state == DriverState::Started) ++stats_.start_successes;
        else ++stats_.start_failures;
    }
}

void DriverManager::import_pci_bindings(const hk::pci::PciRegistry& pci) {
    device_count_ = 0;
    ++stats_.import_passes;
    stats_.imported_devices = 0;
    stats_.skipped_bindings = 0;
    stats_.ahci_devices = 0;
    stats_.e1000_devices = 0;
    stats_.vga_devices = 0;
    stats_.bus_master_required_devices = 0;
    stats_.command_bits_union = 0;
    const auto* bindings = pci.driver_bindings();
    for (uint32_t i = 0; i < pci.driver_binding_count() && device_count_ < 64; ++i) {
        const auto& binding = bindings[i];
        const auto* pci_device = pci.binding_device(binding);
        if (!pci_device || binding.name == nullptr || binding.kind == hk::pci::DriverKind::Unknown || binding.kind == hk::pci::DriverKind::Bridge) {
            ++stats_.skipped_bindings;
            continue;
        }
        devices_[device_count_++] = DriverDevice{
            binding.name,
            binding.kind,
            pci_device->bus,
            pci_device->device,
            pci_device->function,
            pci_device->vendor_id,
            pci_device->device_id,
            binding.required_command_bits,
            binding.bus_master_required,
            DeviceState::Bound,
        };
        ++stats_.imported_devices;
        stats_.command_bits_union |= binding.required_command_bits;
        if (binding.bus_master_required) ++stats_.bus_master_required_devices;
        switch (binding.kind) {
        case hk::pci::DriverKind::Ahci: ++stats_.ahci_devices; break;
        case hk::pci::DriverKind::E1000: ++stats_.e1000_devices; break;
        case hk::pci::DriverKind::Vga: ++stats_.vga_devices; break;
        default: break;
        }
    }
    if (device_count_ < pci.driver_binding_count()) {
        stats_.skipped_bindings += pci.driver_binding_count() - device_count_ - stats_.skipped_bindings;
    }
    hk::log_hex(hk::LogLevel::Info, "Driver PCI devices bound", device_count_);
    hk::log_hex(hk::LogLevel::Info, "Driver registered count", stats_.registered_drivers);
    hk::log_hex(hk::LogLevel::Info, "Driver start attempts", stats_.start_attempts);
    hk::log_hex(hk::LogLevel::Info, "Driver start successes", stats_.start_successes);
    hk::log_hex(hk::LogLevel::Info, "Driver start failures", stats_.start_failures);
    hk::log_hex(hk::LogLevel::Info, "Driver import passes", stats_.import_passes);
    hk::log_hex(hk::LogLevel::Info, "Driver imported devices", stats_.imported_devices);
    hk::log_hex(hk::LogLevel::Info, "Driver skipped bindings", stats_.skipped_bindings);
    hk::log_hex(hk::LogLevel::Info, "Driver AHCI devices", stats_.ahci_devices);
    hk::log_hex(hk::LogLevel::Info, "Driver e1000 devices", stats_.e1000_devices);
    hk::log_hex(hk::LogLevel::Info, "Driver VGA devices", stats_.vga_devices);
    hk::log_hex(hk::LogLevel::Info, "Driver bus-master devices", stats_.bus_master_required_devices);
    hk::log_hex(hk::LogLevel::Info, "Driver command bits union", stats_.command_bits_union);
    for (size_t i = 0; i < device_count_ && i < 8; ++i) {
        const auto& device = devices_[i];
        hk::log_hex(hk::LogLevel::Info, "Driver PCI device kind", static_cast<uint64_t>(device.kind));
        hk::log_hex(hk::LogLevel::Info, "Driver PCI device bdf", (static_cast<uint64_t>(device.bus) << 16) | (static_cast<uint64_t>(device.device) << 8) | device.function);
        hk::log_hex(hk::LogLevel::Info, "Driver PCI device id", (static_cast<uint64_t>(device.vendor_id) << 16) | device.device_id);
        hk::log_hex(hk::LogLevel::Info, "Driver PCI device command bits", device.required_command_bits);
    }
}

size_t DriverManager::started_count() const {
    size_t total = 0;
    for (size_t i = 0; i < count_; ++i) if (drivers_[i].state == DriverState::Started) ++total;
    return total;
}

size_t DriverManager::failed_count() const {
    size_t total = 0;
    for (size_t i = 0; i < count_; ++i) if (drivers_[i].state == DriverState::Failed) ++total;
    return total;
}
}
