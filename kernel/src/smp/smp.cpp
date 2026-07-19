#include "hk/smp/smp.hpp"
#include "hk/apic/apic.hpp"
#include "hk/cpu/gdt.hpp"
#include "hk/cpu/idt.hpp"
#include "hk/cpu/runtime.hpp"
#include "hk/cpu/topology.hpp"
#include "hk/lib/string.hpp"
#include "hk/log.hpp"
#include "hk/mm/pmm.hpp"
#include "hk/mm/vmm.hpp"
#include "hk/smp/ap_trampoline_bin.hpp"

extern "C" [[noreturn]] void smp_ap_park_entry() {
    extern volatile uint32_t smp_ap_entry_cpu_id;
    hk::cpu::CpuRuntimeState runtime_state{};
    bool descriptors_ready = hk::cpu::runtime().copy_state(smp_ap_entry_cpu_id, runtime_state) && runtime_state.descriptors_ready;
    if (descriptors_ready) {
        for (;;) asm volatile("sti; hlt" ::: "memory");
    }
    hk::cpu::load_prepared_cpu_gdt(smp_ap_entry_cpu_id);
    hk::cpu::initialize_idt();
    hk::cpu::runtime().mark_descriptors_ready(smp_ap_entry_cpu_id);
    hk::apic::local_apic().enable_current_cpu();
    if (hk::apic::local_apic().configure_timer(0x40, 0x20000, 0x3, false, true)) {
        hk::cpu::runtime().mark_local_apic_timer_ready(smp_ap_entry_cpu_id);
    }
    hk::cpu::runtime().mark_bootstrap_work_done(smp_ap_entry_cpu_id);
    for (;;) asm volatile("sti; hlt" ::: "memory");
}

