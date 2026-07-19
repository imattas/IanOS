#pragma once
#include <stdint.h>

namespace hk::acpi {

struct CpuInfo { uint8_t processor_id; uint8_t apic_id; bool enabled; };
struct IoApicInfo { uint8_t id; uint32_t address; uint32_t gsi_base; };
struct InterruptOverride { uint8_t bus; uint8_t source; uint32_t gsi; uint16_t flags; };
struct EcamInfo { uint64_t base; uint16_t segment; uint8_t start_bus; uint8_t end_bus; };

struct AcpiDiagnostics {
    bool rsdp_present;
    bool rsdp_signature_valid;
    bool rsdp_base_checksum_valid;
    bool rsdp_extended_checksum_valid;
    bool xsdt_used;
    bool rsdt_used;
    uint32_t root_entry_count;
    uint32_t tables_scanned;
    uint32_t tables_valid;
    uint32_t checksum_failures;
    uint32_t malformed_tables;
    uint32_t madt_entries;
    uint32_t madt_malformed_entries;
    uint32_t madt_lapic_entries;
    uint32_t madt_ioapic_entries;
    uint32_t madt_override_entries;
    uint32_t madt_lapic_address_overrides;
    uint32_t mcfg_entries;
    uint32_t ecam_regions_validated;
    uint32_t ecam_regions_rejected;
    uint32_t ecam_overlaps_rejected;
};

struct PlatformInfo {
    bool valid;
    uint8_t revision;
    uint64_t root_table;
    uint64_t madt;
    uint32_t local_apic_base;
    CpuInfo cpus[32];
    uint32_t cpu_count;
    IoApicInfo io_apics[8];
    uint32_t io_apic_count;
    InterruptOverride overrides[16];
    uint32_t override_count;
    uint64_t mcfg;
    EcamInfo ecam_regions[8];
    uint32_t ecam_region_count;
};

bool initialize(uint64_t rsdp_physical);
const PlatformInfo& platform();
const AcpiDiagnostics& diagnostics();

} // namespace hk::acpi
