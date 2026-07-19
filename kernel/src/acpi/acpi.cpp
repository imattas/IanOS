#include "hk/acpi/acpi.hpp"
#include "hk/log.hpp"

namespace hk::acpi {

namespace {

struct [[gnu::packed]] Rsdp {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
};

struct [[gnu::packed]] SdtHeader {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};

struct [[gnu::packed]] MadtHeader {
    SdtHeader header;
    uint32_t local_apic_address;
    uint32_t flags;
};

struct [[gnu::packed]] McfgHeader {
    SdtHeader header;
    uint64_t reserved;
};

struct [[gnu::packed]] McfgEntry {
    uint64_t base;
    uint16_t segment;
    uint8_t start_bus;
    uint8_t end_bus;
    uint32_t reserved;
};

PlatformInfo info{};
AcpiDiagnostics diag{};

bool memeq(const char* a, const char* b, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) if (a[i] != b[i]) return false;
    return true;
}

uint8_t sum_bytes(const void* ptr, uint32_t length) {
    auto* bytes = static_cast<const uint8_t*>(ptr);
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; ++i) sum = static_cast<uint8_t>(sum + bytes[i]);
    return sum;
}

bool checksum_ok(const void* ptr, uint32_t length) {
    return sum_bytes(ptr, length) == 0;
}

bool sdt_valid(const SdtHeader* h, const char* sig = nullptr, bool count_diagnostics = false) {
    if (!h || h->length < sizeof(SdtHeader)) {
        if (count_diagnostics) ++diag.malformed_tables;
        return false;
    }
    if (sig && !memeq(h->signature, sig, 4)) return false;
    if (!checksum_ok(h, h->length)) {
        if (count_diagnostics) ++diag.checksum_failures;
        return false;
    }
    return true;
}

const SdtHeader* find_table(const SdtHeader* root, const char* sig, bool xsdt) {
    if (!sdt_valid(root)) return nullptr;
    uint32_t entry_size = xsdt ? 8 : 4;
    uint32_t count = (root->length - sizeof(SdtHeader)) / entry_size;
    diag.root_entry_count = count;
    auto* entries = reinterpret_cast<const uint8_t*>(root) + sizeof(SdtHeader);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t address = xsdt ? reinterpret_cast<const uint64_t*>(entries)[i] : reinterpret_cast<const uint32_t*>(entries)[i];
        auto* table = reinterpret_cast<const SdtHeader*>(address);
        ++diag.tables_scanned;
        if (!sdt_valid(table, nullptr, true)) continue;
        ++diag.tables_valid;
        if (memeq(table->signature, sig, 4)) return table;
    }
    return nullptr;
}

void parse_madt(const MadtHeader* madt) {
    info.madt = reinterpret_cast<uint64_t>(madt);
    info.local_apic_base = madt->local_apic_address;
    auto* ptr = reinterpret_cast<const uint8_t*>(madt) + sizeof(MadtHeader);
    auto* end = reinterpret_cast<const uint8_t*>(madt) + madt->header.length;
    while (ptr + 2 <= end) {
        uint8_t type = ptr[0];
        uint8_t length = ptr[1];
        if (length < 2 || ptr + length > end) {
            ++diag.madt_malformed_entries;
            break;
        }
        ++diag.madt_entries;
        switch (type) {
        case 0:
            if (length >= 8 && info.cpu_count < 32) {
                uint32_t flags = *reinterpret_cast<const uint32_t*>(ptr + 4);
                info.cpus[info.cpu_count++] = CpuInfo{ptr[2], ptr[3], (flags & 1u) != 0};
                ++diag.madt_lapic_entries;
            }
            break;
        case 1:
            if (length >= 12 && info.io_apic_count < 8) {
                auto address = *reinterpret_cast<const uint32_t*>(ptr + 4);
                auto gsi = *reinterpret_cast<const uint32_t*>(ptr + 8);
                info.io_apics[info.io_apic_count++] = IoApicInfo{ptr[2], address, gsi};
                ++diag.madt_ioapic_entries;
            }
            break;
        case 2:
            if (length >= 10 && info.override_count < 16) {
                auto gsi = *reinterpret_cast<const uint32_t*>(ptr + 4);
                auto flags = *reinterpret_cast<const uint16_t*>(ptr + 8);
                info.overrides[info.override_count++] = InterruptOverride{ptr[2], ptr[3], gsi, flags};
                ++diag.madt_override_entries;
            }
            break;
        case 5:
            if (length >= 12) {
                info.local_apic_base = static_cast<uint32_t>(*reinterpret_cast<const uint64_t*>(ptr + 4));
                ++diag.madt_lapic_address_overrides;
            }
            break;
        default:
            break;
        }
        ptr += length;
    }
}

