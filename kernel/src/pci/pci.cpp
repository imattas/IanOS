#include "hk/pci/pci.hpp"
#include "hk/log.hpp"
#include "hk/acpi/acpi.hpp"

namespace {

void outl(uint16_t port, uint32_t value) {
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

uint32_t inl(uint16_t port) {
    uint32_t value;
    asm volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

uint32_t read_legacy_config_u32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t address = 0x80000000u
        | (static_cast<uint32_t>(bus) << 16)
        | (static_cast<uint32_t>(device) << 11)
        | (static_cast<uint32_t>(function) << 8)
        | (offset & 0xfc);
    outl(0xcf8, address);
    return inl(0xcfc);
}

void write_legacy_config_u32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value) {
    uint32_t address = 0x80000000u
        | (static_cast<uint32_t>(bus) << 16)
        | (static_cast<uint32_t>(device) << 11)
        | (static_cast<uint32_t>(function) << 8)
        | (offset & 0xfc);
    outl(0xcf8, address);
    outl(0xcfc, value);
}

uint16_t low_u16_from_u32(uint32_t value, uint8_t offset) {
    return static_cast<uint16_t>((value >> ((offset & 2) * 8)) & 0xffff);
}

uint8_t low_u8_from_u32(uint32_t value, uint8_t offset) {
    return static_cast<uint8_t>((value >> ((offset & 3) * 8)) & 0xff);
}

uint64_t bar_size_from_mask32(uint32_t mask, bool io) {
    uint32_t masked = io ? (mask & ~0x3u) : (mask & ~0xfu);
    if (masked == 0) return 0;
    return static_cast<uint64_t>((~masked) + 1u);
}

uint64_t bar_size_from_mask64(uint64_t mask) {
    uint64_t masked = mask & ~0xfull;
    if (masked == 0) return 0;
    return (~masked) + 1ull;
}

const char* driver_name_for(hk::pci::DriverKind kind) {
    switch (kind) {
    case hk::pci::DriverKind::Ahci: return "ahci";
    case hk::pci::DriverKind::E1000: return "e1000";
    case hk::pci::DriverKind::Vga: return "vga";
    case hk::pci::DriverKind::Smbus: return "smbus";
    case hk::pci::DriverKind::Bridge: return "pci-bridge";
    case hk::pci::DriverKind::Unknown: break;
    }
    return "unknown";
}

bool driver_requires_bus_master(hk::pci::DriverKind kind) {
    return kind == hk::pci::DriverKind::Ahci || kind == hk::pci::DriverKind::E1000;
}

} // namespace

