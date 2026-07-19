#include "hk/drivers/vga.hpp"
#include "hk/log.hpp"

namespace hk::drivers::vga {

VgaDriver& driver() {
    static VgaDriver instance;
    return instance;
}

void VgaDriver::probe(const hk::pci::PciRegistry& pci) {
    adapter_ = {};
    const auto* bindings = pci.driver_bindings();
    for (uint32_t i = 0; i < pci.driver_binding_count(); ++i) {
        const auto& binding = bindings[i];
        if (binding.kind != hk::pci::DriverKind::Vga) continue;
        const auto* device = pci.binding_device(binding);
        if (!device) continue;
        uint64_t mmio_base = 0;
        uint64_t mmio_size = 0;
        for (uint8_t bar = 0; bar < device->bar_count; ++bar) {
            const auto& candidate = device->bars[bar];
            if (candidate.type == hk::pci::BarType::Mmio32 || candidate.type == hk::pci::BarType::Mmio64) {
                if (candidate.size > mmio_size) {
                    mmio_base = candidate.base;
                    mmio_size = candidate.size;
                }
            }
        }
        if (mmio_base == 0 || mmio_size == 0) continue;
        adapter_ = Adapter{
            true,
            device->bus,
            device->device,
            device->function,
            device->vendor_id,
            device->device_id,
            mmio_base,
            mmio_size,
            binding.required_command_bits,
        };
        break;
    }

    hk::log_hex(hk::LogLevel::Info, "VGA adapter present", adapter_.present ? 1 : 0);
    if (adapter_.present) {
        hk::log_hex(hk::LogLevel::Info, "VGA adapter bdf", (static_cast<uint64_t>(adapter_.bus) << 16) | (static_cast<uint64_t>(adapter_.device) << 8) | adapter_.function);
        hk::log_hex(hk::LogLevel::Info, "VGA adapter id", (static_cast<uint64_t>(adapter_.vendor_id) << 16) | adapter_.device_id);
        hk::log_hex(hk::LogLevel::Info, "VGA MMIO base", adapter_.mmio_base);
        hk::log_hex(hk::LogLevel::Info, "VGA MMIO size", adapter_.mmio_size);
        hk::log_hex(hk::LogLevel::Info, "VGA command requirements", adapter_.required_command_bits);
    }
}

bool self_test() {
    const auto& a = driver().adapter();
    if (!a.present) return false;
    if (a.mmio_base == 0 || a.mmio_size == 0) return false;
    if ((a.required_command_bits & hk::pci::CommandMemorySpace) == 0) return false;
    return true;
}

} // namespace hk::drivers::vga
