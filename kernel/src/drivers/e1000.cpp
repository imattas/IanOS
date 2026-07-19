#include "hk/drivers/e1000.hpp"
#include "hk/lib/string.hpp"
#include "hk/log.hpp"
#include "hk/mm/pmm.hpp"
#include "hk/mm/vmm.hpp"

namespace hk::drivers::e1000 {
namespace {
constexpr uint64_t kE1000MmioVirt = 0xffff800001040000ull;
constexpr uint64_t kPageSize = 4096;
constexpr uint64_t kProbeWindow = 0x6000;
constexpr uint32_t kRegCtrl = 0x0000;
constexpr uint32_t kRegStatus = 0x0008;
constexpr uint32_t kRegEecd = 0x0010;
constexpr uint32_t kRegCtrlExt = 0x0018;
constexpr uint32_t kRegMdic = 0x0020;
constexpr uint32_t kRegIcr = 0x00c0;
constexpr uint32_t kRegIms = 0x00d0;
constexpr uint32_t kRegImc = 0x00d8;
constexpr uint32_t kRegRctl = 0x0100;
constexpr uint32_t kRegTctl = 0x0400;
constexpr uint32_t kRegTipg = 0x0410;
constexpr uint32_t kRegRdbal = 0x2800;
constexpr uint32_t kRegRdbah = 0x2804;
constexpr uint32_t kRegRdlen = 0x2808;
constexpr uint32_t kRegRdh = 0x2810;
constexpr uint32_t kRegRdt = 0x2818;
constexpr uint32_t kRegTdbal = 0x3800;
constexpr uint32_t kRegTdbah = 0x3804;
constexpr uint32_t kRegTdlen = 0x3808;
constexpr uint32_t kRegTdh = 0x3810;
constexpr uint32_t kRegTdt = 0x3818;
constexpr uint32_t kRegRal0 = 0x5400;
constexpr uint32_t kRegRah0 = 0x5404;
constexpr uint32_t kRahAddressValid = 1u << 31;
constexpr uint32_t kRxDescCount = 16;
constexpr uint32_t kTxDescCount = 8;
constexpr uint32_t kMinEthernetFrame = 60;
constexpr uint32_t kMaxEthernetFrame = 1518;
constexpr uint32_t kRctlEnable = 1u << 1;
constexpr uint32_t kRctlBroadcastAccept = 1u << 15;
constexpr uint32_t kRctlStripEthernetCrc = 1u << 26;
constexpr uint32_t kTctlEnable = 1u << 1;
constexpr uint32_t kTctlPadShortPackets = 1u << 3;
constexpr uint8_t kTxCmdEop = 1u << 0;
constexpr uint8_t kTxCmdIfcs = 1u << 1;
constexpr uint8_t kTxCmdRs = 1u << 3;
constexpr uint8_t kTxStatusDd = 1u << 0;
constexpr uint8_t kRxStatusDd = 1u << 0;
constexpr uint32_t kStatusLinkUp = 1u << 1;
constexpr uint32_t kStatusFullDuplex = 1u << 0;
constexpr uint32_t kStatusSpeedMask = 3u << 6;
constexpr uint32_t kStatusBus64 = 1u << 12;
constexpr uint32_t kStatusBusSpeedMask = 3u << 8;

uint64_t align_down(uint64_t value) { return value & ~(kPageSize - 1); }
uint64_t align_up(uint64_t value) { return (value + kPageSize - 1) & ~(kPageSize - 1); }

uint32_t read_reg32(const Adapter& adapter, uint32_t offset) {
    return *reinterpret_cast<volatile uint32_t*>(adapter.mmio_virtual + offset);
}

void sample_register(Adapter& adapter, uint32_t offset, uint32_t& out) {
    out = read_reg32(adapter, offset);
    ++adapter.register_read_count;
}

void write_reg32(Adapter& adapter, uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(adapter.mmio_virtual + offset) = value;
    ++adapter.register_write_count;
}

void zero_page(uint64_t physical) {
    auto* bytes = reinterpret_cast<volatile uint8_t*>(physical);
    for (uint64_t i = 0; i < kPageSize; ++i) bytes[i] = 0;
}

void decode_status(Adapter& adapter) {
    adapter.link_up = (adapter.status & kStatusLinkUp) != 0;
    adapter.full_duplex = (adapter.status & kStatusFullDuplex) != 0;
    uint32_t speed = (adapter.status & kStatusSpeedMask) >> 6;
    switch (speed) {
    case 0: adapter.link_speed_mbps = 10; break;
    case 1: adapter.link_speed_mbps = 100; break;
    case 2: adapter.link_speed_mbps = 1000; break;
    default: adapter.link_speed_mbps = 0; break;
    }
    uint32_t bus_speed = (adapter.status & kStatusBusSpeedMask) >> 8;
    switch (bus_speed) {
    case 0: adapter.pci_bus_speed_mhz = 33; break;
    case 1: adapter.pci_bus_speed_mhz = 66; break;
    case 2: adapter.pci_bus_speed_mhz = 100; break;
    case 3: adapter.pci_bus_speed_mhz = 133; break;
    }
    adapter.pci_bus_width_bits = (adapter.status & kStatusBus64) != 0 ? 64 : 32;
}

bool allocate_rings(Adapter& adapter) {
    adapter.rx_desc_phys = hk::mm::pmm().allocate_page();
    adapter.tx_desc_phys = hk::mm::pmm().allocate_page();
    if (adapter.rx_desc_phys == 0 || adapter.tx_desc_phys == 0) return false;
    zero_page(adapter.rx_desc_phys);
    zero_page(adapter.tx_desc_phys);
    adapter.rx_desc_count = kRxDescCount;
    adapter.tx_desc_count = kTxDescCount;

    auto* rx_desc = reinterpret_cast<volatile uint64_t*>(adapter.rx_desc_phys);
    for (uint32_t i = 0; i < kRxDescCount; ++i) {
        uint64_t buffer = hk::mm::pmm().allocate_page();
        if (buffer == 0) return false;
        zero_page(buffer);
        adapter.rx_buffers[i] = buffer;
        rx_desc[i * 2] = buffer;
        rx_desc[i * 2 + 1] = 0;
        ++adapter.rx_buffer_count;
    }

    auto* tx_desc = reinterpret_cast<volatile uint64_t*>(adapter.tx_desc_phys);
    for (uint32_t i = 0; i < kTxDescCount; ++i) {
        uint64_t buffer = hk::mm::pmm().allocate_page();
        if (buffer == 0) return false;
        zero_page(buffer);
        adapter.tx_buffers[i] = buffer;
        tx_desc[i * 2] = buffer;
        tx_desc[i * 2 + 1] = static_cast<uint64_t>(kTxStatusDd) << 32;
        ++adapter.tx_buffer_count;
    }
    adapter.rings_allocated = true;
    return true;
}

bool program_rings(Adapter& adapter) {
    if (!adapter.mmio_mapped || !adapter.rings_allocated) return false;
    write_reg32(adapter, kRegRctl, 0);
    write_reg32(adapter, kRegTctl, 0);
    write_reg32(adapter, kRegRdbal, static_cast<uint32_t>(adapter.rx_desc_phys));
    write_reg32(adapter, kRegRdbah, static_cast<uint32_t>(adapter.rx_desc_phys >> 32));
    write_reg32(adapter, kRegRdlen, kRxDescCount * 16);
    write_reg32(adapter, kRegRdh, 0);
    write_reg32(adapter, kRegRdt, kRxDescCount - 1);
    write_reg32(adapter, kRegTdbal, static_cast<uint32_t>(adapter.tx_desc_phys));
    write_reg32(adapter, kRegTdbah, static_cast<uint32_t>(adapter.tx_desc_phys >> 32));
    write_reg32(adapter, kRegTdlen, kTxDescCount * 16);
    write_reg32(adapter, kRegTdh, 0);
    write_reg32(adapter, kRegTdt, 0);
    adapter.tipg = 10u | (8u << 10) | (6u << 20);
    write_reg32(adapter, kRegTipg, adapter.tipg);
    adapter.rctl = kRctlEnable | kRctlBroadcastAccept | kRctlStripEthernetCrc;
    adapter.tctl = kTctlEnable | kTctlPadShortPackets | (0x10u << 4) | (0x40u << 12);
    write_reg32(adapter, kRegRctl, adapter.rctl);
    write_reg32(adapter, kRegTctl, adapter.tctl);
    sample_register(adapter, kRegRctl, adapter.rctl);
    sample_register(adapter, kRegTctl, adapter.tctl);
    sample_register(adapter, kRegTipg, adapter.tipg);
    sample_register(adapter, kRegRdbal, adapter.rdbal);
    sample_register(adapter, kRegRdbah, adapter.rdbah);
    sample_register(adapter, kRegRdlen, adapter.rdlen);
    sample_register(adapter, kRegTdbal, adapter.tdbal);
    sample_register(adapter, kRegTdbah, adapter.tdbah);
    sample_register(adapter, kRegTdlen, adapter.tdlen);
    sample_register(adapter, kRegRdh, adapter.rdh);
    sample_register(adapter, kRegRdt, adapter.rdt);
    sample_register(adapter, kRegTdh, adapter.tdh);
    sample_register(adapter, kRegTdt, adapter.tdt);
    adapter.ring_registers_verified =
        adapter.rdbal == static_cast<uint32_t>(adapter.rx_desc_phys) &&
        adapter.rdbah == static_cast<uint32_t>(adapter.rx_desc_phys >> 32) &&
        adapter.rdlen == kRxDescCount * 16 &&
        adapter.tdbal == static_cast<uint32_t>(adapter.tx_desc_phys) &&
        adapter.tdbah == static_cast<uint32_t>(adapter.tx_desc_phys >> 32) &&
        adapter.tdlen == kTxDescCount * 16;
    adapter.rings_programmed = true;
    return true;
}

void mask_and_ack_interrupts(Adapter& adapter) {
    if (!adapter.mmio_mapped) return;
    write_reg32(adapter, kRegImc, 0xffffffffu);
    adapter.imc = 0xffffffffu;
    adapter.interrupts_masked = true;
    sample_register(adapter, kRegIcr, adapter.icr);
    sample_register(adapter, kRegIms, adapter.ims);
    adapter.interrupts_acked = true;
}

void write_tx_desc16(volatile uint8_t* desc, uint32_t offset, uint16_t value) {
    *reinterpret_cast<volatile uint16_t*>(desc + offset) = value;
}

void write_tx_desc8(volatile uint8_t* desc, uint32_t offset, uint8_t value) {
    desc[offset] = value;
}

bool submit_transmit(Adapter& adapter, const uint8_t* frame, uint32_t length, uint32_t& polls, uint32_t& status, uint64_t& buffer_phys, uint32_t& descriptor_index) {
    polls = 0;
    status = 0;
    buffer_phys = 0;
    descriptor_index = adapter.tx_next_index;
    if (!adapter.rings_programmed || adapter.tx_desc_phys == 0) return false;
    if (frame == nullptr) {
        ++adapter.tx_null_rejects;
        return false;
    }
    if (length < kMinEthernetFrame || length > kMaxEthernetFrame) {
        ++adapter.tx_length_rejects;
        return false;
    }
    if (adapter.tx_desc_count == 0) return false;
    uint32_t index = adapter.tx_next_index % adapter.tx_desc_count;
    auto* desc = reinterpret_cast<volatile uint8_t*>(adapter.tx_desc_phys + index * 16);
    ++adapter.tx_reclaim_checks;
    status = desc[12];
    if ((status & kTxStatusDd) == 0) {
        ++adapter.tx_busy_failures;
        return false;
    }
    uint64_t buffer = adapter.tx_buffers[index];
    if (buffer == 0) return false;
    zero_page(buffer);
    memcpy(reinterpret_cast<void*>(buffer), frame, length);
    buffer_phys = buffer;
    descriptor_index = index;

    write_tx_desc16(desc, 8, static_cast<uint16_t>(length));
    write_tx_desc8(desc, 10, 0);
    write_tx_desc8(desc, 11, kTxCmdEop | kTxCmdIfcs | kTxCmdRs);
    write_tx_desc8(desc, 12, 0);
    write_tx_desc8(desc, 13, 0);
    write_tx_desc16(desc, 14, 0);
    ++adapter.tx_packets_submitted;
    uint32_t next = (index + 1) % adapter.tx_desc_count;
    write_reg32(adapter, kRegTdt, next);
    adapter.tdt = next;

    for (uint32_t i = 0; i < 100000; ++i) {
        polls = i + 1;
        status = desc[12];
        if ((status & kTxStatusDd) != 0) {
            ++adapter.tx_packets_completed;
            adapter.tx_last_index = index;
            adapter.tx_next_index = next;
            sample_register(adapter, kRegTdh, adapter.tdh);
            sample_register(adapter, kRegTdt, adapter.tdt);
            return true;
        }
        asm volatile("pause");
    }
    sample_register(adapter, kRegTdh, adapter.tdh);
    sample_register(adapter, kRegTdt, adapter.tdt);
    return false;
}

bool transmit_smoke(Adapter& adapter) {
    adapter.tx_smoke_attempted = true;
    if (!adapter.rings_programmed || adapter.tx_desc_phys == 0 || !adapter.mac_valid) return false;
    uint8_t frame[kMinEthernetFrame]{};
    for (uint32_t i = 0; i < 6; ++i) frame[i] = 0xff;
    for (uint32_t i = 0; i < 6; ++i) frame[6 + i] = static_cast<uint8_t>((adapter.mac_address >> (i * 8)) & 0xff);
    frame[12] = 0x88;
    frame[13] = 0xb5;
    const char payload[] = "IanOS e1000 tx smoke";
    for (uint32_t i = 0; i < sizeof(payload) - 1; ++i) frame[14 + i] = static_cast<uint8_t>(payload[i]);
    adapter.tx_smoke_length = kMinEthernetFrame;
    uint32_t index = 0;
    adapter.tx_smoke_completed = submit_transmit(adapter, frame, kMinEthernetFrame, adapter.tx_smoke_polls, adapter.tx_smoke_status, adapter.tx_smoke_buffer_phys, index);
    return adapter.tx_smoke_completed;
}

void transmit_validation_smoke(Adapter& adapter) {
    adapter.tx_validation_smoke_attempted = true;
    uint8_t frame[kMinEthernetFrame]{};
    uint32_t polls = 0;
    uint32_t status = 0;
    uint64_t buffer = 0;
    uint32_t index = 0;
    uint32_t null_before = adapter.tx_null_rejects;
    uint32_t length_before = adapter.tx_length_rejects;
    bool null_rejected = !submit_transmit(adapter, nullptr, kMinEthernetFrame, polls, status, buffer, index);
    bool small_rejected = !submit_transmit(adapter, frame, kMinEthernetFrame - 1, polls, status, buffer, index);
    bool large_rejected = !submit_transmit(adapter, frame, kMaxEthernetFrame + 1, polls, status, buffer, index);
    adapter.tx_validation_smoke_passed =
        null_rejected &&
        small_rejected &&
        large_rejected &&
        adapter.tx_null_rejects == null_before + 1 &&
        adapter.tx_length_rejects == length_before + 2 &&
        adapter.tx_packets_submitted == 1 &&
        adapter.tx_next_index == 1;
}

bool rx_idle_poll(Adapter& adapter) {
    adapter.rx_idle_polled = true;
    if (!adapter.rings_programmed || adapter.rx_desc_phys == 0) return false;
    auto* desc = reinterpret_cast<volatile uint8_t*>(adapter.rx_desc_phys);
    for (uint32_t poll = 0; poll < 4; ++poll) {
        adapter.rx_idle_polls = poll + 1;
        uint32_t ready = 0;
        for (uint32_t i = 0; i < adapter.rx_desc_count; ++i) {
            volatile uint8_t* entry = desc + i * 16;
            uint8_t status = entry[12];
            if (i == 0) {
                adapter.rx_first_status = status;
                adapter.rx_first_length = *reinterpret_cast<volatile uint16_t*>(const_cast<uint8_t*>(entry + 8));
            }
            if ((status & kRxStatusDd) != 0) ++ready;
        }
        adapter.rx_ready_descriptors = ready;
        if (ready == 0) {
            adapter.rx_ring_idle = true;
            sample_register(adapter, kRegRdh, adapter.rdh);
            sample_register(adapter, kRegRdt, adapter.rdt);
            return true;
        }
        asm volatile("pause");
    }
    sample_register(adapter, kRegRdh, adapter.rdh);
    sample_register(adapter, kRegRdt, adapter.rdt);
    return false;
}

bool poll_receive_internal(Adapter& adapter, void* out, uint32_t capacity, uint32_t& length) {
    length = 0;
    ++adapter.rx_poll_calls;
    if (!adapter.rings_programmed || adapter.rx_desc_phys == 0 || adapter.rx_desc_count == 0 || out == nullptr) return false;
    uint32_t index = adapter.rx_next_index % adapter.rx_desc_count;
    auto* desc = reinterpret_cast<volatile uint8_t*>(adapter.rx_desc_phys + index * 16);
    uint8_t status = desc[12];
    if ((status & kRxStatusDd) == 0) {
        ++adapter.rx_empty_polls;
        return false;
    }
    uint16_t packet_length = *reinterpret_cast<volatile uint16_t*>(const_cast<uint8_t*>(desc + 8));
    length = packet_length;
    if (packet_length == 0) {
        desc[12] = 0;
        write_reg32(adapter, kRegRdt, index);
        adapter.rdt = index;
        adapter.rx_last_index = index;
        adapter.rx_next_index = (index + 1) % adapter.rx_desc_count;
        return false;
    }
    if (packet_length > capacity || packet_length > kPageSize) {
        ++adapter.rx_small_buffer_drops;
        return false;
    }
    uint64_t buffer = adapter.rx_buffers[index];
    if (buffer == 0) return false;
    memcpy(out, reinterpret_cast<const void*>(buffer), packet_length);
    desc[12] = 0;
    desc[13] = 0;
    *reinterpret_cast<volatile uint16_t*>(const_cast<uint8_t*>(desc + 8)) = 0;
    *reinterpret_cast<volatile uint16_t*>(const_cast<uint8_t*>(desc + 10)) = 0;
    write_reg32(adapter, kRegRdt, index);
    adapter.rdt = index;
    adapter.rx_last_index = index;
    adapter.rx_next_index = (index + 1) % adapter.rx_desc_count;
    ++adapter.rx_packets_received;
    adapter.rx_bytes_received += packet_length;
    return true;
}

void receive_poll_smoke(Adapter& adapter) {
    adapter.rx_poll_smoke_attempted = true;
    uint8_t scratch[64]{};
    uint32_t length = 0;
    adapter.rx_poll_smoke_empty = !poll_receive_internal(adapter, scratch, sizeof(scratch), length) && length == 0;
}
} // namespace

E1000Driver& driver() {
    static E1000Driver instance;
    return instance;
}

void E1000Driver::probe(hk::pci::PciRegistry& pci) {
    adapter_ = {};
    const auto* bindings = pci.driver_bindings();
    for (uint32_t i = 0; i < pci.driver_binding_count(); ++i) {
        const auto& binding = bindings[i];
        if (binding.kind != hk::pci::DriverKind::E1000) continue;
        const auto* device = pci.binding_device(binding);
        if (!device) continue;

        uint64_t mmio_base = 0;
        uint64_t mmio_size = 0;
        uint64_t io_base = 0;
        uint64_t io_size = 0;
        for (uint8_t bar = 0; bar < device->bar_count; ++bar) {
            const auto& candidate = device->bars[bar];
            if ((candidate.type == hk::pci::BarType::Mmio32 || candidate.type == hk::pci::BarType::Mmio64) && mmio_base == 0) {
                mmio_base = candidate.base;
                mmio_size = candidate.size;
            } else if (candidate.type == hk::pci::BarType::Io && io_base == 0) {
                io_base = candidate.base;
                io_size = candidate.size;
            }
        }
        if (mmio_base == 0 || mmio_size == 0) continue;
        adapter_ = {};
        adapter_.present = true;
        adapter_.bus = device->bus;
        adapter_.device = device->device;
        adapter_.function = device->function;
        adapter_.vendor_id = device->vendor_id;
        adapter_.device_id = device->device_id;
        adapter_.mmio_base = mmio_base;
        adapter_.mmio_size = mmio_size;
        adapter_.io_base = io_base;
        adapter_.io_size = io_size;
        adapter_.required_command_bits = binding.required_command_bits;
        adapter_.command_enabled = pci.set_command_bits(*device, binding.required_command_bits);
        adapter_.command_after_enable = pci.command_for(*device);
        uint64_t phys = align_down(adapter_.mmio_base);
        uint64_t page_offset = adapter_.mmio_base - phys;
        uint64_t length = align_up(kProbeWindow + page_offset);
        if (adapter_.mmio_size >= kProbeWindow && length <= 0x20000) {
            auto mapped = hk::mm::vmm().map_range(kE1000MmioVirt, phys, length, hk::mm::PageWrite | hk::mm::PageCacheDisable | hk::mm::PageGlobal);
            if (mapped.ok) {
                adapter_.mmio_virtual = kE1000MmioVirt + page_offset;
                adapter_.mmio_mapped = true;
                sample_register(adapter_, kRegCtrl, adapter_.ctrl);
                sample_register(adapter_, kRegStatus, adapter_.status);
                decode_status(adapter_);
                sample_register(adapter_, kRegEecd, adapter_.eecd);
                sample_register(adapter_, kRegCtrlExt, adapter_.ctrl_ext);
                sample_register(adapter_, kRegMdic, adapter_.mdic);
                mask_and_ack_interrupts(adapter_);
                sample_register(adapter_, kRegRal0, adapter_.mac_low);
                sample_register(adapter_, kRegRah0, adapter_.mac_high);
                adapter_.mac_address =
                    static_cast<uint64_t>(adapter_.mac_low) |
                    ((static_cast<uint64_t>(adapter_.mac_high) & 0xffffull) << 32);
                adapter_.mac_valid = (adapter_.mac_high & kRahAddressValid) != 0 && adapter_.mac_address != 0;
                if (adapter_.command_enabled && allocate_rings(adapter_)) {
                    if (program_rings(adapter_)) {
                        transmit_smoke(adapter_);
                        transmit_validation_smoke(adapter_);
                        rx_idle_poll(adapter_);
                        receive_poll_smoke(adapter_);
                    }
                }
            }
        }
        break;
    }

    hk::log_hex(hk::LogLevel::Info, "e1000 adapter present", adapter_.present ? 1 : 0);
    if (adapter_.present) {
        hk::log_hex(hk::LogLevel::Info, "e1000 adapter bdf", (static_cast<uint64_t>(adapter_.bus) << 16) | (static_cast<uint64_t>(adapter_.device) << 8) | adapter_.function);
        hk::log_hex(hk::LogLevel::Info, "e1000 adapter id", (static_cast<uint64_t>(adapter_.vendor_id) << 16) | adapter_.device_id);
        hk::log_hex(hk::LogLevel::Info, "e1000 MMIO base", adapter_.mmio_base);
        hk::log_hex(hk::LogLevel::Info, "e1000 MMIO size", adapter_.mmio_size);
        hk::log_hex(hk::LogLevel::Info, "e1000 MMIO mapped", adapter_.mmio_mapped ? 1 : 0);
        hk::log_hex(hk::LogLevel::Info, "e1000 MMIO virtual", adapter_.mmio_virtual);
        hk::log_hex(hk::LogLevel::Info, "e1000 IO base", adapter_.io_base);
        hk::log_hex(hk::LogLevel::Info, "e1000 command requirements", adapter_.required_command_bits);
        hk::log_hex(hk::LogLevel::Info, "e1000 command after enable", adapter_.command_after_enable);
        hk::log_hex(hk::LogLevel::Info, "e1000 command enabled", adapter_.command_enabled ? 1 : 0);
        hk::log_hex(hk::LogLevel::Info, "e1000 register reads", adapter_.register_read_count);
        hk::log_hex(hk::LogLevel::Info, "e1000 register writes", adapter_.register_write_count);
        hk::log_hex(hk::LogLevel::Info, "e1000 CTRL", adapter_.ctrl);
        hk::log_hex(hk::LogLevel::Info, "e1000 STATUS", adapter_.status);
        hk::log_hex(hk::LogLevel::Info, "e1000 link up", adapter_.link_up ? 1 : 0);
        hk::log_hex(hk::LogLevel::Info, "e1000 full duplex", adapter_.full_duplex ? 1 : 0);
        hk::log_hex(hk::LogLevel::Info, "e1000 link speed Mbps", adapter_.link_speed_mbps);
        hk::log_hex(hk::LogLevel::Info, "e1000 PCI bus speed MHz", adapter_.pci_bus_speed_mhz);
        hk::log_hex(hk::LogLevel::Info, "e1000 PCI bus width bits", adapter_.pci_bus_width_bits);
        hk::log_hex(hk::LogLevel::Info, "e1000 EECD", adapter_.eecd);
        hk::log_hex(hk::LogLevel::Info, "e1000 CTRL_EXT", adapter_.ctrl_ext);
        hk::log_hex(hk::LogLevel::Info, "e1000 MDIC", adapter_.mdic);
        hk::log_hex(hk::LogLevel::Info, "e1000 ICR", adapter_.icr);
        hk::log_hex(hk::LogLevel::Info, "e1000 IMS", adapter_.ims);
        hk::log_hex(hk::LogLevel::Info, "e1000 IMC", adapter_.imc);
        hk::log_hex(hk::LogLevel::Info, "e1000 interrupts masked", adapter_.interrupts_masked ? 1 : 0);
        hk::log_hex(hk::LogLevel::Info, "e1000 interrupts acked", adapter_.interrupts_acked ? 1 : 0);
        hk::log_hex(hk::LogLevel::Info, "e1000 MAC low", adapter_.mac_low);
        hk::log_hex(hk::LogLevel::Info, "e1000 MAC high", adapter_.mac_high);
        hk::log_hex(hk::LogLevel::Info, "e1000 MAC address", adapter_.mac_address);
        hk::log_hex(hk::LogLevel::Info, "e1000 MAC valid", adapter_.mac_valid ? 1 : 0);
        hk::log_hex(hk::LogLevel::Info, "e1000 rings allocated", adapter_.rings_allocated ? 1 : 0);
        hk::log_hex(hk::LogLevel::Info, "e1000 rings programmed", adapter_.rings_programmed ? 1 : 0);
        hk::log_hex(hk::LogLevel::Info, "e1000 RX desc phys", adapter_.rx_desc_phys);
        hk::log_hex(hk::LogLevel::Info, "e1000 TX desc phys", adapter_.tx_desc_phys);
        hk::log_hex(hk::LogLevel::Info, "e1000 RX desc count", adapter_.rx_desc_count);
        hk::log_hex(hk::LogLevel::Info, "e1000 TX desc count", adapter_.tx_desc_count);
        hk::log_hex(hk::LogLevel::Info, "e1000 RX buffer count", adapter_.rx_buffer_count);
        hk::log_hex(hk::LogLevel::Info, "e1000 TX buffer count", adapter_.tx_buffer_count);
        hk::log_hex(hk::LogLevel::Info, "e1000 TX next index", adapter_.tx_next_index);
        hk::log_hex(hk::LogLevel::Info, "e1000 TX last index", adapter_.tx_last_index);
        hk::log_hex(hk::LogLevel::Info, "e1000 TX reclaim checks", adapter_.tx_reclaim_checks);
        hk::log_hex(hk::LogLevel::Info, "e1000 TX busy failures", adapter_.tx_busy_failures);
        hk::log_hex(hk::LogLevel::Info, "e1000 TX null rejects", adapter_.tx_null_rejects);
        hk::log_hex(hk::LogLevel::Info, "e1000 TX length rejects", adapter_.tx_length_rejects);
        hk::log_hex(hk::LogLevel::Info, "e1000 TX validation smoke attempted", adapter_.tx_validation_smoke_attempted ? 1 : 0);
        hk::log_hex(hk::LogLevel::Info, "e1000 TX validation smoke passed", adapter_.tx_validation_smoke_passed ? 1 : 0);
        hk::log_hex(hk::LogLevel::Info, "e1000 RCTL", adapter_.rctl);
        hk::log_hex(hk::LogLevel::Info, "e1000 TCTL", adapter_.tctl);
        hk::log_hex(hk::LogLevel::Info, "e1000 TIPG", adapter_.tipg);
        hk::log_hex(hk::LogLevel::Info, "e1000 RDBA", (static_cast<uint64_t>(adapter_.rdbah) << 32) | adapter_.rdbal);
        hk::log_hex(hk::LogLevel::Info, "e1000 RDLEN", adapter_.rdlen);
        hk::log_hex(hk::LogLevel::Info, "e1000 TDBA", (static_cast<uint64_t>(adapter_.tdbah) << 32) | adapter_.tdbal);
        hk::log_hex(hk::LogLevel::Info, "e1000 TDLEN", adapter_.tdlen);
        hk::log_hex(hk::LogLevel::Info, "e1000 ring registers verified", adapter_.ring_registers_verified ? 1 : 0);
        hk::log_hex(hk::LogLevel::Info, "e1000 RDH/RDT", (static_cast<uint64_t>(adapter_.rdh) << 32) | adapter_.rdt);
        hk::log_hex(hk::LogLevel::Info, "e1000 TDH/TDT", (static_cast<uint64_t>(adapter_.tdh) << 32) | adapter_.tdt);
        hk::log_hex(hk::LogLevel::Info, "e1000 TX smoke attempted", adapter_.tx_smoke_attempted ? 1 : 0);
        hk::log_hex(hk::LogLevel::Info, "e1000 TX smoke completed", adapter_.tx_smoke_completed ? 1 : 0);
        hk::log_hex(hk::LogLevel::Info, "e1000 TX smoke buffer", adapter_.tx_smoke_buffer_phys);
        hk::log_hex(hk::LogLevel::Info, "e1000 TX smoke length", adapter_.tx_smoke_length);
        hk::log_hex(hk::LogLevel::Info, "e1000 TX smoke polls", adapter_.tx_smoke_polls);
        hk::log_hex(hk::LogLevel::Info, "e1000 TX smoke status", adapter_.tx_smoke_status);
        hk::log_hex(hk::LogLevel::Info, "e1000 TX packets submitted", adapter_.tx_packets_submitted);
        hk::log_hex(hk::LogLevel::Info, "e1000 TX packets completed", adapter_.tx_packets_completed);
        hk::log_hex(hk::LogLevel::Info, "e1000 RX idle polled", adapter_.rx_idle_polled ? 1 : 0);
        hk::log_hex(hk::LogLevel::Info, "e1000 RX ring idle", adapter_.rx_ring_idle ? 1 : 0);
        hk::log_hex(hk::LogLevel::Info, "e1000 RX idle polls", adapter_.rx_idle_polls);
        hk::log_hex(hk::LogLevel::Info, "e1000 RX ready descriptors", adapter_.rx_ready_descriptors);
        hk::log_hex(hk::LogLevel::Info, "e1000 RX first status", adapter_.rx_first_status);
        hk::log_hex(hk::LogLevel::Info, "e1000 RX first length", adapter_.rx_first_length);
        hk::log_hex(hk::LogLevel::Info, "e1000 RX next index", adapter_.rx_next_index);
        hk::log_hex(hk::LogLevel::Info, "e1000 RX last index", adapter_.rx_last_index);
        hk::log_hex(hk::LogLevel::Info, "e1000 RX poll calls", adapter_.rx_poll_calls);
        hk::log_hex(hk::LogLevel::Info, "e1000 RX empty polls", adapter_.rx_empty_polls);
        hk::log_hex(hk::LogLevel::Info, "e1000 RX packets received", adapter_.rx_packets_received);
        hk::log_hex(hk::LogLevel::Info, "e1000 RX bytes received", adapter_.rx_bytes_received);
        hk::log_hex(hk::LogLevel::Info, "e1000 RX small buffer drops", adapter_.rx_small_buffer_drops);
        hk::log_hex(hk::LogLevel::Info, "e1000 RX poll smoke attempted", adapter_.rx_poll_smoke_attempted ? 1 : 0);
        hk::log_hex(hk::LogLevel::Info, "e1000 RX poll smoke empty", adapter_.rx_poll_smoke_empty ? 1 : 0);
    }
}

bool E1000Driver::transmit_frame(const void* data, uint32_t length) {
    uint32_t polls = 0;
    uint32_t status = 0;
    uint64_t buffer = 0;
    uint32_t index = 0;
    return submit_transmit(adapter_, static_cast<const uint8_t*>(data), length, polls, status, buffer, index);
}

bool E1000Driver::poll_receive(void* out, uint32_t capacity, uint32_t& length) {
    return poll_receive_internal(adapter_, out, capacity, length);
}

bool self_test() {
    const auto& a = driver().adapter();
    if (!a.present) return false;
    if (a.vendor_id != 0x8086) return false;
    if (a.mmio_base == 0 || a.mmio_size == 0 || a.mmio_size > (16ull * 1024 * 1024)) return false;
    if (!a.command_enabled || (a.command_after_enable & a.required_command_bits) != a.required_command_bits) return false;
    if (!a.mmio_mapped || a.mmio_virtual == 0 || a.register_read_count < 24 || a.register_write_count < 16) return false;
    if (a.ctrl == 0xffffffffu || a.status == 0xffffffffu || a.eecd == 0xffffffffu) return false;
    if (!a.link_up || !a.full_duplex || a.link_speed_mbps != 1000 || a.pci_bus_speed_mhz == 0 || (a.pci_bus_width_bits != 32 && a.pci_bus_width_bits != 64)) return false;
    if (!a.interrupts_masked || !a.interrupts_acked || a.imc != 0xffffffffu) return false;
    if (!a.rings_allocated || !a.rings_programmed || a.rx_desc_phys == 0 || a.tx_desc_phys == 0) return false;
    if (a.rx_desc_count != kRxDescCount || a.tx_desc_count != kTxDescCount || a.rx_buffer_count != kRxDescCount || a.tx_buffer_count != kTxDescCount) return false;
    if ((a.rctl & kRctlEnable) == 0 || (a.tctl & kTctlEnable) == 0) return false;
    if (!a.ring_registers_verified ||
        a.rdbal != static_cast<uint32_t>(a.rx_desc_phys) ||
        a.rdbah != static_cast<uint32_t>(a.rx_desc_phys >> 32) ||
        a.rdlen != kRxDescCount * 16 ||
        a.tdbal != static_cast<uint32_t>(a.tx_desc_phys) ||
        a.tdbah != static_cast<uint32_t>(a.tx_desc_phys >> 32) ||
        a.tdlen != kTxDescCount * 16) return false;
    if (a.rdt != kRxDescCount - 1) return false;
    if (!a.tx_smoke_attempted || !a.tx_smoke_completed || a.tx_smoke_buffer_phys == 0) return false;
    if (a.tx_smoke_length != 60 || a.tx_smoke_status == 0 || a.tx_packets_submitted != 1 || a.tx_packets_completed != 1) return false;
    if (a.tx_next_index != 1 || a.tx_last_index != 0 || a.tx_reclaim_checks != 1 || a.tx_busy_failures != 0) return false;
    if (a.tx_null_rejects != 1 || a.tx_length_rejects != 2 || !a.tx_validation_smoke_attempted || !a.tx_validation_smoke_passed) return false;
    if (!a.rx_idle_polled || !a.rx_ring_idle || a.rx_ready_descriptors != 0 || a.rx_first_status != 0 || a.rx_first_length != 0) return false;
    if (!a.rx_poll_smoke_attempted || !a.rx_poll_smoke_empty || a.rx_poll_calls != 1 || a.rx_empty_polls != 1) return false;
    if (a.rx_next_index != 0 || a.rx_packets_received != 0 || a.rx_bytes_received != 0 || a.rx_small_buffer_drops != 0) return false;
    if ((a.required_command_bits & hk::pci::CommandMemorySpace) == 0) return false;
    if ((a.required_command_bits & hk::pci::CommandBusMaster) == 0) return false;
    return true;
}

} // namespace hk::drivers::e1000
