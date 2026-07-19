#include "hk/cpu/topology.hpp"
#include "hk/acpi/acpi.hpp"
#include "hk/apic/apic.hpp"
#include "hk/log.hpp"

namespace hk::cpu {

CpuTopology& topology() {
    static CpuTopology topo;
    return topo;
}

void CpuTopology::initialize_from_acpi() {
    cpu_count_ = 0;
    bootstrap_cpu_id_ = 0;
    for (auto& cpu : cpus_) cpu = CpuState{};

    const auto& platform = hk::acpi::platform();
    uint32_t bootstrap_apic = hk::apic::local_apic().enabled() ? hk::apic::local_apic().id() : 0;
    for (uint32_t i = 0; i < platform.cpu_count && cpu_count_ < kMaxCpus; ++i) {
        const auto& acpi_cpu = platform.cpus[i];
        CpuState state{};
        state.cpu_id = cpu_count_;
        state.apic_id = acpi_cpu.apic_id;
        state.acpi_processor_id = acpi_cpu.processor_id;
        state.enabled = acpi_cpu.enabled;
        state.bootstrap = acpi_cpu.enabled && acpi_cpu.apic_id == bootstrap_apic;
        state.online = state.bootstrap;
        state.startup_attempted = false;
        if (state.bootstrap) bootstrap_cpu_id_ = state.cpu_id;
        cpus_[cpu_count_++] = state;
    }

    if (cpu_count_ == 0) {
        cpus_[0] = CpuState{0, bootstrap_apic, 0, true, true, true, false};
        cpu_count_ = 1;
        bootstrap_cpu_id_ = 0;
    } else {
        bool has_bootstrap = false;
        for (uint32_t i = 0; i < cpu_count_; ++i) {
            if (cpus_[i].bootstrap) {
                has_bootstrap = true;
                break;
            }
        }
        if (!has_bootstrap) {
            cpus_[0].bootstrap = true;
            cpus_[0].online = true;
            bootstrap_cpu_id_ = 0;
        }
    }

    hk::log_hex(hk::LogLevel::Info, "CPU topology count", cpu_count_);
    hk::log_hex(hk::LogLevel::Info, "CPU topology online", online_count());
    hk::log_hex(hk::LogLevel::Info, "CPU topology startup attempts", startup_attempt_count());
    hk::log_hex(hk::LogLevel::Info, "CPU topology bootstrap", bootstrap_cpu_id_);
    for (uint32_t i = 0; i < cpu_count_; ++i) {
        hk::log_hex(hk::LogLevel::Info, "CPU topology apic", (static_cast<uint64_t>(cpus_[i].cpu_id) << 32) | cpus_[i].apic_id);
    }
}

uint32_t CpuTopology::online_count() const {
    uint32_t total = 0;
    for (uint32_t i = 0; i < cpu_count_; ++i) if (cpus_[i].online) ++total;
    return total;
}

uint32_t CpuTopology::startup_attempt_count() const {
    uint32_t total = 0;
    for (uint32_t i = 0; i < cpu_count_; ++i) if (cpus_[i].startup_attempted) ++total;
    return total;
}

const CpuState* CpuTopology::cpu(uint32_t index) const {
    return index < cpu_count_ ? &cpus_[index] : nullptr;
}

bool CpuTopology::copy_info(uint32_t index, hybrid::CpuInfo& out) const {
    const CpuState* state = cpu(index);
    if (!state) return false;
    out.cpu_id = state->cpu_id;
    out.apic_id = state->apic_id;
    out.acpi_processor_id = state->acpi_processor_id;
    out.flags = 0;
    if (state->enabled) out.flags |= hybrid::CpuInfoEnabled;
    if (state->online) out.flags |= hybrid::CpuInfoOnline;
    if (state->bootstrap) out.flags |= hybrid::CpuInfoBootstrap;
    if (state->startup_attempted) out.flags |= hybrid::CpuInfoStartupAttempted;
    return true;
}

bool CpuTopology::mark_startup_attempted_by_apic(uint32_t apic_id) {
    for (uint32_t i = 0; i < cpu_count_; ++i) {
        if (cpus_[i].apic_id != apic_id) continue;
        cpus_[i].startup_attempted = true;
        return true;
    }
    return false;
}

bool CpuTopology::mark_online_by_apic(uint32_t apic_id) {
    for (uint32_t i = 0; i < cpu_count_; ++i) {
        if (cpus_[i].apic_id != apic_id) continue;
        cpus_[i].online = true;
        return true;
    }
    return false;
}

} // namespace hk::cpu