namespace hk::pci {

PciRegistry& registry() {
    static PciRegistry pci;
    return pci;
}

void PciRegistry::add(Device device) {
    if (device.class_code == 0x01 && device.subclass == 0x06 && device.prog_if == 0x01) {
        device.driver_kind = DriverKind::Ahci;
        ++ahci_candidates_;
    } else if (device.vendor_id == 0x8086 && (device.device_id == 0x100e || device.device_id == 0x10d3 || device.class_code == 0x02)) {
        device.driver_kind = DriverKind::E1000;
        ++e1000_candidates_;
    } else if (device.class_code == 0x03) {
        device.driver_kind = DriverKind::Vga;
        ++vga_candidates_;
    } else if (device.class_code == 0x0c && device.subclass == 0x05) {
        device.driver_kind = DriverKind::Smbus;
    } else if (device.class_code == 0x06) {
        device.driver_kind = DriverKind::Bridge;
    }
    if (device.driver_kind != DriverKind::Unknown) ++driver_candidate_count_;
    switch (device.class_code) {
    case 0x01: ++storage_controllers_; break;
    case 0x02: ++network_controllers_; break;
    case 0x03: ++display_controllers_; break;
    case 0x06: ++bridge_devices_; break;
    default: break;
    }
    if (count_ < 128) devices_[count_++] = device;
}

void PciRegistry::reset() {
    count_ = 0;
    driver_binding_count_ = 0;
    scanned_buses_ = 0;
    storage_controllers_ = 0;
    network_controllers_ = 0;
    display_controllers_ = 0;
    bridge_devices_ = 0;
    mmio_bar_count_ = 0;
    io_bar_count_ = 0;
    driver_candidate_count_ = 0;
    ahci_candidates_ = 0;
    e1000_candidates_ = 0;
    vga_candidates_ = 0;
    ahci_bindings_ = 0;
    e1000_bindings_ = 0;
    vga_bindings_ = 0;
    ecam_region_count_ = hk::acpi::platform().ecam_region_count;
    ecam_verified_devices_ = 0;
    config_probe_count_ = 0;
    config_probe_failures_ = 0;
    config_ecam_mismatches_ = 0;
    malformed_config_rejects_ = 0;
    preferred_ecam_reads_ = 0;
    legacy_config_reads_ = 0;
    ecam_fallback_reads_ = 0;
    preferred_ecam_writes_ = 0;
    legacy_config_writes_ = 0;
    ecam_write_fallbacks_ = 0;
    command_enable_attempts_ = 0;
    command_enable_successes_ = 0;
}

void PciRegistry::scan_bus(uint8_t bus) {
    ++scanned_buses_;
    for (uint8_t dev = 0; dev < 32; ++dev) {
        uint16_t vendor = read_config_u16_preferred(bus, dev, 0, 0x00);
        if (vendor == 0xffff) continue;
        uint8_t header = read_config_u8_preferred(bus, dev, 0, 0x0e);
        uint8_t functions = (header & 0x80) ? 8 : 1;
        for (uint8_t fn = 0; fn < functions; ++fn) {
            vendor = read_config_u16_preferred(bus, dev, fn, 0x00);
            if (vendor == 0xffff) continue;
            uint16_t device_id = read_config_u16_preferred(bus, dev, fn, 0x02);
            uint8_t prog_if = read_config_u8_preferred(bus, dev, fn, 0x09);
            uint8_t subclass = read_config_u8_preferred(bus, dev, fn, 0x0a);
            uint8_t class_code = read_config_u8_preferred(bus, dev, fn, 0x0b);
            uint8_t fn_header = read_config_u8_preferred(bus, dev, fn, 0x0e);
            uint16_t command = read_config_u16_preferred(bus, dev, fn, 0x04);
            uint16_t status = read_config_u16_preferred(bus, dev, fn, 0x06);
            Device device_record{bus, dev, fn, vendor, device_id, class_code, subclass, prog_if, static_cast<uint8_t>(fn_header & 0x7f), {}, 0, command, status, DriverKind::Unknown};
            if (device_record.header_type == 0) {
                for (uint8_t bar_index = 0; bar_index < 6; ++bar_index) {
                    uint8_t offset = static_cast<uint8_t>(0x10 + bar_index * 4);
                    uint32_t original = read_config_u32_preferred(bus, dev, fn, offset);
                    if (original == 0) continue;
                    write_config_u32_preferred(bus, dev, fn, offset, 0xffffffffu);
                    uint32_t mask = read_config_u32_preferred(bus, dev, fn, offset);
                    write_config_u32_preferred(bus, dev, fn, offset, original);
                    if (mask == 0 || mask == 0xffffffffu) continue;

                    Bar bar{};
                    if (original & 0x1) {
                        bar.type = BarType::Io;
                        bar.base = original & ~0x3ull;
                        bar.size = bar_size_from_mask32(mask, true);
                    } else {
                        uint8_t type_bits = static_cast<uint8_t>((original >> 1) & 0x3);
                        bar.prefetchable = (original & 0x8) != 0;
                        if (type_bits == 0x2 && bar_index + 1 < 6) {
                            uint32_t original_high = read_config_u32_preferred(bus, dev, fn, static_cast<uint8_t>(offset + 4));
                            write_config_u32_preferred(bus, dev, fn, static_cast<uint8_t>(offset + 4), 0xffffffffu);
                            uint32_t mask_high = read_config_u32_preferred(bus, dev, fn, static_cast<uint8_t>(offset + 4));
                            write_config_u32_preferred(bus, dev, fn, static_cast<uint8_t>(offset + 4), original_high);
                            bar.type = BarType::Mmio64;
                            bar.base = (static_cast<uint64_t>(original_high) << 32) | (original & ~0xfull);
                            uint64_t full_mask = (static_cast<uint64_t>(mask_high) << 32) | (mask & ~0xfull);
                            bar.size = bar_size_from_mask64(full_mask);
                            ++bar_index;
                        } else {
                            bar.type = BarType::Mmio32;
                            bar.base = original & ~0xfull;
                            bar.size = bar_size_from_mask32(mask, false);
                        }
                    }
                    if (bar.size != 0 && device_record.bar_count < 6) {
                        if (bar.type == BarType::Io) ++io_bar_count_;
                        else ++mmio_bar_count_;
                        device_record.bars[device_record.bar_count++] = bar;
                    }
                }
            }
            add(device_record);
        }
    }
}

uint16_t PciRegistry::command_for(const Device& device) const {
    uint32_t value = 0;
    if (!read_ecam_u32(device.bus, device.device, device.function, 0x04, value)) {
        value = read_legacy_config_u32(device.bus, device.device, device.function, 0x04);
    }
    return low_u16_from_u32(value, 0x04);
}

uint16_t PciRegistry::status_for(const Device& device) const {
    uint32_t value = 0;
    if (!read_ecam_u32(device.bus, device.device, device.function, 0x04, value)) {
        value = read_legacy_config_u32(device.bus, device.device, device.function, 0x04);
    }
    return low_u16_from_u32(value, 0x06);
}

uint16_t PciRegistry::required_command_bits(const Device& device, bool bus_master) const {
    uint16_t bits = 0;
    for (uint8_t i = 0; i < device.bar_count; ++i) {
        if (device.bars[i].type == BarType::Io) bits |= CommandIoSpace;
        if (device.bars[i].type == BarType::Mmio32 || device.bars[i].type == BarType::Mmio64) bits |= CommandMemorySpace;
    }
    if (bus_master) bits |= CommandBusMaster;
    return bits;
}

bool PciRegistry::set_command_bits(const Device& device, uint16_t bits) {
    ++command_enable_attempts_;
    uint16_t current = command_for(device);
    uint16_t next = static_cast<uint16_t>(current | bits);
    uint32_t raw = read_config_u32_preferred(device.bus, device.device, device.function, 0x04);
    raw = (raw & 0xffff0000u) | next;
    write_config_u32_preferred(device.bus, device.device, device.function, 0x04, raw);
    bool enabled = (command_for(device) & bits) == bits;
    if (enabled) ++command_enable_successes_;
    return enabled;
}

const Device* PciRegistry::binding_device(const DriverBinding& binding) const {
    if (binding.device_index >= count_) return nullptr;
    return &devices_[binding.device_index];
}

bool PciRegistry::read_ecam_u32(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint32_t& value) const {
    if (device >= 32 || function >= 8) return false;
    if ((offset & 3) != 0 || offset >= 4096) return false;
    const auto& platform = hk::acpi::platform();
    for (uint32_t i = 0; i < platform.ecam_region_count; ++i) {
        const auto& ecam = platform.ecam_regions[i];
        if (bus < ecam.start_bus || bus > ecam.end_bus) continue;
        uint64_t relative_bus = static_cast<uint64_t>(bus - ecam.start_bus);
        uint64_t address = ecam.base
            + (relative_bus << 20)
            + (static_cast<uint64_t>(device) << 15)
            + (static_cast<uint64_t>(function) << 12)
            + offset;
        value = *reinterpret_cast<volatile uint32_t*>(address);
        return true;
    }
    return false;
}

uint32_t PciRegistry::read_config_u32_preferred(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t value = 0xffffffffu;
    if (read_ecam_u32(bus, device, function, offset, value)) {
        ++preferred_ecam_reads_;
        return value;
    }
    if (ecam_available()) ++ecam_fallback_reads_;
    ++legacy_config_reads_;
    return read_legacy_config_u32(bus, device, function, offset);
}

uint16_t PciRegistry::read_config_u16_preferred(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    return low_u16_from_u32(read_config_u32_preferred(bus, device, function, static_cast<uint8_t>(offset & 0xfc)), offset);
}

uint8_t PciRegistry::read_config_u8_preferred(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    return low_u8_from_u32(read_config_u32_preferred(bus, device, function, static_cast<uint8_t>(offset & 0xfc)), offset);
}

void PciRegistry::write_config_u32_preferred(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value) {
    uint32_t before = 0;
    if (read_ecam_u32(bus, device, function, offset, before)) {
        const auto& platform = hk::acpi::platform();
        for (uint32_t i = 0; i < platform.ecam_region_count; ++i) {
            const auto& ecam = platform.ecam_regions[i];
            if (bus < ecam.start_bus || bus > ecam.end_bus) continue;
            uint64_t address = ecam.base
                + (static_cast<uint64_t>(bus - ecam.start_bus) << 20)
                + (static_cast<uint64_t>(device) << 15)
                + (static_cast<uint64_t>(function) << 12)
                + (offset & 0xfc);
            *reinterpret_cast<volatile uint32_t*>(address) = value;
            ++preferred_ecam_writes_;
            return;
        }
    }
    if (ecam_available()) ++ecam_write_fallbacks_;
    ++legacy_config_writes_;
    write_legacy_config_u32(bus, device, function, offset, value);
}

void PciRegistry::add_binding(uint32_t device_index) {
    if (device_index >= count_ || driver_binding_count_ >= 32) return;
    const Device& device = devices_[device_index];
    if (device.driver_kind == DriverKind::Unknown || device.driver_kind == DriverKind::Bridge) return;

    bool bus_master = driver_requires_bus_master(device.driver_kind);
    DriverBinding binding{
        device.driver_kind,
        device_index,
        driver_name_for(device.driver_kind),
        required_command_bits(device, bus_master),
        bus_master,
    };
    driver_bindings_[driver_binding_count_++] = binding;
    switch (device.driver_kind) {
    case DriverKind::Ahci: ++ahci_bindings_; break;
    case DriverKind::E1000: ++e1000_bindings_; break;
    case DriverKind::Vga: ++vga_bindings_; break;
    default: break;
    }
}

void PciRegistry::build_driver_bindings() {
    driver_binding_count_ = 0;
    ahci_bindings_ = 0;
    e1000_bindings_ = 0;
    vga_bindings_ = 0;
    for (uint32_t i = 0; i < count_; ++i) add_binding(i);
}

void PciRegistry::verify_ecam_devices() {
    ecam_verified_devices_ = 0;
    if (!ecam_available()) return;
    for (uint32_t i = 0; i < count_; ++i) {
        const auto& device = devices_[i];
        uint32_t id = 0xffffffffu;
        if (!read_ecam_u32(device.bus, device.device, device.function, 0x00, id)) continue;
        uint16_t vendor = static_cast<uint16_t>(id & 0xffff);
        uint16_t device_id = static_cast<uint16_t>((id >> 16) & 0xffff);
        if (vendor == device.vendor_id && device_id == device.device_id) ++ecam_verified_devices_;
    }
}

void PciRegistry::probe_config_space() {
    config_probe_count_ = 0;
    config_probe_failures_ = 0;
    config_ecam_mismatches_ = 0;
    malformed_config_rejects_ = 0;

    uint32_t ignored = 0;
    if (!read_ecam_u32(0, 0, 0, 1, ignored)) ++malformed_config_rejects_;
    if (!read_ecam_u32(0, 0, 0, 4096, ignored)) ++malformed_config_rejects_;
    if (!read_ecam_u32(0, 32, 0, 0, ignored)) ++malformed_config_rejects_;
    if (!read_ecam_u32(0, 0, 8, 0, ignored)) ++malformed_config_rejects_;

    constexpr uint8_t offsets[] = {0x00, 0x04, 0x08, 0x0c, 0x10, 0x3c};
    for (uint32_t i = 0; i < count_; ++i) {
        const auto& device = devices_[i];
        for (uint8_t offset : offsets) {
            uint32_t legacy = read_legacy_config_u32(device.bus, device.device, device.function, offset);
            ++config_probe_count_;
            if (offset == 0x00) {
                uint16_t vendor = static_cast<uint16_t>(legacy & 0xffff);
                uint16_t id = static_cast<uint16_t>((legacy >> 16) & 0xffff);
                if (vendor != device.vendor_id || id != device.device_id) ++config_probe_failures_;
            }
            uint32_t ecam = 0;
            if (read_ecam_u32(device.bus, device.device, device.function, offset, ecam) && ecam != legacy) {
                ++config_ecam_mismatches_;
            }
        }
    }
}

void PciRegistry::scan_bus0() {
    reset();
    scan_bus(0);
    hk::log_hex(hk::LogLevel::Info, "PCI bus0 devices", count_);
    for (uint32_t i = 0; i < count_ && i < 8; ++i) {
        const auto& d = devices_[i];
        hk::log_hex(hk::LogLevel::Info, "PCI device vendor", d.vendor_id);
        hk::log_hex(hk::LogLevel::Info, "PCI class", (static_cast<uint64_t>(d.class_code) << 16) | (static_cast<uint64_t>(d.subclass) << 8) | d.prog_if);
    }
}

void PciRegistry::scan_all() {
    reset();
    for (uint16_t bus = 0; bus < 256; ++bus) scan_bus(static_cast<uint8_t>(bus));
    build_driver_bindings();
    verify_ecam_devices();
    probe_config_space();
    hk::log_hex(hk::LogLevel::Info, "PCI scanned buses", scanned_buses_);
    hk::log_hex(hk::LogLevel::Info, "PCI total devices", count_);
    hk::log_hex(hk::LogLevel::Info, "PCI storage controllers", storage_controllers_);
    hk::log_hex(hk::LogLevel::Info, "PCI network controllers", network_controllers_);
    hk::log_hex(hk::LogLevel::Info, "PCI display controllers", display_controllers_);
    hk::log_hex(hk::LogLevel::Info, "PCI bridge devices", bridge_devices_);
    hk::log_hex(hk::LogLevel::Info, "PCI MMIO BARs", mmio_bar_count_);
    hk::log_hex(hk::LogLevel::Info, "PCI IO BARs", io_bar_count_);
    hk::log_hex(hk::LogLevel::Info, "PCI driver candidates", driver_candidate_count_);
    hk::log_hex(hk::LogLevel::Info, "PCI AHCI candidates", ahci_candidates_);
    hk::log_hex(hk::LogLevel::Info, "PCI e1000 candidates", e1000_candidates_);
    hk::log_hex(hk::LogLevel::Info, "PCI VGA candidates", vga_candidates_);
    hk::log_hex(hk::LogLevel::Info, "PCI driver bindings", driver_binding_count_);
    hk::log_hex(hk::LogLevel::Info, "PCI ECAM regions", ecam_region_count_);
    hk::log_hex(hk::LogLevel::Info, "PCI ECAM verified devices", ecam_verified_devices_);
    hk::log_hex(hk::LogLevel::Info, "PCI config probes", config_probe_count_);
    hk::log_hex(hk::LogLevel::Info, "PCI config probe failures", config_probe_failures_);
    hk::log_hex(hk::LogLevel::Info, "PCI config ECAM mismatches", config_ecam_mismatches_);
    hk::log_hex(hk::LogLevel::Info, "PCI malformed config rejects", malformed_config_rejects_);
    hk::log_hex(hk::LogLevel::Info, "PCI preferred ECAM reads", preferred_ecam_reads_);
    hk::log_hex(hk::LogLevel::Info, "PCI legacy config reads", legacy_config_reads_);
    hk::log_hex(hk::LogLevel::Info, "PCI ECAM fallback reads", ecam_fallback_reads_);
    hk::log_hex(hk::LogLevel::Info, "PCI preferred ECAM writes", preferred_ecam_writes_);
    hk::log_hex(hk::LogLevel::Info, "PCI legacy config writes", legacy_config_writes_);
    hk::log_hex(hk::LogLevel::Info, "PCI ECAM write fallbacks", ecam_write_fallbacks_);
    hk::log_hex(hk::LogLevel::Info, "PCI command enable attempts", command_enable_attempts_);
    hk::log_hex(hk::LogLevel::Info, "PCI command enable successes", command_enable_successes_);
    hk::log_hex(hk::LogLevel::Info, "PCI AHCI bindings", ahci_bindings_);
    hk::log_hex(hk::LogLevel::Info, "PCI e1000 bindings", e1000_bindings_);
    hk::log_hex(hk::LogLevel::Info, "PCI VGA bindings", vga_bindings_);
    for (uint32_t i = 0; i < driver_binding_count_ && i < 8; ++i) {
        const auto& binding = driver_bindings_[i];
        const Device* device = binding_device(binding);
        if (!device) continue;
        hk::log_hex(hk::LogLevel::Info, "PCI binding index", i);
        hk::log_hex(hk::LogLevel::Info, "PCI binding kind", static_cast<uint64_t>(binding.kind));
        hk::log_hex(hk::LogLevel::Info, "PCI binding bdf", (static_cast<uint64_t>(device->bus) << 16) | (static_cast<uint64_t>(device->device) << 8) | device->function);
        hk::log_hex(hk::LogLevel::Info, "PCI binding command bits", binding.required_command_bits);
    }
    for (uint32_t i = 0; i < count_ && i < 8; ++i) {
        const auto& d = devices_[i];
        hk::log_hex(hk::LogLevel::Info, "PCI device bdf", (static_cast<uint64_t>(d.bus) << 16) | (static_cast<uint64_t>(d.device) << 8) | d.function);
        hk::log_hex(hk::LogLevel::Info, "PCI device id", (static_cast<uint64_t>(d.vendor_id) << 16) | d.device_id);
        hk::log_hex(hk::LogLevel::Info, "PCI device class", (static_cast<uint64_t>(d.class_code) << 16) | (static_cast<uint64_t>(d.subclass) << 8) | d.prog_if);
        hk::log_hex(hk::LogLevel::Info, "PCI driver kind", static_cast<uint64_t>(d.driver_kind));
        hk::log_hex(hk::LogLevel::Info, "PCI command/status", (static_cast<uint64_t>(d.command) << 16) | d.status);
        if (d.bar_count > 0) {
            hk::log_hex(hk::LogLevel::Info, "PCI first BAR base", d.bars[0].base);
            hk::log_hex(hk::LogLevel::Info, "PCI first BAR size", d.bars[0].size);
            hk::log_hex(hk::LogLevel::Info, "PCI required command bits", required_command_bits(d, d.class_code == 0x01 || d.class_code == 0x02));
        }
    }
}

} // namespace hk::pci
