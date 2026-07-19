#pragma once
#include <stddef.h>
#include <stdint.h>
#include "hk/pci/pci.hpp"

namespace hk::drivers {
enum class DriverState { Registered, Started, Failed };
enum class DeviceState : uint8_t { Bound, Started, Failed };

struct Driver {
    const char* name;
    bool (*start)();
    DriverState state;
};

struct DriverDevice {
    const char* driver_name;
    hk::pci::DriverKind kind;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t required_command_bits;
    bool bus_master_required;
    DeviceState state;
};

struct DriverManagerStats {
    uint64_t register_attempts;
    uint64_t registered_drivers;
    uint64_t failed_registrations;
    uint64_t start_attempts;
    uint64_t start_successes;
    uint64_t start_failures;
    uint64_t import_passes;
    uint64_t imported_devices;
    uint64_t skipped_bindings;
    uint64_t ahci_devices;
    uint64_t e1000_devices;
    uint64_t vga_devices;
    uint64_t bus_master_required_devices;
    uint64_t command_bits_union;
};

class DriverManager {
public:
    bool register_driver(const char* name, bool (*start)());
    void start_all();
    void import_pci_bindings(const hk::pci::PciRegistry& pci);
    size_t count() const { return count_; }
    size_t device_count() const { return device_count_; }
    const DriverDevice* devices() const { return devices_; }
    size_t started_count() const;
    size_t failed_count() const;
    DriverManagerStats stats() const { return stats_; }
private:
    Driver drivers_[32]{};
    DriverDevice devices_[64]{};
    size_t count_ = 0;
    size_t device_count_ = 0;
    DriverManagerStats stats_{};
};
DriverManager& driver_manager();
}
