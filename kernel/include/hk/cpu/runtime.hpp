#pragma once
#include <stdint.h>
#include "hybrid/syscall.hpp"

namespace hk::cpu {

enum class CpuRunState : uint32_t {
    Offline = 0,
    Bootstrap = 1,
    Parked = 2,
    Scheduler = 3,
};

struct CpuRuntimeState {
    uint32_t cpu_id;
    uint32_t apic_id;
    CpuRunState state;
    uint64_t scheduler_ticks;
    uint64_t interrupts;
    bool descriptors_ready;
    bool local_apic_timer_ready;
    bool bootstrap_work_done;
    bool ipi_work_done;
};

class CpuRuntime {
public:
    void initialize_from_topology();
    uint32_t current_cpu_id() const;
    bool mark_scheduler_cpu(uint32_t cpu_id);
    bool mark_parked_by_apic(uint32_t apic_id);
    bool mark_descriptors_ready(uint32_t cpu_id);
    bool mark_local_apic_timer_ready(uint32_t cpu_id);
    bool mark_bootstrap_work_done(uint32_t cpu_id);
    bool mark_ipi_work_done(uint32_t cpu_id);
    bool copy_state(uint32_t cpu_id, CpuRuntimeState& out) const;
    bool decorate_cpu_info(uint32_t cpu_id, hybrid::CpuInfo& out) const;
    void note_scheduler_tick();
private:
    CpuRuntimeState states_[32]{};
    uint32_t count_ = 0;
};

CpuRuntime& runtime();

} // namespace hk::cpu
