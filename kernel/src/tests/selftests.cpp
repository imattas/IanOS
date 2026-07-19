#include "hk/tests/selftests.hpp"
#include "hk/boot/bootinfo.hpp"
#include "hk/log.hpp"
#include "hk/mm/pmm.hpp"
#include "hk/mm/vmm.hpp"
#include "hk/mm/address_space.hpp"
#include "hk/mm/heap.hpp"
#include "hk/acpi/acpi.hpp"
#include "hk/sched/scheduler.hpp"
#include "hk/syscall/syscall.hpp"
#include "hk/userspace/userspace.hpp"
#include "hk/interrupts/irq.hpp"
#include "hk/apic/apic.hpp"
#include "hk/apic/io_apic.hpp"
#include "hk/pci/pci.hpp"
#include "hk/drivers/driver_manager.hpp"
#include "hk/drivers/ahci.hpp"
#include "hk/drivers/e1000.hpp"
#include "hk/drivers/vga.hpp"
#include "hk/drivers/device_inventory.hpp"
#include "hk/block/block_device.hpp"
#include "hk/fs/vfs.hpp"
#include "hk/console.hpp"
#include "hk/terminal.hpp"

namespace hk::tests {

namespace {
bool fail(const char* name) {
    hk::log(hk::LogLevel::Error, name);
    return false;
}

void test_thread_entry(void*) {}
}

bool run_boot_self_tests(const hybrid::BootInfo& boot) {
    bool ok = true;
    hk::log(hk::LogLevel::Info, "SELFTEST bootinfo begin");
    auto validation = hk::boot::validate_boot_info(&boot);
    if (!validation.ok) ok = fail(validation.reason);
    else hk::log(hk::LogLevel::Info, "SELFTEST bootinfo PASS");

    hk::log(hk::LogLevel::Info, "SELFTEST PMM begin");
    uint64_t before = hk::mm::pmm().free_pages();
    auto pmm_diag_before = hk::mm::pmm().diagnostics();
    uint64_t a = hk::mm::pmm().allocate_page();
    uint64_t b = hk::mm::pmm().allocate_contiguous(3);
    if (a == 0 || b == 0 || a == b || (a & 0xfff) || (b & 0xfff)) ok = fail("SELFTEST PMM FAIL");
    if (hk::mm::pmm().allocate_contiguous(0) != 0) ok = fail("SELFTEST PMM zero contiguous FAIL");
    hk::mm::pmm().free_page(a + 1);
    hk::mm::pmm().free_page(a);
    hk::mm::pmm().free_contiguous(b, 3);
    auto pmm_diag_after = hk::mm::pmm().diagnostics();
    if (hk::mm::pmm().free_pages() != before) ok = fail("SELFTEST PMM stats did not recover");
    if (pmm_diag_after.allocate_page_calls <= pmm_diag_before.allocate_page_calls ||
        pmm_diag_after.allocate_contiguous_calls <= pmm_diag_before.allocate_contiguous_calls ||
        pmm_diag_after.failed_allocations <= pmm_diag_before.failed_allocations ||
        pmm_diag_after.invalid_frees <= pmm_diag_before.invalid_frees ||
        pmm_diag_after.peak_used_pages < hk::mm::pmm().stats().used_pages) ok = fail("SELFTEST PMM diagnostics FAIL");
    if (ok) {
        hk::log_hex(hk::LogLevel::Info, "PMM alloc page calls", pmm_diag_after.allocate_page_calls);
        hk::log_hex(hk::LogLevel::Info, "PMM alloc contiguous calls", pmm_diag_after.allocate_contiguous_calls);
        hk::log_hex(hk::LogLevel::Info, "PMM failed allocations", pmm_diag_after.failed_allocations);
        hk::log_hex(hk::LogLevel::Info, "PMM invalid frees", pmm_diag_after.invalid_frees);
        hk::log_hex(hk::LogLevel::Info, "PMM peak used pages", pmm_diag_after.peak_used_pages);
        hk::log(hk::LogLevel::Info, "SELFTEST PMM PASS");
    }

    hk::log(hk::LogLevel::Info, "SELFTEST VMM begin");
    auto vmm_diag_before = hk::mm::vmm().diagnostics();
    uint64_t phys = hk::mm::pmm().allocate_page();
    uint64_t phys_range = hk::mm::pmm().allocate_contiguous(2);
    constexpr uint64_t test_virt = 0xffff800000200000ull;
    constexpr uint64_t range_virt = 0xffff800000210000ull;
    auto mapped = hk::mm::vmm().map_page(test_virt, phys, hk::mm::PageWrite | hk::mm::PageGlobal);
    if (!mapped.ok) ok = fail(mapped.error);
    else {
        auto* ptr = reinterpret_cast<volatile uint64_t*>(test_virt);
        *ptr = 0x123456789abcdef0ull;
        if (*ptr != 0x123456789abcdef0ull) ok = fail("SELFTEST VMM read/write FAIL");
        if (hk::mm::vmm().translate(test_virt) != phys) ok = fail("SELFTEST VMM translate FAIL");
        if (hk::mm::vmm().map_page(test_virt, phys, hk::mm::PageWrite).ok) ok = fail("SELFTEST VMM duplicate map FAIL");
        hk::mm::vmm().unmap_page(test_virt);
        if (hk::mm::vmm().translate(test_virt) != 0) ok = fail("SELFTEST VMM unmap translate FAIL");
        if (hk::mm::vmm().unmap_page(test_virt).ok) ok = fail("SELFTEST VMM absent unmap FAIL");
    }
    if (hk::mm::vmm().map_page(test_virt + 1, phys, hk::mm::PageWrite).ok) ok = fail("SELFTEST VMM unaligned map FAIL");
    auto range_mapped = hk::mm::vmm().map_range(range_virt, phys_range, hk::mm::kPageSize * 2, hk::mm::PageWrite | hk::mm::PageGlobal | hk::mm::PageNoExecute);
    if (!range_mapped.ok) ok = fail(range_mapped.error);
    else {
        if (hk::mm::vmm().translate(range_virt) != phys_range ||
            hk::mm::vmm().translate(range_virt + hk::mm::kPageSize) != phys_range + hk::mm::kPageSize) ok = fail("SELFTEST VMM range translate FAIL");
        hk::mm::vmm().unmap_page(range_virt);
        hk::mm::vmm().unmap_page(range_virt + hk::mm::kPageSize);
    }
    hk::mm::pmm().free_page(phys);
    hk::mm::pmm().free_contiguous(phys_range, 2);
    auto vmm_diag_after = hk::mm::vmm().diagnostics();
    if (vmm_diag_after.mapped_pages < vmm_diag_before.mapped_pages + 3 ||
        vmm_diag_after.unmapped_pages < vmm_diag_before.unmapped_pages + 3 ||
        vmm_diag_after.duplicate_map_rejects <= vmm_diag_before.duplicate_map_rejects ||
        vmm_diag_after.unaligned_map_rejects <= vmm_diag_before.unaligned_map_rejects ||
        vmm_diag_after.absent_unmap_rejects <= vmm_diag_before.absent_unmap_rejects) ok = fail("SELFTEST VMM diagnostics FAIL");
    if (ok) {
        hk::log_hex(hk::LogLevel::Info, "VMM mapped pages", vmm_diag_after.mapped_pages);
        hk::log_hex(hk::LogLevel::Info, "VMM unmapped pages", vmm_diag_after.unmapped_pages);
        hk::log_hex(hk::LogLevel::Info, "VMM failed maps", vmm_diag_after.failed_maps);
        hk::log_hex(hk::LogLevel::Info, "VMM duplicate rejects", vmm_diag_after.duplicate_map_rejects);
        hk::log_hex(hk::LogLevel::Info, "VMM unaligned rejects", vmm_diag_after.unaligned_map_rejects);
        hk::log_hex(hk::LogLevel::Info, "VMM absent unmap rejects", vmm_diag_after.absent_unmap_rejects);
        hk::log_hex(hk::LogLevel::Info, "VMM remote shootdowns", vmm_diag_after.remote_shootdowns_requested);
        hk::log(hk::LogLevel::Info, "SELFTEST VMM PASS");
    }

    hk::log(hk::LogLevel::Info, "SELFTEST address-space begin");
    if (!hk::mm::address_space_self_test()) ok = fail("SELFTEST address-space FAIL");
    else hk::log(hk::LogLevel::Info, "SELFTEST address-space PASS");

    hk::log(hk::LogLevel::Info, "SELFTEST heap begin");
    void* small = hk::mm::kmalloc(32);
    void* medium = hk::mm::kmalloc(2048);
    void* aligned = hk::mm::kmalloc_aligned(128, 64);
    if (!small || !medium || !aligned || (reinterpret_cast<uint64_t>(aligned) & 63)) ok = fail("SELFTEST heap allocation FAIL");
    auto* bytes = static_cast<unsigned char*>(medium);
    for (int i = 0; i < 64; ++i) bytes[i] = static_cast<unsigned char>(i);
    for (int i = 0; i < 64; ++i) if (bytes[i] != static_cast<unsigned char>(i)) ok = fail("SELFTEST heap pattern FAIL");
    hk::mm::kfree(small);
    hk::mm::kfree(medium);
    hk::mm::kfree(aligned);
    if (ok) hk::log(hk::LogLevel::Info, "SELFTEST heap PASS");

    hk::log(hk::LogLevel::Info, "SELFTEST ACPI begin");
    const auto& acpi = hk::acpi::platform();
    const auto& acpi_diag = hk::acpi::diagnostics();
    bool acpi_sane = boot.rsdp == 0 || acpi.valid;
    if (boot.rsdp != 0) {
        acpi_sane = acpi_sane &&
            acpi_diag.rsdp_present &&
            acpi_diag.rsdp_signature_valid &&
            acpi_diag.rsdp_base_checksum_valid &&
            acpi_diag.rsdp_extended_checksum_valid &&
            acpi.root_table != 0 &&
            acpi_diag.root_entry_count > 0 &&
            acpi_diag.tables_scanned >= acpi_diag.tables_valid &&
            acpi_diag.checksum_failures == 0 &&
            acpi_diag.malformed_tables == 0;
        if (acpi.madt != 0) {
            acpi_sane = acpi_sane &&
                acpi.local_apic_base != 0 &&
                acpi.cpu_count > 0 &&
                acpi.io_apic_count > 0 &&
                acpi_diag.madt_entries >= acpi_diag.madt_lapic_entries + acpi_diag.madt_ioapic_entries &&
                acpi_diag.madt_lapic_entries >= acpi.cpu_count &&
                acpi_diag.madt_ioapic_entries >= acpi.io_apic_count &&
                acpi_diag.madt_override_entries >= acpi.override_count &&
                acpi_diag.madt_malformed_entries == 0;
        }
    }
    if (acpi.mcfg != 0) {
        acpi_sane = acpi_sane && acpi.ecam_region_count > 0 && acpi_diag.mcfg_entries > 0 &&
            acpi_diag.ecam_regions_validated == acpi.ecam_region_count &&
            acpi_diag.ecam_overlaps_rejected == 0;
        for (uint32_t i = 0; i < acpi.ecam_region_count; ++i) {
            const auto& ecam = acpi.ecam_regions[i];
            if (ecam.base == 0 || ecam.end_bus < ecam.start_bus) acpi_sane = false;
            for (uint32_t j = i + 1; j < acpi.ecam_region_count; ++j) {
                const auto& other = acpi.ecam_regions[j];
                if (ecam.segment == other.segment && !(ecam.end_bus < other.start_bus || ecam.start_bus > other.end_bus)) acpi_sane = false;
            }
        }
    }
    if (!acpi_sane) ok = fail("SELFTEST ACPI FAIL");
    else hk::log(hk::LogLevel::Info, "SELFTEST ACPI PASS");

    hk::log(hk::LogLevel::Info, "SELFTEST Local APIC timer begin");
    if (!hk::apic::local_apic().enabled() ||
        (hk::apic::local_apic().timer_lvt() & 0xff) != 0x40 ||
        (hk::apic::local_apic().timer_lvt() & (1u << 16)) == 0 ||
        hk::apic::local_apic().timer_initial_count() != 0x10000 ||
        hk::apic::local_apic().timer_probe_delta() == 0 ||
        hk::apic::local_apic().timer_probe_current() >= hk::apic::local_apic().timer_probe_initial()) {
        ok = fail("SELFTEST Local APIC timer FAIL");
    } else {
        hk::log_hex(hk::LogLevel::Info, "SELFTEST Local APIC timer probe delta", hk::apic::local_apic().timer_probe_delta());
        hk::log(hk::LogLevel::Info, "SELFTEST Local APIC timer PASS");
    }

    hk::log(hk::LogLevel::Info, "SELFTEST IRQ routing begin");
    auto irq_stats_before = hk::interrupts::stats();
    uint32_t irq0_gsi = hk::interrupts::legacy_irq_to_gsi(0);
    uint32_t irq1_gsi = hk::interrupts::legacy_irq_to_gsi(1);
    if (!hk::apic::io_apic().enabled() ||
        !hk::apic::io_apic().handles_gsi(irq0_gsi) ||
        !hk::apic::io_apic().handles_gsi(irq1_gsi) ||
        !hk::interrupts::ioapic_route_matches(0, 0x20, true) ||
        !hk::interrupts::ioapic_route_matches(1, 0x21, true)) {
        ok = fail("SELFTEST IRQ routing FAIL");
    } else {
        auto irq_stats_after = hk::interrupts::stats();
        if (irq_stats_after.pic_remaps == 0 ||
            irq_stats_after.ioapic_route_prepares < 2 ||
            irq_stats_after.ioapic_route_match_checks < irq_stats_before.ioapic_route_match_checks + 2 ||
            irq_stats_after.ioapic_route_match_successes < irq_stats_before.ioapic_route_match_successes + 2) {
            ok = fail("SELFTEST IRQ diagnostics FAIL");
        }
        hk::log_hex(hk::LogLevel::Info, "SELFTEST IRQ0 GSI", irq0_gsi);
        hk::log_hex(hk::LogLevel::Info, "SELFTEST IRQ1 GSI", irq1_gsi);
        hk::log_hex(hk::LogLevel::Info, "IRQ PIC remaps", irq_stats_after.pic_remaps);
        hk::log_hex(hk::LogLevel::Info, "IRQ mask updates", irq_stats_after.mask_updates);
        hk::log_hex(hk::LogLevel::Info, "IRQ route prepares", irq_stats_after.ioapic_route_prepares);
        hk::log_hex(hk::LogLevel::Info, "IRQ route match checks", irq_stats_after.ioapic_route_match_checks);
        hk::log_hex(hk::LogLevel::Info, "IRQ route match successes", irq_stats_after.ioapic_route_match_successes);
        if (ok) hk::log(hk::LogLevel::Info, "SELFTEST IRQ routing PASS");
    }

    hk::log(hk::LogLevel::Info, "SELFTEST PCI begin");
    bool pci_resources_sane = false;
    bool pci_command_sane = false;
    bool pci_bindings_sane = hk::pci::registry().driver_binding_count() > 0;
    const auto* pci_devices = hk::pci::registry().devices();
    for (uint32_t i = 0; i < hk::pci::registry().count(); ++i) {
        if (hk::pci::registry().command_for(pci_devices[i]) == pci_devices[i].command &&
            hk::pci::registry().status_for(pci_devices[i]) == pci_devices[i].status) {
            pci_command_sane = true;
        }
        for (uint8_t bar = 0; bar < pci_devices[i].bar_count; ++bar) {
            uint64_t size = pci_devices[i].bars[bar].size;
            if (size != 0 && size <= (256ull * 1024 * 1024)) pci_resources_sane = true;
            if (size > (4ull * 1024 * 1024 * 1024)) pci_resources_sane = false;
        }
        if (pci_devices[i].bar_count > 0 && hk::pci::registry().required_command_bits(pci_devices[i], false) == 0) {
            pci_resources_sane = false;
        }
    }
    bool pci_ecam_sane = !hk::pci::registry().ecam_available() || hk::pci::registry().ecam_verified_devices() > 0;
    bool pci_config_probe_sane = hk::pci::registry().config_probe_count() >= hk::pci::registry().count() &&
        hk::pci::registry().config_probe_failures() == 0 &&
        hk::pci::registry().config_ecam_mismatches() == 0 &&
        hk::pci::registry().malformed_config_rejects() >= 4;
    bool pci_config_path_sane = !hk::pci::registry().ecam_available() ||
        (hk::pci::registry().preferred_ecam_reads() >= hk::pci::registry().count() &&
         hk::pci::registry().legacy_config_reads() == 0 &&
         hk::pci::registry().ecam_fallback_reads() == 0 &&
         hk::pci::registry().preferred_ecam_writes() > 0 &&
         hk::pci::registry().legacy_config_writes() == 0 &&
         hk::pci::registry().ecam_write_fallbacks() == 0);
    bool pci_command_enable_sane = hk::pci::registry().command_enable_attempts() > 0 &&
        hk::pci::registry().command_enable_successes() == hk::pci::registry().command_enable_attempts();
    const auto* pci_bindings = hk::pci::registry().driver_bindings();
    for (uint32_t i = 0; i < hk::pci::registry().driver_binding_count(); ++i) {
        const auto& binding = pci_bindings[i];
        const auto* device = hk::pci::registry().binding_device(binding);
        if (!device ||
            binding.device_index >= hk::pci::registry().count() ||
            binding.kind == hk::pci::DriverKind::Unknown ||
            binding.kind == hk::pci::DriverKind::Bridge ||
            device->driver_kind != binding.kind ||
            binding.name == nullptr) {
            pci_bindings_sane = false;
        }
        if ((binding.kind == hk::pci::DriverKind::Ahci || binding.kind == hk::pci::DriverKind::E1000) &&
            (binding.required_command_bits & hk::pci::CommandBusMaster) == 0) {
            pci_bindings_sane = false;
        }
        if (device && device->bar_count > 0 && binding.required_command_bits == 0) {
            pci_bindings_sane = false;
        }
    }
    if (hk::pci::registry().count() == 0 ||
        hk::pci::registry().scanned_buses() != 256 ||
        hk::pci::registry().bridge_devices() == 0 ||
        hk::pci::registry().display_controllers() == 0 ||
        hk::pci::registry().mmio_bar_count() == 0 ||
        hk::pci::registry().driver_candidate_count() == 0 ||
        hk::pci::registry().ahci_candidates() == 0 ||
        hk::pci::registry().e1000_candidates() == 0 ||
        hk::pci::registry().vga_candidates() == 0 ||
        hk::pci::registry().ahci_bindings() == 0 ||
        hk::pci::registry().e1000_bindings() == 0 ||
        hk::pci::registry().vga_bindings() == 0 ||
        !pci_resources_sane ||
        !pci_ecam_sane ||
        !pci_config_probe_sane ||
        !pci_config_path_sane ||
        !pci_command_sane ||
        !pci_bindings_sane ||
        !pci_command_enable_sane) {
        ok = fail("SELFTEST PCI FAIL");
    } else {
        hk::log_hex(hk::LogLevel::Info, "PCI selftest preferred ECAM reads", hk::pci::registry().preferred_ecam_reads());
        hk::log_hex(hk::LogLevel::Info, "PCI selftest preferred ECAM writes", hk::pci::registry().preferred_ecam_writes());
        hk::log_hex(hk::LogLevel::Info, "PCI selftest legacy config writes", hk::pci::registry().legacy_config_writes());
        hk::log_hex(hk::LogLevel::Info, "PCI selftest command enable attempts", hk::pci::registry().command_enable_attempts());
        hk::log_hex(hk::LogLevel::Info, "PCI selftest command enable successes", hk::pci::registry().command_enable_successes());
        hk::log(hk::LogLevel::Info, "SELFTEST PCI PASS");
    }

    hk::log(hk::LogLevel::Info, "SELFTEST driver manager begin");
    auto driver_stats = hk::drivers::driver_manager().stats();
    bool driver_devices_sane = hk::drivers::driver_manager().count() >= 2 &&
        hk::drivers::driver_manager().started_count() >= 2 &&
        hk::drivers::driver_manager().failed_count() == 0 &&
        hk::drivers::driver_manager().device_count() == hk::pci::registry().driver_binding_count() &&
        driver_stats.registered_drivers == hk::drivers::driver_manager().count() &&
        driver_stats.start_attempts == hk::drivers::driver_manager().count() &&
        driver_stats.start_successes == hk::drivers::driver_manager().started_count() &&
        driver_stats.start_failures == hk::drivers::driver_manager().failed_count() &&
        driver_stats.import_passes != 0 &&
        driver_stats.imported_devices == hk::drivers::driver_manager().device_count() &&
        driver_stats.ahci_devices == hk::pci::registry().ahci_bindings() &&
        driver_stats.e1000_devices == hk::pci::registry().e1000_bindings() &&
        driver_stats.vga_devices == hk::pci::registry().vga_bindings() &&
        driver_stats.bus_master_required_devices >= 2 &&
        (driver_stats.command_bits_union & hk::pci::CommandMemorySpace) != 0 &&
        (driver_stats.command_bits_union & hk::pci::CommandBusMaster) != 0;
    const auto* driver_devices = hk::drivers::driver_manager().devices();
    for (uint64_t i = 0; i < hk::drivers::driver_manager().device_count(); ++i) {
        const auto& device = driver_devices[i];
        if (device.driver_name == nullptr ||
            device.kind == hk::pci::DriverKind::Unknown ||
            device.kind == hk::pci::DriverKind::Bridge ||
            device.state != hk::drivers::DeviceState::Bound) {
            driver_devices_sane = false;
        }
        if ((device.kind == hk::pci::DriverKind::Ahci || device.kind == hk::pci::DriverKind::E1000) &&
            (device.required_command_bits & hk::pci::CommandBusMaster) == 0) {
            driver_devices_sane = false;
        }
    }
    if (!driver_devices_sane) ok = fail("SELFTEST driver manager FAIL");
    else {
        hk::log_hex(hk::LogLevel::Info, "Driver selftest imported devices", driver_stats.imported_devices);
        hk::log_hex(hk::LogLevel::Info, "Driver selftest bus-master devices", driver_stats.bus_master_required_devices);
        hk::log_hex(hk::LogLevel::Info, "Driver selftest command bits union", driver_stats.command_bits_union);
        hk::log(hk::LogLevel::Info, "SELFTEST driver manager PASS");
    }

    hk::log(hk::LogLevel::Info, "SELFTEST AHCI begin");
    if (!hk::drivers::ahci::self_test()) ok = fail("SELFTEST AHCI FAIL");
    else hk::log(hk::LogLevel::Info, "SELFTEST AHCI PASS");

    hk::log(hk::LogLevel::Info, "SELFTEST block cache begin");
    if (!hk::block::self_test()) ok = fail("SELFTEST block cache FAIL");
    else hk::log(hk::LogLevel::Info, "SELFTEST block cache PASS");

    hk::log(hk::LogLevel::Info, "SELFTEST e1000 begin");
    if (!hk::drivers::e1000::self_test()) ok = fail("SELFTEST e1000 FAIL");
    else hk::log(hk::LogLevel::Info, "SELFTEST e1000 PASS");

    hk::log(hk::LogLevel::Info, "SELFTEST VGA begin");
    if (!hk::drivers::vga::self_test()) ok = fail("SELFTEST VGA FAIL");
    else hk::log(hk::LogLevel::Info, "SELFTEST VGA PASS");

    hk::log(hk::LogLevel::Info, "SELFTEST device inventory begin");
    if (!hk::drivers::inventory_self_test()) ok = fail("SELFTEST device inventory FAIL");
    else hk::log(hk::LogLevel::Info, "SELFTEST device inventory PASS");

    hk::log(hk::LogLevel::Info, "SELFTEST console begin");
    if (!hk::console().self_test()) ok = fail("SELFTEST console FAIL");
    else hk::log(hk::LogLevel::Info, "SELFTEST console PASS");

    hk::log(hk::LogLevel::Info, "SELFTEST terminal begin");
    if (!hk::terminal::self_test()) ok = fail("SELFTEST terminal FAIL");
    else hk::log(hk::LogLevel::Info, "SELFTEST terminal PASS");

    hk::log(hk::LogLevel::Info, "SELFTEST VFS begin");
    if (!hk::fs::self_test()) ok = fail("SELFTEST VFS FAIL");
    else hk::log(hk::LogLevel::Info, "SELFTEST VFS PASS");

    hk::log(hk::LogLevel::Info, "SELFTEST scheduler begin");
    auto* thread = hk::sched::scheduler().create_kernel_thread("selftest", test_thread_entry, nullptr);
    if (!thread || thread->state != hk::sched::ThreadState::Ready || !thread->kernel_stack_base) ok = fail("SELFTEST scheduler FAIL");
    else {
        hk::sched::ThreadSnapshot snapshot{};
        if (!hk::sched::scheduler().snapshot_thread(hk::sched::scheduler().thread_count() - 1, snapshot) ||
            snapshot.id != thread->id ||
            snapshot.state != hk::sched::ThreadState::Ready ||
            snapshot.affinity_mask == 0) {
            ok = fail("SELFTEST scheduler snapshot FAIL");
        }
        thread->state = hk::sched::ThreadState::Dead;
        hk::log(hk::LogLevel::Info, "SELFTEST scheduler PASS");
    }

    hk::log(hk::LogLevel::Info, "SELFTEST syscall begin");
    if (!hk::syscall::self_test()) ok = fail("SELFTEST syscall FAIL");
    else hk::log(hk::LogLevel::Info, "SELFTEST syscall PASS");

    hk::log(hk::LogLevel::Info, "SELFTEST process lifecycle begin");
    if (!hk::userspace::process_lifecycle_self_test()) ok = fail("SELFTEST process lifecycle FAIL");
    else hk::log(hk::LogLevel::Info, "SELFTEST process lifecycle PASS");

    hk::log(hk::LogLevel::Info, "SELFTEST launch context begin");
    if (!hk::userspace::launch_context_self_test()) ok = fail("SELFTEST launch context FAIL");
    else hk::log(hk::LogLevel::Info, "SELFTEST launch context PASS");

    hk::log(hk::LogLevel::Info, "SELFTEST file descriptors begin");
    if (!hk::userspace::file_descriptor_self_test()) ok = fail("SELFTEST file descriptors FAIL");
    else hk::log(hk::LogLevel::Info, "SELFTEST file descriptors PASS");

    hk::log(hk::LogLevel::Info, ok ? "SELFTEST overall PASS" : "SELFTEST overall FAIL");
    return ok;
}

} // namespace hk::tests
