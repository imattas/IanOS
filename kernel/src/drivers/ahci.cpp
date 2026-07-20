#include "hk/drivers/ahci.hpp"
#include "hk/lib/string.hpp"
#include "hk/log.hpp"
#include "hk/mm/pmm.hpp"
#include "hk/mm/vmm.hpp"

namespace hk::drivers::ahci {
namespace {
constexpr uint64_t kAhciAbarVirt = 0xffff800001000000ull;
constexpr uint32_t kRegCap = 0x00;
constexpr uint32_t kRegGhc = 0x04;
constexpr uint32_t kRegPortsImplemented = 0x0c;
constexpr uint32_t kRegVersion = 0x10;
constexpr uint32_t kPortBase = 0x100;
constexpr uint32_t kPortStride = 0x80;
constexpr uint32_t kPortClb = 0x00;
constexpr uint32_t kPortClbu = 0x04;
constexpr uint32_t kPortFb = 0x08;
constexpr uint32_t kPortFbu = 0x0c;
constexpr uint32_t kPortIs = 0x10;
constexpr uint32_t kPortCmd = 0x18;
constexpr uint32_t kPortTfd = 0x20;
constexpr uint32_t kPortSignature = 0x24;
constexpr uint32_t kPortSsts = 0x28;
constexpr uint32_t kPortCi = 0x38;
constexpr uint64_t kPageSize = 4096;
constexpr uint32_t kCmdSt = 1u << 0;
constexpr uint32_t kCmdFre = 1u << 4;
constexpr uint32_t kCmdFr = 1u << 14;
constexpr uint32_t kCmdCr = 1u << 15;
constexpr uint8_t kFisTypeRegH2d = 0x27;
constexpr uint8_t kAtaReadDmaExt = 0x25;
constexpr uint8_t kAtaIdentify = 0xec;
constexpr uint32_t kAtaBusyDrq = 0x88;

uint64_t align_down(uint64_t value) { return value & ~(kPageSize - 1); }
uint64_t align_up(uint64_t value) { return (value + kPageSize - 1) & ~(kPageSize - 1); }

uint32_t read_hba32(uint64_t hba_virtual, uint32_t offset) {
    return *reinterpret_cast<volatile uint32_t*>(hba_virtual + offset);
}

void write_hba32(uint64_t hba_virtual, uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(hba_virtual + offset) = value;
}

void zero_page(uint64_t physical) {
    auto* bytes = reinterpret_cast<volatile uint8_t*>(physical);
    for (uint64_t i = 0; i < kPageSize; ++i) bytes[i] = 0;
}

void write_u32(volatile uint8_t* base, uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(const_cast<uint8_t*>(base) + offset) = value;
}

void write_u16(volatile uint8_t* base, uint32_t offset, uint16_t value) {
    *reinterpret_cast<volatile uint16_t*>(const_cast<uint8_t*>(base) + offset) = value;
}

void write_u8(volatile uint8_t* base, uint32_t offset, uint8_t value) {
    base[offset] = value;
}

uint32_t bit_count(uint32_t value) {
    uint32_t total = 0;
    while (value != 0) {
        total += value & 1u;
        value >>= 1;
    }
    return total;
}

bool port_active(uint32_t ssts) {
    uint32_t det = ssts & 0xf;
    uint32_t ipm = (ssts >> 8) & 0xf;
    return det == 3 && ipm == 1;
}

bool wait_clear(uint64_t hba, uint32_t offset, uint32_t mask, uint32_t limit) {
    for (uint32_t i = 0; i < limit; ++i) {
        if ((read_hba32(hba, offset) & mask) == 0) return true;
        asm volatile("pause");
    }
    return false;
}

bool stop_port(Controller& controller, uint32_t port_base, uint32_t& saved_cmd) {
    saved_cmd = read_hba32(controller.hba_virtual, port_base + kPortCmd);
    write_hba32(controller.hba_virtual, port_base + kPortCmd, saved_cmd & ~(kCmdSt | kCmdFre));
    return wait_clear(controller.hba_virtual, port_base + kPortCmd, kCmdCr | kCmdFr, 100000);
}

void start_port(Controller& controller, uint32_t port_base, uint32_t saved_cmd) {
    write_hba32(controller.hba_virtual, port_base + kPortCmd, (saved_cmd | kCmdFre | kCmdSt) & ~(kCmdCr | kCmdFr));
}

bool setup_slot0(Controller& controller, uint32_t port_base, uint64_t transfer_page, uint32_t byte_count, uint8_t command, bool dma_read, uint64_t lba, uint16_t sector_count) {
    uint64_t command_list = hk::mm::pmm().allocate_page();
    uint64_t fis = hk::mm::pmm().allocate_page();
    uint64_t command_table = hk::mm::pmm().allocate_page();
    if (!command_list || !fis || !command_table || !transfer_page || byte_count == 0) return false;
    zero_page(command_list);
    zero_page(fis);
    zero_page(command_table);
    zero_page(transfer_page);

    write_hba32(controller.hba_virtual, port_base + kPortClb, static_cast<uint32_t>(command_list));
    write_hba32(controller.hba_virtual, port_base + kPortClbu, static_cast<uint32_t>(command_list >> 32));
    write_hba32(controller.hba_virtual, port_base + kPortFb, static_cast<uint32_t>(fis));
    write_hba32(controller.hba_virtual, port_base + kPortFbu, static_cast<uint32_t>(fis >> 32));
    write_hba32(controller.hba_virtual, port_base + kPortIs, 0xffffffffu);

    auto* header = reinterpret_cast<volatile uint8_t*>(command_list);
    write_u16(header, 0x00, 5);
    write_u16(header, 0x02, 1);
    write_u32(header, 0x08, static_cast<uint32_t>(command_table));
    write_u32(header, 0x0c, static_cast<uint32_t>(command_table >> 32));

    auto* table = reinterpret_cast<volatile uint8_t*>(command_table);
    write_u8(table, 0x00, kFisTypeRegH2d);
    write_u8(table, 0x01, 0x80);
    write_u8(table, 0x02, command);
    write_u8(table, 0x07, 1u << 6);
    if (dma_read) {
        write_u8(table, 0x04, static_cast<uint8_t>(lba & 0xff));
        write_u8(table, 0x05, static_cast<uint8_t>((lba >> 8) & 0xff));
        write_u8(table, 0x06, static_cast<uint8_t>((lba >> 16) & 0xff));
        write_u8(table, 0x08, static_cast<uint8_t>((lba >> 24) & 0xff));
        write_u8(table, 0x09, static_cast<uint8_t>((lba >> 32) & 0xff));
        write_u8(table, 0x0a, static_cast<uint8_t>((lba >> 40) & 0xff));
        write_u8(table, 0x0c, static_cast<uint8_t>(sector_count & 0xff));
        write_u8(table, 0x0d, static_cast<uint8_t>((sector_count >> 8) & 0xff));
    }
    write_u32(table, 0x80, static_cast<uint32_t>(transfer_page));
    write_u32(table, 0x84, static_cast<uint32_t>(transfer_page >> 32));
    write_u32(table, 0x8c, (byte_count - 1) | (1u << 31));
    return true;
}

bool wait_command_complete(Controller& controller, uint32_t port_base, uint32_t& status, uint32_t& error) {
    write_hba32(controller.hba_virtual, port_base + kPortCi, 1);
    error = 0;
    bool complete = false;
    for (uint32_t i = 0; i < 2000000; ++i) {
        uint32_t ci = read_hba32(controller.hba_virtual, port_base + kPortCi);
        uint32_t is = read_hba32(controller.hba_virtual, port_base + kPortIs);
        if ((is & (1u << 30)) != 0) {
            error = is;
            break;
        }
        if ((ci & 1u) == 0) {
            complete = true;
            break;
        }
        asm volatile("pause");
    }
    status = read_hba32(controller.hba_virtual, port_base + kPortTfd);
    if (!complete && error == 0) error = 4;
    return complete;
}

bool issue_command(Controller& controller, uint8_t command, bool dma_read, uint64_t lba, uint16_t sectors, uint64_t buffer, uint32_t bytes, uint32_t& status, uint32_t& error) {
    uint32_t port_base = kPortBase + controller.first_active_port * kPortStride;
    uint32_t saved_cmd = 0;
    if (!stop_port(controller, port_base, saved_cmd)) {
        status = read_hba32(controller.hba_virtual, port_base + kPortCmd);
        error = 1;
        return false;
    }
    if (!setup_slot0(controller, port_base, buffer, bytes, command, dma_read, lba, sectors)) {
        error = 2;
        return false;
    }
    if (!wait_clear(controller.hba_virtual, port_base + kPortTfd, kAtaBusyDrq, 100000)) {
        status = read_hba32(controller.hba_virtual, port_base + kPortTfd);
        error = 3;
        return false;
    }
    start_port(controller, port_base, saved_cmd);
    return wait_command_complete(controller, port_base, status, error);
}

bool issue_identify(Controller& controller) {
    if (!controller.hba_mapped || controller.first_active_port >= 32 || controller.first_active_signature != 0x00000101u) return false;
    controller.identify_attempted = true;
    uint64_t identify = hk::mm::pmm().allocate_page();
    if (!issue_command(controller, kAtaIdentify, false, 0, 0, identify, 512, controller.identify_status, controller.identify_error)) return false;
    auto* words = reinterpret_cast<volatile uint16_t*>(identify);
    controller.identify_signature_word = words[0];
    controller.identify_capabilities = words[49];
    controller.identify_major_version = words[80];
    controller.identify_lba28_sectors = static_cast<uint64_t>(words[60]) |
        (static_cast<uint64_t>(words[61]) << 16);
    controller.identify_lba48_sectors = static_cast<uint64_t>(words[100]) |
        (static_cast<uint64_t>(words[101]) << 16) |
        (static_cast<uint64_t>(words[102]) << 32) |
        (static_cast<uint64_t>(words[103]) << 48);
    controller.identify_sector_count = controller.identify_lba48_sectors != 0
        ? controller.identify_lba48_sectors
        : controller.identify_lba28_sectors;
    controller.identify_success = controller.identify_signature_word != 0;
    return controller.identify_success;
}

bool issue_read_lba0(Controller& controller) {
    if (!controller.identify_success || controller.first_active_port >= 32) return false;
    controller.read_lba0_attempted = true;
    uint64_t sector = hk::mm::pmm().allocate_page();
    if (!issue_command(controller, kAtaReadDmaExt, true, 0, 1, sector, 512, controller.read_lba0_status, controller.read_lba0_error)) return false;
    auto* bytes = reinterpret_cast<volatile uint8_t*>(sector);
    controller.read_lba0_boot_signature = static_cast<uint16_t>(bytes[510] | (static_cast<uint16_t>(bytes[511]) << 8));
    controller.read_lba0_oem = static_cast<uint32_t>(bytes[3]) |
        (static_cast<uint32_t>(bytes[4]) << 8) |
        (static_cast<uint32_t>(bytes[5]) << 16) |
        (static_cast<uint32_t>(bytes[6]) << 24);
    controller.read_lba0_fstype = static_cast<uint64_t>(bytes[54]) |
        (static_cast<uint64_t>(bytes[55]) << 8) |
        (static_cast<uint64_t>(bytes[56]) << 16) |
        (static_cast<uint64_t>(bytes[57]) << 24) |
        (static_cast<uint64_t>(bytes[58]) << 32) |
        (static_cast<uint64_t>(bytes[59]) << 40) |
        (static_cast<uint64_t>(bytes[60]) << 48) |
        (static_cast<uint64_t>(bytes[61]) << 56);
    controller.read_lba0_success = controller.read_lba0_boot_signature == 0xaa55;
    controller.read_lba0_buffer = sector;
    return controller.read_lba0_success;
}
}

AhciDriver& driver() {
    static AhciDriver instance;
    return instance;
}

bool AhciDriver::read_sector(uint64_t lba, void* out_512) {
    if (!out_512 || !controller_.identify_success || controller_.first_active_port >= 32) return false;
    uint64_t sector = hk::mm::pmm().allocate_page();
    uint32_t status = 0;
    uint32_t error = 0;
    if (!issue_command(controller_, kAtaReadDmaExt, true, lba, 1, sector, 512, status, error)) return false;
    memcpy(out_512, reinterpret_cast<const void*>(sector), 512);
    return true;
}

void AhciDriver::probe(const hk::pci::PciRegistry& pci) {
    controller_ = {};
    const auto* bindings = pci.driver_bindings();
    for (uint32_t i = 0; i < pci.driver_binding_count(); ++i) {
        const auto& binding = bindings[i];
        if (binding.kind != hk::pci::DriverKind::Ahci) continue;
        const auto* device = pci.binding_device(binding);
        if (!device) continue;
        const hk::pci::Bar* abar = nullptr;
        for (uint8_t bar = 0; bar < device->bar_count; ++bar) {
            const auto& candidate = device->bars[bar];
            if (candidate.type == hk::pci::BarType::Mmio32 || candidate.type == hk::pci::BarType::Mmio64) abar = &candidate;
        }
        if (!abar || abar->base == 0 || abar->size == 0) continue;
        controller_ = Controller{
            true, device->bus, device->device, device->function, device->vendor_id, device->device_id,
            abar->base, abar->size, 0, binding.required_command_bits, false, 0, 0, 0, 0, 0, 0, 0xffffffffu,
            0, 0, 0, false, false, 0, 0, 0, 0, 0, 0, 0, false, false, 0, 0, 0, 0, 0, 0, 0,
        };
        uint64_t phys = align_down(controller_.abar);
        uint64_t page_offset = controller_.abar - phys;
        uint64_t length = align_up(controller_.abar_size + page_offset);
        if (length != 0 && length <= 0x20000) {
            auto mapped = hk::mm::vmm().map_range(kAhciAbarVirt, phys, length, hk::mm::PageWrite | hk::mm::PageCacheDisable | hk::mm::PageGlobal);
            if (mapped.ok) {
                controller_.hba_virtual = kAhciAbarVirt + page_offset;
                controller_.hba_mapped = true;
                controller_.cap = read_hba32(controller_.hba_virtual, kRegCap);
                controller_.ghc = read_hba32(controller_.hba_virtual, kRegGhc);
                controller_.version = read_hba32(controller_.hba_virtual, kRegVersion);
                controller_.ports_implemented = read_hba32(controller_.hba_virtual, kRegPortsImplemented);
                controller_.implemented_port_count = bit_count(controller_.ports_implemented);
                for (uint32_t port = 0; port < 32; ++port) {
                    if ((controller_.ports_implemented & (1u << port)) == 0) continue;
                    uint32_t base = kPortBase + port * kPortStride;
                    uint32_t signature = read_hba32(controller_.hba_virtual, base + kPortSignature);
                    uint32_t ssts = read_hba32(controller_.hba_virtual, base + kPortSsts);
                    uint32_t cmd = read_hba32(controller_.hba_virtual, base + kPortCmd);
                    if (port_active(ssts)) {
                        ++controller_.active_port_count;
                        if (controller_.first_active_port == 0xffffffffu) {
                            controller_.first_active_port = port;
                            controller_.first_active_signature = signature;
                            controller_.first_active_ssts = ssts;
                            controller_.first_active_cmd = cmd;
                        }
                    }
                }
                issue_identify(controller_);
                issue_read_lba0(controller_);
            }
        }
        break;
    }

    hk::log_hex(hk::LogLevel::Info, "AHCI controller present", controller_.present ? 1 : 0);
    if (!controller_.present) return;
    hk::log_hex(hk::LogLevel::Info, "AHCI controller bdf", (static_cast<uint64_t>(controller_.bus) << 16) | (static_cast<uint64_t>(controller_.device) << 8) | controller_.function);
    hk::log_hex(hk::LogLevel::Info, "AHCI controller id", (static_cast<uint64_t>(controller_.vendor_id) << 16) | controller_.device_id);
    hk::log_hex(hk::LogLevel::Info, "AHCI ABAR base", controller_.abar);
    hk::log_hex(hk::LogLevel::Info, "AHCI ABAR size", controller_.abar_size);
    hk::log_hex(hk::LogLevel::Info, "AHCI command requirements", controller_.required_command_bits);
    hk::log_hex(hk::LogLevel::Info, "AHCI HBA mapped", controller_.hba_mapped ? 1 : 0);
    hk::log_hex(hk::LogLevel::Info, "AHCI CAP", controller_.cap);
    hk::log_hex(hk::LogLevel::Info, "AHCI GHC", controller_.ghc);
    hk::log_hex(hk::LogLevel::Info, "AHCI version", controller_.version);
    hk::log_hex(hk::LogLevel::Info, "AHCI ports implemented", controller_.ports_implemented);
    hk::log_hex(hk::LogLevel::Info, "AHCI implemented port count", controller_.implemented_port_count);
    hk::log_hex(hk::LogLevel::Info, "AHCI active port count", controller_.active_port_count);
    if (controller_.first_active_port != 0xffffffffu) {
        hk::log_hex(hk::LogLevel::Info, "AHCI first active port", controller_.first_active_port);
        hk::log_hex(hk::LogLevel::Info, "AHCI first active signature", controller_.first_active_signature);
        hk::log_hex(hk::LogLevel::Info, "AHCI first active SSTS", controller_.first_active_ssts);
        hk::log_hex(hk::LogLevel::Info, "AHCI first active CMD", controller_.first_active_cmd);
    }
    hk::log_hex(hk::LogLevel::Info, "AHCI identify attempted", controller_.identify_attempted ? 1 : 0);
    hk::log_hex(hk::LogLevel::Info, "AHCI identify success", controller_.identify_success ? 1 : 0);
    hk::log_hex(hk::LogLevel::Info, "AHCI identify status", controller_.identify_status);
    hk::log_hex(hk::LogLevel::Info, "AHCI identify error", controller_.identify_error);
    if (controller_.identify_success) {
        hk::log_hex(hk::LogLevel::Info, "AHCI identify word0", controller_.identify_signature_word);
        hk::log_hex(hk::LogLevel::Info, "AHCI identify capabilities", controller_.identify_capabilities);
        hk::log_hex(hk::LogLevel::Info, "AHCI identify major version", controller_.identify_major_version);
        hk::log_hex(hk::LogLevel::Info, "AHCI identify LBA28 sectors", controller_.identify_lba28_sectors);
        hk::log_hex(hk::LogLevel::Info, "AHCI identify LBA48 sectors", controller_.identify_lba48_sectors);
        hk::log_hex(hk::LogLevel::Info, "AHCI identify sector count", controller_.identify_sector_count);
    }
    hk::log_hex(hk::LogLevel::Info, "AHCI read lba0 attempted", controller_.read_lba0_attempted ? 1 : 0);
    hk::log_hex(hk::LogLevel::Info, "AHCI read lba0 success", controller_.read_lba0_success ? 1 : 0);
    hk::log_hex(hk::LogLevel::Info, "AHCI read lba0 status", controller_.read_lba0_status);
    hk::log_hex(hk::LogLevel::Info, "AHCI read lba0 error", controller_.read_lba0_error);
    if (controller_.read_lba0_success) {
        hk::log_hex(hk::LogLevel::Info, "AHCI read lba0 boot signature", controller_.read_lba0_boot_signature);
        hk::log_hex(hk::LogLevel::Info, "AHCI read lba0 OEM", controller_.read_lba0_oem);
        hk::log_hex(hk::LogLevel::Info, "AHCI read lba0 fstype", controller_.read_lba0_fstype);
        hk::log_hex(hk::LogLevel::Info, "AHCI read lba0 buffer", controller_.read_lba0_buffer);
    }
}

bool self_test() {
    const auto& c = driver().controller();
    if (!c.present) return false;
    if (c.abar == 0 || c.abar_size == 0 || c.abar_size > (16ull * 1024 * 1024)) return false;
    if ((c.required_command_bits & hk::pci::CommandMemorySpace) == 0) return false;
    if ((c.required_command_bits & hk::pci::CommandBusMaster) == 0) return false;
    if (!c.hba_mapped || c.hba_virtual == 0) return false;
    if (c.cap == 0 || c.version == 0 || c.ports_implemented == 0 || c.implemented_port_count == 0) return false;
    if (c.active_port_count != 0 && c.first_active_port >= 32) return false;
    if (!c.identify_attempted || !c.identify_success || c.identify_signature_word == 0 || c.identify_sector_count == 0) return false;
    if (!c.read_lba0_attempted || !c.read_lba0_success || c.read_lba0_boot_signature != 0xaa55) return false;
    unsigned char sector[512]{};
    if (!driver().read_sector(0, sector)) return false;
    if (sector[510] != 0x55 || sector[511] != 0xaa) return false;
    return true;
}

} // namespace hk::drivers::ahci
