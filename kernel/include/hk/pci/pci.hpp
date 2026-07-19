#pragma once
#include <stdint.h>

namespace hk::pci {

enum class BarType : uint8_t { None, Io, Mmio32, Mmio64 };
enum class DriverKind : uint8_t { Unknown, Bridge, Ahci, E1000, Vga, Smbus };

struct Bar {
    BarType type;
    uint64_t base;
    uint64_t size;
    bool prefetchable;
};

struct Device {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t header_type;
    Bar bars[6];
    uint8_t bar_count;
    uint16_t command;
    uint16_t status;
    DriverKind driver_kind;
};

struct DriverBinding {
    DriverKind kind;
    uint32_t device_index;
    const char* name;
    uint16_t required_command_bits;
    bool bus_master_required;
};

enum CommandBits : uint16_t {
    CommandIoSpace = 1u << 0,
    CommandMemorySpace = 1u << 1,
    CommandBusMaster = 1u << 2,
};

class PciRegistry {
public:
    void scan_bus0();
    void scan_all();
    uint32_t count() const { return count_; }
    const Device* devices() const { return devices_; }
    uint32_t scanned_buses() const { return scanned_buses_; }
    uint32_t storage_controllers() const { return storage_controllers_; }
    uint32_t network_controllers() const { return network_controllers_; }
    uint32_t display_controllers() const { return display_controllers_; }
    uint32_t bridge_devices() const { return bridge_devices_; }
    uint32_t mmio_bar_count() const { return mmio_bar_count_; }
    uint32_t io_bar_count() const { return io_bar_count_; }
    uint32_t driver_candidate_count() const { return driver_candidate_count_; }
    uint32_t ahci_candidates() const { return ahci_candidates_; }
    uint32_t e1000_candidates() const { return e1000_candidates_; }
    uint32_t vga_candidates() const { return vga_candidates_; }
    uint32_t driver_binding_count() const { return driver_binding_count_; }
    const DriverBinding* driver_bindings() const { return driver_bindings_; }
    uint32_t ahci_bindings() const { return ahci_bindings_; }
    uint32_t e1000_bindings() const { return e1000_bindings_; }
    uint32_t vga_bindings() const { return vga_bindings_; }
    uint32_t ecam_region_count() const { return ecam_region_count_; }
    uint32_t ecam_verified_devices() const { return ecam_verified_devices_; }
    uint32_t config_probe_count() const { return config_probe_count_; }
    uint32_t config_probe_failures() const { return config_probe_failures_; }
    uint32_t config_ecam_mismatches() const { return config_ecam_mismatches_; }
    uint32_t malformed_config_rejects() const { return malformed_config_rejects_; }
    uint32_t preferred_ecam_reads() const { return preferred_ecam_reads_; }
    uint32_t legacy_config_reads() const { return legacy_config_reads_; }
    uint32_t ecam_fallback_reads() const { return ecam_fallback_reads_; }
    uint32_t preferred_ecam_writes() const { return preferred_ecam_writes_; }
    uint32_t legacy_config_writes() const { return legacy_config_writes_; }
    uint32_t ecam_write_fallbacks() const { return ecam_write_fallbacks_; }
    uint32_t command_enable_attempts() const { return command_enable_attempts_; }
    uint32_t command_enable_successes() const { return command_enable_successes_; }
    bool ecam_available() const { return ecam_region_count_ > 0; }
    const Device* binding_device(const DriverBinding& binding) const;
    bool read_ecam_u32(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint32_t& value) const;
    uint16_t command_for(const Device& device) const;
    uint16_t status_for(const Device& device) const;
    uint16_t required_command_bits(const Device& device, bool bus_master) const;
    bool set_command_bits(const Device& device, uint16_t bits);
private:
    Device devices_[128]{};
    DriverBinding driver_bindings_[32]{};
    uint32_t count_ = 0;
    uint32_t driver_binding_count_ = 0;
    uint32_t scanned_buses_ = 0;
    uint32_t storage_controllers_ = 0;
    uint32_t network_controllers_ = 0;
    uint32_t display_controllers_ = 0;
    uint32_t bridge_devices_ = 0;
    uint32_t mmio_bar_count_ = 0;
    uint32_t io_bar_count_ = 0;
    uint32_t driver_candidate_count_ = 0;
    uint32_t ahci_candidates_ = 0;
    uint32_t e1000_candidates_ = 0;
    uint32_t vga_candidates_ = 0;
    uint32_t ahci_bindings_ = 0;
    uint32_t e1000_bindings_ = 0;
    uint32_t vga_bindings_ = 0;
    uint32_t ecam_region_count_ = 0;
    uint32_t ecam_verified_devices_ = 0;
    uint32_t config_probe_count_ = 0;
    uint32_t config_probe_failures_ = 0;
    uint32_t config_ecam_mismatches_ = 0;
    uint32_t malformed_config_rejects_ = 0;
    uint32_t preferred_ecam_reads_ = 0;
    uint32_t legacy_config_reads_ = 0;
    uint32_t ecam_fallback_reads_ = 0;
    uint32_t preferred_ecam_writes_ = 0;
    uint32_t legacy_config_writes_ = 0;
    uint32_t ecam_write_fallbacks_ = 0;
    uint32_t command_enable_attempts_ = 0;
    uint32_t command_enable_successes_ = 0;
    void add(Device device);
    void add_binding(uint32_t device_index);
    void build_driver_bindings();
    void verify_ecam_devices();
    void probe_config_space();
    void reset();
    void scan_bus(uint8_t bus);
    uint32_t read_config_u32_preferred(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
    uint16_t read_config_u16_preferred(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
    uint8_t read_config_u8_preferred(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
    void write_config_u32_preferred(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value);
};

PciRegistry& registry();

} // namespace hk::pci