namespace hk::smp {
namespace {
constexpr uint64_t kTrampolineBase = 0x7000;
constexpr uint64_t kTrampolineConfigBase = 0x8000;
constexpr uint64_t kTrampolineGdtBase = kTrampolineConfigBase + 0x40;
constexpr uint32_t kApStackPages = 4;
constexpr uint32_t kApCheckinPolls = 2000000;
constexpr uint32_t kApParkedMagic = 0xc0def00d;
constexpr uint32_t kApWorkQueueSize = 8;
constexpr uint32_t kApWorkCounted = 1;
constexpr uint32_t kApWorkInvalidatePage = 2;

struct [[gnu::packed]] TrampolineGdtr {
    uint16_t limit;
    uint32_t base;
};

struct [[gnu::packed]] TrampolineConfig {
    TrampolineGdtr gdtr;
    uint8_t reserved0[10];
    uint64_t cr3;
    uint64_t stack_top;
    uint64_t checkin_ptr;
    uint64_t state_ptr;
    uint64_t entry_ptr;
    uint8_t reserved1[8];
    uint64_t gdt[5];
};

uint32_t attempts = 0;
uint32_t delivered = 0;
uint32_t sipi_delivered = 0;
uint32_t ipi_completed = 0;
uint32_t queued_completed = 0;
uint32_t shootdown_completed = 0;
alignas(8) volatile uint32_t checkins = 0;
alignas(8) volatile uint32_t ap_state_magic[hk::cpu::kMaxCpus]{};
alignas(8) volatile uint32_t ap_work_tail[hk::cpu::kMaxCpus]{};
alignas(8) volatile uint32_t ap_work_done[hk::cpu::kMaxCpus]{};
alignas(8) volatile uint32_t ap_work_commands[hk::cpu::kMaxCpus][kApWorkQueueSize]{};
alignas(8) volatile uint64_t ap_work_args[hk::cpu::kMaxCpus][kApWorkQueueSize]{};
alignas(8) volatile uint32_t ap_work_counters[hk::cpu::kMaxCpus]{};
alignas(8) volatile uint32_t ap_tlb_shootdown_counters[hk::cpu::kMaxCpus]{};
extern "C" volatile uint32_t smp_ap_entry_cpu_id = 0;

void pause_delay(uint32_t loops) {
    for (uint32_t i = 0; i < loops; ++i) asm volatile("pause");
}

bool install_trampoline(uint32_t cpu_id, uint64_t stack_top) {
    if (trampoline_image::ap_trampoline_size > 4096) {
        hk::log_hex(hk::LogLevel::Error, "SMP trampoline too large", trampoline_image::ap_trampoline_size);
        return false;
    }
    auto* trampoline = reinterpret_cast<uint8_t*>(kTrampolineBase);
    memset(trampoline, 0, 4096);
    memcpy(trampoline, trampoline_image::ap_trampoline, trampoline_image::ap_trampoline_size);

    auto* config = reinterpret_cast<TrampolineConfig*>(kTrampolineConfigBase);
    memset(config, 0, sizeof(*config));
    config->gdtr.limit = static_cast<uint16_t>(sizeof(config->gdt) - 1);
    config->gdtr.base = static_cast<uint32_t>(kTrampolineGdtBase);
    config->cr3 = hk::mm::vmm().active_pml4();
    config->stack_top = stack_top;
    config->checkin_ptr = reinterpret_cast<uint64_t>(&checkins);
    config->state_ptr = reinterpret_cast<uint64_t>(&ap_state_magic[cpu_id]);
    config->entry_ptr = reinterpret_cast<uint64_t>(&smp_ap_park_entry);
    config->gdt[0] = 0;
    config->gdt[1] = 0x00cf9a000000ffffull;
    config->gdt[2] = 0x00cf92000000ffffull;
    config->gdt[3] = 0x00af9a000000ffffull;
    config->gdt[4] = 0x00af92000000ffffull;
    asm volatile("mfence" ::: "memory");
    return true;
}

bool wait_for_checkin(uint32_t before) {
    for (uint32_t i = 0; i < kApCheckinPolls; ++i) {
        if (checkins != before) return true;
        asm volatile("pause");
    }
    return false;
}

void patch_trampoline_for_long_reentry() {
    auto* trampoline = reinterpret_cast<uint8_t*>(kTrampolineBase);
    uint64_t entry = reinterpret_cast<uint64_t>(&smp_ap_park_entry);
    trampoline[0] = 0x48;
    trampoline[1] = 0xb8;
    for (uint32_t i = 0; i < 8; ++i) {
        trampoline[2 + i] = static_cast<uint8_t>((entry >> (i * 8)) & 0xff);
    }
    trampoline[10] = 0xff;
    trampoline[11] = 0xe0;
    for (uint32_t i = 12; i < 32; ++i) trampoline[i] = 0x90;
    asm volatile("mfence; wbinvd" ::: "memory");
}

bool enqueue_ap_work(uint32_t cpu_id, uint32_t apic_id, uint32_t command, uint64_t argument) {
    if (cpu_id >= hk::cpu::kMaxCpus) return false;
    uint32_t before = ap_work_done[cpu_id];
    uint32_t tail = ap_work_tail[cpu_id];
    if ((tail - before) >= kApWorkQueueSize) return false;
    uint32_t slot = tail % kApWorkQueueSize;
    ap_work_commands[cpu_id][slot] = command;
    ap_work_args[cpu_id][slot] = argument;
    ap_work_tail[cpu_id] = tail + 1;
    asm volatile("mfence" ::: "memory");
    if (!hk::apic::local_apic().send_fixed_ipi(static_cast<uint8_t>(apic_id), 0x41)) return false;
    for (uint32_t i = 0; i < 2000000; ++i) {
        if (ap_work_done[cpu_id] != before) return true;
        asm volatile("pause");
    }
    return false;
}

bool dispatch_control_ipi(uint32_t cpu_id, uint32_t apic_id) {
    return enqueue_ap_work(cpu_id, apic_id, kApWorkCounted, 0);
}
} // namespace

void initialize() {
    attempts = 0;
    delivered = 0;
    sipi_delivered = 0;
    ipi_completed = 0;
    queued_completed = 0;
    shootdown_completed = 0;
    checkins = 0;
    for (uint32_t i = 0; i < hk::cpu::kMaxCpus; ++i) {
        ap_state_magic[i] = 0;
        ap_work_tail[i] = 0;
        ap_work_done[i] = 0;
        ap_work_counters[i] = 0;
        ap_tlb_shootdown_counters[i] = 0;
        for (uint32_t slot = 0; slot < kApWorkQueueSize; ++slot) {
            ap_work_commands[i][slot] = 0;
            ap_work_args[i][slot] = 0;
        }
    }
    auto& topo = hk::cpu::topology();
    for (uint32_t i = 0; i < topo.cpu_count(); ++i) {
        const auto* cpu = topo.cpu(i);
        if (!cpu || !cpu->enabled || cpu->bootstrap || cpu->online) continue;
        ++attempts;
        uint64_t stack = hk::mm::pmm().allocate_contiguous(kApStackPages);
        if (stack == 0 ||
            !hk::cpu::prepare_cpu_gdt(cpu->cpu_id, stack + kApStackPages * hk::mm::kPageSize) ||
            !install_trampoline(cpu->cpu_id, stack + kApStackPages * hk::mm::kPageSize)) {
            hk::log_hex(hk::LogLevel::Warn, "SMP AP trampoline setup failed APIC", cpu->apic_id);
            continue;
        }
        smp_ap_entry_cpu_id = cpu->cpu_id;
        asm volatile("mfence" ::: "memory");
        uint32_t before = checkins;
        if (hk::apic::local_apic().send_init_ipi(static_cast<uint8_t>(cpu->apic_id))) {
            ++delivered;
            topo.mark_startup_attempted_by_apic(cpu->apic_id);
            hk::log_hex(hk::LogLevel::Info, "SMP INIT IPI delivered APIC", cpu->apic_id);
            pause_delay(100000);
            if (hk::apic::local_apic().send_startup_ipi(static_cast<uint8_t>(cpu->apic_id), static_cast<uint8_t>(kTrampolineBase >> 12))) {
                ++sipi_delivered;
                hk::log_hex(hk::LogLevel::Info, "SMP SIPI delivered APIC", cpu->apic_id);
            }
            pause_delay(200000);
            if (checkins == before) {
                if (hk::apic::local_apic().send_startup_ipi(static_cast<uint8_t>(cpu->apic_id), static_cast<uint8_t>(kTrampolineBase >> 12))) {
                    ++sipi_delivered;
                    hk::log_hex(hk::LogLevel::Info, "SMP SIPI delivered APIC", cpu->apic_id);
                }
            }
            if (wait_for_checkin(before) && ap_state_magic[cpu->cpu_id] == kApParkedMagic) {
                patch_trampoline_for_long_reentry();
                topo.mark_online_by_apic(cpu->apic_id);
                hk::cpu::runtime().mark_parked_by_apic(cpu->apic_id);
                hk::log_hex(hk::LogLevel::Info, "SMP AP long mode online APIC", cpu->apic_id);
                if (dispatch_control_ipi(cpu->cpu_id, cpu->apic_id)) {
                    ++ipi_completed;
                    ++queued_completed;
                    hk::log_hex(hk::LogLevel::Info, "SMP AP IPI work complete APIC", cpu->apic_id);
                } else {
                    hk::log_hex(hk::LogLevel::Warn, "SMP AP IPI work timeout APIC", cpu->apic_id);
                }
                if (dispatch_control_ipi(cpu->cpu_id, cpu->apic_id)) {
                    ++ipi_completed;
                    ++queued_completed;
                    hk::log_hex(hk::LogLevel::Info, "SMP AP queued work complete APIC", cpu->apic_id);
                } else {
                    hk::log_hex(hk::LogLevel::Warn, "SMP AP queued work timeout APIC", cpu->apic_id);
                }
                if (queue_tlb_shootdown(cpu->cpu_id, reinterpret_cast<uint64_t>(&checkins))) {
                    hk::log_hex(hk::LogLevel::Info, "SMP AP TLB shootdown complete APIC", cpu->apic_id);
                } else {
                    hk::log_hex(hk::LogLevel::Warn, "SMP AP TLB shootdown timeout APIC", cpu->apic_id);
                }
            } else {
                hk::log_hex(hk::LogLevel::Warn, "SMP AP checkin timeout APIC", cpu->apic_id);
            }
        } else {
            hk::log_hex(hk::LogLevel::Warn, "SMP INIT IPI delivery failed APIC", cpu->apic_id);
        }
    }
    hk::log_hex(hk::LogLevel::Info, "SMP startup attempts", attempts);
    hk::log_hex(hk::LogLevel::Info, "SMP INIT IPI delivered count", delivered);
    hk::log_hex(hk::LogLevel::Info, "SMP SIPI delivered count", sipi_delivered);
    hk::log_hex(hk::LogLevel::Info, "SMP AP checkins", checkins);
    hk::log_hex(hk::LogLevel::Info, "SMP AP IPI work completed", ipi_completed);
    hk::log_hex(hk::LogLevel::Info, "SMP AP queued work completed", queued_completed);
    hk::log_hex(hk::LogLevel::Info, "SMP AP TLB shootdown completed", shootdown_completed);
    hk::log_hex(hk::LogLevel::Info, "SMP online CPUs", topo.online_count());
}

uint32_t init_ipi_attempts() {
    return attempts;
}

uint32_t init_ipi_delivered() {
    return delivered;
}

uint32_t startup_ipi_delivered() {
    return sipi_delivered;
}

uint32_t ap_checkins() {
    return checkins;
}

uint32_t ipi_work_completed() {
    return ipi_completed;
}

uint32_t queued_work_completed() {
    return queued_completed;
}

uint32_t work_counter(uint32_t cpu_id) {
    if (cpu_id >= hk::cpu::kMaxCpus) return 0;
    return ap_work_counters[cpu_id];
}

bool queue_kernel_work(uint32_t cpu_id) {
    const auto* cpu = hk::cpu::topology().cpu(cpu_id);
    if (!cpu || !cpu->online || cpu->bootstrap) return false;
    if (!enqueue_ap_work(cpu_id, cpu->apic_id, kApWorkCounted, 0)) return false;
    ++ipi_completed;
    ++queued_completed;
    hk::log_hex(hk::LogLevel::Info, "SMP AP queued work complete APIC", cpu->apic_id);
    return true;
}

uint32_t tlb_shootdown_completed() {
    return shootdown_completed;
}

uint32_t tlb_shootdown_counter(uint32_t cpu_id) {
    if (cpu_id >= hk::cpu::kMaxCpus) return 0;
    return ap_tlb_shootdown_counters[cpu_id];
}

bool queue_tlb_shootdown(uint32_t cpu_id, uint64_t virt) {
    const auto* cpu = hk::cpu::topology().cpu(cpu_id);
    if (!cpu || !cpu->online || cpu->bootstrap || virt == 0) return false;
    if (!enqueue_ap_work(cpu_id, cpu->apic_id, kApWorkInvalidatePage, virt)) return false;
    ++ipi_completed;
    ++queued_completed;
    ++shootdown_completed;
    return true;
}

uint32_t shootdown_remote_tlbs(uint64_t virt) {
    if (virt == 0) return 0;
    uint32_t sent = 0;
    auto& topo = hk::cpu::topology();
    for (uint32_t i = 0; i < topo.cpu_count(); ++i) {
        const auto* cpu = topo.cpu(i);
        if (!cpu || !cpu->online || cpu->bootstrap) continue;
        if (queue_tlb_shootdown(cpu->cpu_id, virt)) ++sent;
    }
    if (sent != 0) hk::log_hex(hk::LogLevel::Info, "SMP remote TLB shootdowns", sent);
    return sent;
}

void handle_ipi() {
    uint32_t cpu_id = hk::cpu::runtime().current_cpu_id();
    if (cpu_id < hk::cpu::kMaxCpus) {
        while (ap_work_done[cpu_id] != ap_work_tail[cpu_id]) {
            uint32_t index = ap_work_done[cpu_id] % kApWorkQueueSize;
            uint32_t command = ap_work_commands[cpu_id][index];
            uint64_t argument = ap_work_args[cpu_id][index];
            if (command == kApWorkCounted) ap_work_counters[cpu_id] = ap_work_counters[cpu_id] + 1;
            if (command == kApWorkInvalidatePage && argument != 0) {
                hk::mm::vmm().invalidate_local_page(argument);
                ap_tlb_shootdown_counters[cpu_id] = ap_tlb_shootdown_counters[cpu_id] + 1;
            }
            ap_work_commands[cpu_id][index] = 0;
            ap_work_args[cpu_id][index] = 0;
            asm volatile("mfence" ::: "memory");
            ap_work_done[cpu_id] = ap_work_done[cpu_id] + 1;
        }
        hk::cpu::runtime().mark_ipi_work_done(cpu_id);
    }
    hk::apic::local_apic().eoi();
}

} // namespace hk::smp
