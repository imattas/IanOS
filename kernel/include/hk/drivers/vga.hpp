#pragma once
#include <stdint.h>
#include "hk/pci/pci.hpp"

namespace hk::drivers::vga {

struct Adapter {
    bool present;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint64_t mmio_base;
    uint64_t mmio_size;
    uint16_t required_command_bits;
};

class VgaDriver {
public:
    void probe(const hk::pci::PciRegistry& pci);
    const Adapter& adapter() const { return adapter_; }
    bool present() const { return adapter_.present; }
private:
    Adapter adapter_{};
};

VgaDriver& driver();
bool self_test();

} // namespace hk::drivers::vga
