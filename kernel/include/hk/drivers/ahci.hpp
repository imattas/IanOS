#pragma once
#include <stdint.h>
#include "hk/pci/pci.hpp"

namespace hk::drivers::ahci {

struct Controller {
    bool present;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint64_t abar;
    uint64_t abar_size;
    uint64_t hba_virtual;
    uint16_t required_command_bits;
    bool hba_mapped;
    uint32_t cap;
    uint32_t ghc;
    uint32_t version;
    uint32_t ports_implemented;
    uint32_t implemented_port_count;
    uint32_t active_port_count;
    uint32_t first_active_port;
    uint32_t first_active_signature;
    uint32_t first_active_ssts;
    uint32_t first_active_cmd;
    bool identify_attempted;
    bool identify_success;
    uint32_t identify_status;
    uint32_t identify_error;
    uint16_t identify_signature_word;
    uint16_t identify_capabilities;
    uint16_t identify_major_version;
    bool read_lba0_attempted;
    bool read_lba0_success;
    uint32_t read_lba0_status;
    uint32_t read_lba0_error;
    uint16_t read_lba0_boot_signature;
    uint32_t read_lba0_oem;
    uint64_t read_lba0_fstype;
    uint64_t read_lba0_buffer;
};

class AhciDriver {
public:
    void probe(const hk::pci::PciRegistry& pci);
    const Controller& controller() const { return controller_; }
    bool present() const { return controller_.present; }
    bool read_sector(uint64_t lba, void* out_512);
private:
    Controller controller_{};
};

AhciDriver& driver();
bool self_test();

} // namespace hk::drivers::ahci
