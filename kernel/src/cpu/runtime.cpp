#include "hk/cpu/runtime.hpp"
#include "hk/apic/apic.hpp"
#include "hk/cpu/topology.hpp"
#include "hk/log.hpp"

namespace hk::cpu {

CpuRuntime& runtime() {
    static CpuRuntime state;
    return state;
}

void CpuRuntime::initialize_from_topology() {
    count_ = topology().cpu_count();
    if (count_ > 32) count_ = 32;
    for (uint32_t i = 0; i < count_; ++i) {
        const auto* cpu = topology().cpu(i);
        if (!cpu) continue;
        states_[i] = CpuRuntimeState{
            cpu->cpu_id,
            cpu->apic_id,
            cpu->bootstrap ? CpuRunState::Scheduler : CpuRunState::Offline,
            0,
            0,
            cpu->bootstrap,
            false,
            cpu->bootstrap,
            cpu->bootstrap,
        };
    }
    hk::log_hex(hk::LogLevel::Info, "CPU runtime records", count_);
    hk::log_hex(hk::LogLevel::Info, "CPU runtime current", current_cpu_id());
}

uint32_t CpuRuntime::current_cpu_id() const {
    uint32_t apic_id = hk::apic::local_apic().enabled() ? hk::apic::local_apic().id() : 0;
    for (uint32_t i = 0; i < count_; ++i) {
        if (states_[i].apic_id == apic_id) return states_[i].cpu_id;
    }
    return 0;
}

bool CpuRuntime::mark_scheduler_cpu(uint32_t cpu_id) {
    if (cpu_id >= count_) return false;
    states_[cpu_id].state = CpuRunState::Scheduler;
    return true;
}

bool CpuRuntime::mark_parked_by_apic(uint32_t apic_id) {
    for (uint32_t i = 0; i < count_; ++i) {
        if (states_[i].apic_id != apic_id) continue;
        states_[i].state = CpuRunState::Parked;
        hk::log_hex(hk::LogLevel::Info, "CPU runtime parked APIC", apic_id);
        return true;
    }
    return false;
}

bool CpuRuntime::mark_descriptors_ready(uint32_t cpu_id) {
    if (cpu_id >= count_) return false;
    states_[cpu_id].descriptors_ready = true;
    hk::log_hex(hk::LogLevel::Info, "CPU runtime descriptors ready", cpu_id);
    return true;
}

bool CpuRuntime::mark_local_apic_timer_ready(uint32_t cpu_id) {
    if (cpu_id >= count_) return false;
    states_[cpu_id].local_apic_timer_ready = true;
    hk::log_hex(hk::LogLevel::Info, "CPU runtime LAPIC timer ready", cpu_id);
    return true;
}

bool CpuRuntime::mark_bootstrap_work_done(uint32_t cpu_id) {
    if (cpu_id >= count_) return false;
    states_[cpu_id].bootstrap_work_done = true;
    hk::log_hex(hk::LogLevel::Info, "CPU runtime bootstrap work done", cpu_id);
    return true;
}

bool CpuRuntime::mark_ipi_work_done(uint32_t cpu_id) {
    if (cpu_id >= count_) return false;
    states_[cpu_id].ipi_work_done = true;
    hk::log_hex(hk::LogLevel::Info, "CPU runtime IPI work done", cpu_id);
    return true;
}

bool CpuRuntime::copy_state(uint32_t cpu_id, CpuRuntimeState& out) const {
    if (cpu_id >= count_) return false;
    out = states_[cpu_id];
    return true;
}

bool CpuRuntime::decorate_cpu_info(uint32_t cpu_id, hybrid::CpuInfo& out) const {
    if (cpu_id >= count_) return false;
    const CpuRuntimeState& state = states_[cpu_id];
    if (state.state == CpuRunState::Parked) out.flags |= hybrid::CpuInfoParked;
    if (state.state == CpuRunState::Scheduler || state.state == CpuRunState::Bootstrap) out.flags |= hybrid::CpuInfoScheduler;
    if (state.descriptors_ready) out.flags |= hybrid::CpuInfoDescriptorsReady;
    if (state.local_apic_timer_ready) out.flags |= hybrid::CpuInfoLocalApicTimerReady;
    if (state.bootstrap_work_done) out.flags |= hybrid::CpuInfoBootstrapWorkDone;
    if (state.ipi_work_done) out.flags |= hybrid::CpuInfoIpiWorkDone;
    return true;
}

void CpuRuntime::note_scheduler_tick() {
    uint32_t cpu_id = current_cpu_id();
    if (cpu_id >= count_) return;
    ++states_[cpu_id].interrupts;
    ++states_[cpu_id].scheduler_ticks;
}

} // namespace hk::cpu
