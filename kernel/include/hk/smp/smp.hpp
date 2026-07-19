#pragma once
#include <stdint.h>

namespace hk::smp {

void initialize();
uint32_t init_ipi_attempts();
uint32_t init_ipi_delivered();
uint32_t startup_ipi_delivered();
uint32_t ap_checkins();
uint32_t ipi_work_completed();
uint32_t queued_work_completed();
uint32_t work_counter(uint32_t cpu_id);
bool queue_kernel_work(uint32_t cpu_id);
uint32_t tlb_shootdown_completed();
uint32_t tlb_shootdown_counter(uint32_t cpu_id);
bool queue_tlb_shootdown(uint32_t cpu_id, uint64_t virt);
uint32_t shootdown_remote_tlbs(uint64_t virt);
void handle_ipi();

} // namespace hk::smp
