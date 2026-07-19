#pragma once
#include <stdint.h>
#include "hk/pci/pci.hpp"

namespace hk::drivers::e1000 {

struct Adapter {
    bool present;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint64_t mmio_base;
    uint64_t mmio_size;
    uint64_t mmio_virtual;
    uint64_t io_base;
    uint64_t io_size;
    uint16_t required_command_bits;
    uint16_t command_after_enable;
    bool command_enabled;
    bool mmio_mapped;
    uint32_t register_read_count;
    uint32_t register_write_count;
    uint32_t ctrl;
    uint32_t status;
    bool link_up;
    bool full_duplex;
    uint32_t link_speed_mbps;
    uint32_t pci_bus_speed_mhz;
    uint32_t pci_bus_width_bits;
    uint32_t eecd;
    uint32_t ctrl_ext;
    uint32_t mdic;
    uint32_t icr;
    uint32_t ims;
    uint32_t imc;
    uint32_t mac_low;
    uint32_t mac_high;
    uint64_t mac_address;
    bool mac_valid;
    bool rings_allocated;
    bool rings_programmed;
    uint64_t rx_desc_phys;
    uint64_t tx_desc_phys;
    uint64_t rx_buffers[16];
    uint64_t tx_buffers[8];
    uint32_t rx_desc_count;
    uint32_t tx_desc_count;
    uint32_t rx_buffer_count;
    uint32_t tx_buffer_count;
    uint32_t rctl;
    uint32_t tctl;
    uint32_t tipg;
    uint32_t rdbal;
    uint32_t rdbah;
    uint32_t rdlen;
    uint32_t tdbal;
    uint32_t tdbah;
    uint32_t tdlen;
    bool ring_registers_verified;
    uint32_t rdh;
    uint32_t rdt;
    uint32_t tdh;
    uint32_t tdt;
    bool tx_smoke_attempted;
    bool tx_smoke_completed;
    uint64_t tx_smoke_buffer_phys;
    uint32_t tx_smoke_length;
    uint32_t tx_smoke_polls;
    uint32_t tx_smoke_status;
    uint32_t tx_packets_submitted;
    uint32_t tx_packets_completed;
    uint32_t tx_next_index;
    uint32_t tx_last_index;
    uint32_t tx_reclaim_checks;
    uint32_t tx_busy_failures;
    uint32_t tx_null_rejects;
    uint32_t tx_length_rejects;
    bool tx_validation_smoke_attempted;
    bool tx_validation_smoke_passed;
    bool interrupts_masked;
    bool interrupts_acked;
    bool rx_idle_polled;
    bool rx_ring_idle;
    uint32_t rx_idle_polls;
    uint32_t rx_ready_descriptors;
    uint32_t rx_first_status;
    uint32_t rx_first_length;
    uint32_t rx_next_index;
    uint32_t rx_last_index;
    uint32_t rx_poll_calls;
    uint32_t rx_empty_polls;
    uint32_t rx_packets_received;
    uint32_t rx_bytes_received;
    uint32_t rx_small_buffer_drops;
    bool rx_poll_smoke_attempted;
    bool rx_poll_smoke_empty;
};

class E1000Driver {
public:
    void probe(hk::pci::PciRegistry& pci);
    bool transmit_frame(const void* data, uint32_t length);
    bool poll_receive(void* out, uint32_t capacity, uint32_t& length);
    const Adapter& adapter() const { return adapter_; }
    bool present() const { return adapter_.present; }
private:
    Adapter adapter_{};
};

E1000Driver& driver();
bool self_test();

} // namespace hk::drivers::e1000
