#pragma once
#include <stdint.h>
#include "hybrid/syscall.hpp"

namespace hk::cpu {

constexpr uint32_t kMaxCpus = 32;

struct CpuState {
    uint32_t cpu_id;
    uint32_t apic_id;
    uint32_t acpi_processor_id;
    bool enabled;
    bool online;
    bool bootstrap;
    bool startup_attempted;
};

class CpuTopology {
public:
    void initialize_from_acpi();
    uint32_t cpu_count() const { return cpu_count_; }
    uint32_t online_count() const;
    uint32_t startup_attempt_count() const;
    uint32_t current_cpu_id() const { return bootstrap_cpu_id_; }
    const CpuState* cpu(uint32_t index) const;
    bool mark_startup_attempted_by_apic(uint32_t apic_id);
    bool mark_online_by_apic(uint32_t apic_id);
    bool copy_info(uint32_t index, hybrid::CpuInfo& out) const;
private:
    CpuState cpus_[kMaxCpus]{};
    uint32_t cpu_count_ = 0;
    uint32_t bootstrap_cpu_id_ = 0;
};

CpuTopology& topology();

} // namespace hk::cpu