void parse_mcfg(const McfgHeader* mcfg) {
    info.mcfg = reinterpret_cast<uint64_t>(mcfg);
    if (!mcfg || mcfg->header.length < sizeof(McfgHeader)) return;
    uint32_t count = (mcfg->header.length - sizeof(McfgHeader)) / sizeof(McfgEntry);
    diag.mcfg_entries = count;
    auto* entries = reinterpret_cast<const McfgEntry*>(reinterpret_cast<const uint8_t*>(mcfg) + sizeof(McfgHeader));
    for (uint32_t i = 0; i < count && info.ecam_region_count < 8; ++i) {
        if (entries[i].base == 0 || entries[i].end_bus < entries[i].start_bus) {
            ++diag.ecam_regions_rejected;
            continue;
        }
        bool overlaps = false;
        for (uint32_t j = 0; j < info.ecam_region_count; ++j) {
            const auto& existing = info.ecam_regions[j];
            if (existing.segment != entries[i].segment) continue;
            bool disjoint = entries[i].end_bus < existing.start_bus || entries[i].start_bus > existing.end_bus;
            if (!disjoint) {
                overlaps = true;
                break;
            }
        }
        if (overlaps) {
            ++diag.ecam_overlaps_rejected;
            ++diag.ecam_regions_rejected;
            continue;
        }
        info.ecam_regions[info.ecam_region_count++] = EcamInfo{
            entries[i].base,
            entries[i].segment,
            entries[i].start_bus,
            entries[i].end_bus,
        };
        ++diag.ecam_regions_validated;
    }
}

} // namespace

bool initialize(uint64_t rsdp_physical) {
    info = {};
    diag = {};
    if (rsdp_physical == 0) {
        hk::log(hk::LogLevel::Warn, "ACPI RSDP not provided");
        return false;
    }
    diag.rsdp_present = true;
    auto* rsdp = reinterpret_cast<const Rsdp*>(rsdp_physical);
    diag.rsdp_signature_valid = memeq(rsdp->signature, "RSD PTR ", 8);
    diag.rsdp_base_checksum_valid = checksum_ok(rsdp, 20);
    if (!diag.rsdp_signature_valid || !diag.rsdp_base_checksum_valid) {
        hk::log(hk::LogLevel::Error, "ACPI RSDP checksum/signature failed");
        return false;
    }
    diag.rsdp_extended_checksum_valid = rsdp->revision < 2 || (rsdp->length >= sizeof(Rsdp) && checksum_ok(rsdp, rsdp->length));
    bool xsdt = rsdp->revision >= 2 && rsdp->xsdt_address != 0 && diag.rsdp_extended_checksum_valid;
    diag.xsdt_used = xsdt;
    diag.rsdt_used = !xsdt;
    info.revision = rsdp->revision;
    info.root_table = xsdt ? rsdp->xsdt_address : rsdp->rsdt_address;
    auto* root = reinterpret_cast<const SdtHeader*>(info.root_table);
    if (!sdt_valid(root, xsdt ? "XSDT" : "RSDT", true)) {
        hk::log(hk::LogLevel::Error, "ACPI root table validation failed");
        return false;
    }
    auto* madt = reinterpret_cast<const MadtHeader*>(find_table(root, "APIC", xsdt));
    auto* mcfg = reinterpret_cast<const McfgHeader*>(find_table(root, "MCFG", xsdt));
    if (mcfg) parse_mcfg(mcfg);
    if (!madt) {
        hk::log(hk::LogLevel::Warn, "ACPI MADT not found");
        info.valid = true;
        return true;
    }
    parse_madt(madt);
    info.valid = true;
    hk::log_hex(hk::LogLevel::Info, "ACPI revision", info.revision);
    hk::log_hex(hk::LogLevel::Info, "ACPI root", info.root_table);
    hk::log_hex(hk::LogLevel::Info, "MADT", info.madt);
    hk::log_hex(hk::LogLevel::Info, "Local APIC base", info.local_apic_base);
    hk::log_hex(hk::LogLevel::Info, "MADT CPU count", info.cpu_count);
    hk::log_hex(hk::LogLevel::Info, "MADT IOAPIC count", info.io_apic_count);
    hk::log_hex(hk::LogLevel::Info, "MADT interrupt overrides", info.override_count);
    hk::log_hex(hk::LogLevel::Info, "MCFG", info.mcfg);
    hk::log_hex(hk::LogLevel::Info, "ECAM region count", info.ecam_region_count);
    hk::log_hex(hk::LogLevel::Info, "ACPI diagnostics RSDP flags",
        (diag.rsdp_present ? 1ull : 0ull) |
        (diag.rsdp_signature_valid ? 2ull : 0ull) |
        (diag.rsdp_base_checksum_valid ? 4ull : 0ull) |
        (diag.rsdp_extended_checksum_valid ? 8ull : 0ull) |
        (diag.xsdt_used ? 0x10ull : 0ull) |
        (diag.rsdt_used ? 0x20ull : 0ull));
    hk::log_hex(hk::LogLevel::Info, "ACPI diagnostics root entries", diag.root_entry_count);
    hk::log_hex(hk::LogLevel::Info, "ACPI diagnostics tables scanned", diag.tables_scanned);
    hk::log_hex(hk::LogLevel::Info, "ACPI diagnostics tables valid", diag.tables_valid);
    hk::log_hex(hk::LogLevel::Info, "ACPI diagnostics checksum failures", diag.checksum_failures);
    hk::log_hex(hk::LogLevel::Info, "ACPI diagnostics malformed tables", diag.malformed_tables);
    hk::log_hex(hk::LogLevel::Info, "ACPI diagnostics MADT entries", diag.madt_entries);
    hk::log_hex(hk::LogLevel::Info, "ACPI diagnostics MADT malformed", diag.madt_malformed_entries);
    hk::log_hex(hk::LogLevel::Info, "ACPI diagnostics MADT LAPIC entries", diag.madt_lapic_entries);
    hk::log_hex(hk::LogLevel::Info, "ACPI diagnostics MADT IOAPIC entries", diag.madt_ioapic_entries);
    hk::log_hex(hk::LogLevel::Info, "ACPI diagnostics MADT override entries", diag.madt_override_entries);
    hk::log_hex(hk::LogLevel::Info, "ACPI diagnostics MCFG entries", diag.mcfg_entries);
    hk::log_hex(hk::LogLevel::Info, "ACPI diagnostics ECAM validated", diag.ecam_regions_validated);
    hk::log_hex(hk::LogLevel::Info, "ACPI diagnostics ECAM rejected", diag.ecam_regions_rejected);
    hk::log_hex(hk::LogLevel::Info, "ACPI diagnostics ECAM overlaps rejected", diag.ecam_overlaps_rejected);
    for (uint32_t i = 0; i < info.ecam_region_count; ++i) {
        const auto& ecam = info.ecam_regions[i];
        hk::log_hex(hk::LogLevel::Info, "ECAM base", ecam.base);
        hk::log_hex(hk::LogLevel::Info, "ECAM segment/buses", (static_cast<uint64_t>(ecam.segment) << 16) | (static_cast<uint64_t>(ecam.start_bus) << 8) | ecam.end_bus);
    }
    return true;
}

const PlatformInfo& platform() {
    return info;
}

const AcpiDiagnostics& diagnostics() {
    return diag;
}

} // namespace hk::acpi
