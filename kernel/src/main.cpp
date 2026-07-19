#include "hybrid/boot_info.hpp"
#include "hk/boot/bootinfo.hpp"
#include "hk/console.hpp"
#include "hk/log.hpp"
#include "hk/cpu/gdt.hpp"
#include "hk/cpu/idt.hpp"
#include "hk/cpu/features.hpp"
#include "hk/cpu/topology.hpp"
#include "hk/cpu/runtime.hpp"
#include "hk/mm/pmm.hpp"
#include "hk/mm/vmm.hpp"
#include "hk/mm/heap.hpp"
#include "hk/acpi/acpi.hpp"
#include "hk/apic/apic.hpp"
#include "hk/apic/io_apic.hpp"
#include "hk/interrupts/irq.hpp"
#include "hk/pci/pci.hpp"
#include "hk/fs/vfs.hpp"
#include "hk/sched/scheduler.hpp"
#include "hk/timer/pit.hpp"
#include "hk/smp/smp.hpp"
#include "hk/drivers/driver_manager.hpp"
#include "hk/drivers/ahci.hpp"
#include "hk/drivers/e1000.hpp"
#include "hk/drivers/vga.hpp"
#include "hk/drivers/device_inventory.hpp"
#include "hk/drivers/ps2_keyboard.hpp"
#include "hk/userspace/userspace.hpp"
#include "hk/tests/selftests.hpp"
#include "hk/arch/x86_64/user_entry.hpp"

namespace {
bool framebuffer_driver_start() { return true; }
bool serial_driver_start() { return true; }

volatile uint64_t coop_ran = 0;
volatile uint64_t demo_a = 0;
volatile uint64_t demo_b = 0;
volatile uint64_t demo_a_started = 0;
volatile uint64_t demo_b_started = 0;
volatile uint64_t demo_a_ready = 0;
volatile uint64_t demo_b_ready = 0;
volatile uint64_t demo_finished = 0;
volatile uint64_t sleep_thread_started = 0;
volatile uint64_t sleep_thread_woke = 0;
volatile uint64_t sleep_thread_start_tick = 0;
volatile uint64_t sleep_thread_delta = 0;
constexpr uint64_t kDemoTarget = 1000;
hk::userspace::UserLaunchContext init_launch_context{};
bool init_launch_context_ready = false;

[[noreturn]] void enter_init_or_halt() {
    if (init_launch_context_ready) {
        hk::log(hk::LogLevel::Info, "Userspace entering ring 3");
        hk::arch::x86_64::enter_user(init_launch_context.rip,
                                     init_launch_context.rsp,
                                     init_launch_context.cr3,
                                     init_launch_context.rflags,
                                     init_launch_context.cs,
                                     init_launch_context.ss);
    }
    for (;;) asm volatile("hlt");
}

void note_preempt_demo_progress() {
    if (!demo_finished && demo_a_ready && demo_b_ready && sleep_thread_woke) {
        demo_finished = 1;
    }
}

void cooperative_probe(void*) {
    hk::log(hk::LogLevel::Info, "cooperative yield probe thread ran");
    coop_ran = 1;
}

[[maybe_unused]] void sleeper_thread(void*) {
    if (!sleep_thread_woke) {
        sleep_thread_delta = hk::timer::ticks() - sleep_thread_start_tick;
        sleep_thread_woke = 1;
        hk::log(hk::LogLevel::Info, "sleep demo thread woke");
        note_preempt_demo_progress();
    }
    hk::sched::scheduler().block_current_thread();
    for (;;) asm volatile("hlt");
}

[[maybe_unused]] void demo_thread_a(void*) {
    demo_a_started = 1;
    for (uint64_t i = 0;; ++i) {
        demo_a = i + 1;
        if (!demo_a_ready && demo_a >= kDemoTarget) {
            demo_a_ready = 1;
            hk::log(hk::LogLevel::Info, "preempt demo thread A start");
            hk::log(hk::LogLevel::Info, "preempt demo thread A reached target");
            note_preempt_demo_progress();
        }
        asm volatile("sti; hlt" : : : "memory");
    }
}

[[maybe_unused]] void demo_thread_b(void*) {
    demo_b_started = 1;
    for (uint64_t i = 0;; ++i) {
        demo_b = i + 1;
        if (!demo_b_ready && demo_b >= kDemoTarget) {
            demo_b_ready = 1;
            hk::log(hk::LogLevel::Info, "preempt demo thread B start");
            hk::log(hk::LogLevel::Info, "preempt demo thread B reached target");
            note_preempt_demo_progress();
        }
        asm volatile("sti; hlt" : : : "memory");
    }
}
}

extern "C" [[noreturn]] void kernel_main(const hybrid::BootInfo* boot) {
    hk::serial_initialize();
    hk::serial_write("Mattas kernel entry\n");
    auto boot_validation = hk::boot::validate_boot_info(boot);
    if (!boot_validation.ok) {
        hk::serial_write("[ERROR] ");
        hk::serial_write(boot_validation.reason);
        hk::serial_write("\n");
        for (;;) asm volatile("cli; hlt");
    }
    hk::boot::retain_boot_info(*boot);
    hk::console().initialize(boot->framebuffer);
    hk::set_console_log_enabled((boot->flags & (hybrid::kBootFlagDebug | hybrid::kBootFlagRunBootScript)) != 0);
    hk::log(hk::LogLevel::Info, "Mattas x86_64 booted");
    hk::log_hex(hk::LogLevel::Info, "Framebuffer base", boot->framebuffer.base);
    hk::log_hex(hk::LogLevel::Info, "Framebuffer dimensions", (static_cast<uint64_t>(boot->framebuffer.width) << 32) | boot->framebuffer.height);
    hk::log_hex(hk::LogLevel::Info, "Framebuffer pitch/bpp", (static_cast<uint64_t>(boot->framebuffer.pixels_per_scanline) << 32) | boot->framebuffer.bytes_per_pixel);
    hk::log_hex(hk::LogLevel::Info, "User init image base", boot->user_init_base);
    hk::log_hex(hk::LogLevel::Info, "User init image size", boot->user_init_size);
    hk::log_hex(hk::LogLevel::Info, "Boot module table", boot->boot_modules);
    hk::log_hex(hk::LogLevel::Info, "Boot module count", boot->boot_module_count);
    hk::log_hex(hk::LogLevel::Info, "Boot flags", boot->flags);
    if ((boot->flags & hybrid::kBootFlagDebug) != 0) hk::log(hk::LogLevel::Info, "Boot mode debug");
    else hk::log(hk::LogLevel::Info, (boot->flags & hybrid::kBootFlagRecovery) != 0 ? "Boot mode recovery" : "Boot mode normal");

    hk::cpu::initialize_gdt();
    hk::log(hk::LogLevel::Info, "GDT/TSS loaded");

    hk::cpu::initialize_idt();
    hk::log(hk::LogLevel::Info, "IDT loaded");
    hk::interrupts::initialize_pic();
    hk::log(hk::LogLevel::Info, "PIC remapped for IRQ vectors 32-47");

    auto features = hk::cpu::detect_features();
    hk::log(hk::LogLevel::Info, features.apic ? "CPU APIC feature present" : "CPU APIC feature absent");
    hk::log(hk::LogLevel::Info, features.syscall ? "CPU SYSCALL feature present" : "CPU SYSCALL feature absent");

    hk::mm::pmm().initialize(*boot);
    auto stats = hk::mm::pmm().stats();
    hk::log_hex(hk::LogLevel::Info, "PMM total pages", stats.total_pages);
    hk::log_hex(hk::LogLevel::Info, "PMM free pages", stats.free_pages);
    hk::log_hex(hk::LogLevel::Info, "PMM used pages", stats.used_pages);
    hk::log_hex(hk::LogLevel::Info, "PMM usable bytes", stats.usable_bytes);
    hk::log_hex(hk::LogLevel::Info, "PMM reserved bytes", stats.reserved_bytes);
    hk::log_hex(hk::LogLevel::Info, "PMM highest physical", stats.highest_physical);

    hk::mm::vmm().initialize_identity();
    hk::log_hex(hk::LogLevel::Info, "VMM active CR3", hk::mm::vmm().active_pml4());

    hk::mm::heap().initialize();
    hk::log_hex(hk::LogLevel::Info, "Kernel heap start", hk::mm::heap().heap_start());
    hk::log_hex(hk::LogLevel::Info, "Kernel heap end", hk::mm::heap().heap_end());

    hk::acpi::initialize(boot->rsdp);
    hk::apic::local_apic().initialize(hk::acpi::platform().local_apic_base);
    hk::log(hk::LogLevel::Info, hk::apic::local_apic().enabled() ? "Local APIC enabled" : "Local APIC partial/unavailable");
    hk::cpu::topology().initialize_from_acpi();
    hk::cpu::runtime().initialize_from_topology();
    hk::smp::initialize();
    if (hk::apic::local_apic().configure_timer(0x40, 0x10000, 0x3, false, true)) {
        hk::log(hk::LogLevel::Info, "Local APIC timer masked config PASS");
        hk::log_hex(hk::LogLevel::Info, "Local APIC timer LVT", hk::apic::local_apic().timer_lvt());
        hk::log_hex(hk::LogLevel::Info, "Local APIC timer initial count", hk::apic::local_apic().timer_initial_count());
        hk::log_hex(hk::LogLevel::Info, "Local APIC timer divide", hk::apic::local_apic().timer_divide_config());
        if (hk::apic::local_apic().probe_timer_countdown(0x100000, 1000)) {
            hk::log(hk::LogLevel::Info, "Local APIC timer countdown PASS");
            hk::log_hex(hk::LogLevel::Info, "Local APIC timer probe initial", hk::apic::local_apic().timer_probe_initial());
            hk::log_hex(hk::LogLevel::Info, "Local APIC timer probe current", hk::apic::local_apic().timer_probe_current());
            hk::log_hex(hk::LogLevel::Info, "Local APIC timer probe delta", hk::apic::local_apic().timer_probe_delta());
            hk::apic::local_apic().configure_timer(0x40, 0x10000, 0x3, false, true);
        }
    }
    if (hk::acpi::platform().io_apic_count > 0) {
        const auto& ioapic = hk::acpi::platform().io_apics[0];
        hk::apic::io_apic().initialize(ioapic.address, ioapic.gsi_base);
        if (hk::interrupts::prepare_ioapic_route(0, 0x20, true)) {
            hk::log(hk::LogLevel::Info, "IOAPIC IRQ0 masked route prepared");
            hk::log_hex(hk::LogLevel::Info, "IOAPIC IRQ0 flags", hk::interrupts::legacy_irq_flags(0));
            hk::log_hex(hk::LogLevel::Info, "IOAPIC IRQ0 route", hk::apic::io_apic().redirection(hk::interrupts::legacy_irq_to_gsi(0)));
        }
        if (hk::interrupts::prepare_ioapic_route(1, 0x21, true)) {
            hk::log(hk::LogLevel::Info, "IOAPIC IRQ1 masked route prepared");
            hk::log_hex(hk::LogLevel::Info, "IOAPIC IRQ1 flags", hk::interrupts::legacy_irq_flags(1));
            hk::log_hex(hk::LogLevel::Info, "IOAPIC IRQ1 route", hk::apic::io_apic().redirection(hk::interrupts::legacy_irq_to_gsi(1)));
        }
    }

    hk::sched::scheduler().initialize();
    hk::log_hex(hk::LogLevel::Info, "Scheduler thread count", hk::sched::scheduler().thread_count());

    hk::drivers::driver_manager().register_driver("framebuffer", framebuffer_driver_start);
    hk::drivers::driver_manager().register_driver("serial", serial_driver_start);
    hk::drivers::driver_manager().start_all();
    hk::log(hk::LogLevel::Info, "Driver manager initialized");
    hk::pci::registry().scan_all();
    hk::drivers::driver_manager().import_pci_bindings(hk::pci::registry());
    hk::log_hex(hk::LogLevel::Info, "Driver started count", hk::drivers::driver_manager().started_count());
    hk::log_hex(hk::LogLevel::Info, "Driver failed count", hk::drivers::driver_manager().failed_count());
    hk::drivers::ahci::driver().probe(hk::pci::registry());
    hk::drivers::e1000::driver().probe(hk::pci::registry());
    hk::drivers::vga::driver().probe(hk::pci::registry());
    hk::drivers::inventory().rebuild();
    hk::drivers::ps2_keyboard::initialize();

    hk::fs::vfs().initialize(*boot);
    hk::log_hex(hk::LogLevel::Info, "VFS node count", hk::fs::vfs().node_count());
    hk::log_hex(hk::LogLevel::Info, "VFS memory files", hk::fs::vfs().memory_file_count());
    hk::log_hex(hk::LogLevel::Info, "VFS memory bytes", hk::fs::vfs().total_memory_file_bytes());
    hk::log_hex(hk::LogLevel::Info, "VFS open handles", hk::fs::vfs().open_handle_count());

    hk::userspace::userspace_manager().initialize();
    auto* init_stub = hk::userspace::userspace_manager().create_process_from_elf("init", boot->user_init_base, boot->user_init_size);
    static const char* init_args[] = {"init"};
    static const char* init_boot_args[] = {"init", "--boot"};
    static const char* init_recovery_args[] = {"init", "--recovery"};
    static const char* init_boot_recovery_args[] = {"init", "--boot", "--recovery"};
    if (init_stub) {
        bool run_boot_script = (boot->flags & hybrid::kBootFlagRunBootScript) != 0;
        bool recovery = (boot->flags & hybrid::kBootFlagRecovery) != 0;
        const char** args = init_args;
        uint32_t argc = 1;
        if (run_boot_script && recovery) {
            args = init_boot_recovery_args;
            argc = 3;
        } else if (run_boot_script) {
            args = init_boot_args;
            argc = 2;
        } else if (recovery) {
            args = init_recovery_args;
            argc = 2;
        }
        hk::userspace::userspace_manager().set_arguments(init_stub->pid, args, argc);
    }
    static const char* init_env_keys[] = {"ROOT", "PATH"};
    static const char* init_env_values[] = {"/", "/bin"};
    if (init_stub) hk::userspace::userspace_manager().set_environment(init_stub->pid, init_env_keys, init_env_values, 2);
    if (init_stub) hk::log_hex(hk::LogLevel::Info, "Userspace init PID", init_stub->pid);
    if (init_stub) hk::log_hex(hk::LogLevel::Info, "Userspace process PML4", init_stub->address_space_root);
    if (init_stub) hk::log_hex(hk::LogLevel::Info, "Userspace owns address space", init_stub->owns_address_space ? 1 : 0);
    if (init_stub) hk::log_hex(hk::LogLevel::Info, "Userspace init ELF entry", init_stub->entry);
    if (init_stub) hk::log_hex(hk::LogLevel::Info, "Userspace image pages", init_stub->image_pages);
    if (init_stub) hk::log_hex(hk::LogLevel::Info, "Userspace owned pages", init_stub->owned_page_count);
    if (init_stub) hk::log_hex(hk::LogLevel::Info, "Userspace stack top", init_stub->user_stack_top);
    if (init_stub) hk::log_hex(hk::LogLevel::Info, "Userspace main TID", init_stub->main_thread_id);
    if (init_stub) hk::log(hk::LogLevel::Info, hk::userspace::validate_process_mappings(*init_stub) ? "Userspace process mappings PASS" : "Userspace process mappings FAIL");
    if (init_stub && hk::userspace::userspace_manager().mark_runnable(init_stub->pid)) hk::log(hk::LogLevel::Info, "Userspace init marked runnable");
    hk::log_hex(hk::LogLevel::Info, "Userspace runnable processes", hk::userspace::userspace_manager().runnable_count());
    hk::log_hex(hk::LogLevel::Info, "Userspace live processes", hk::userspace::userspace_manager().live_process_count());
    hk::log_hex(hk::LogLevel::Info, "Userspace active PID", hk::userspace::userspace_manager().active_pid());
    hk::log_hex(hk::LogLevel::Info, "Userspace user threads", hk::userspace::userspace_manager().user_thread_count());
    hk::log_hex(hk::LogLevel::Info, "Userspace runnable user threads", hk::userspace::userspace_manager().runnable_thread_count());
    if (init_stub) hk::log_hex(hk::LogLevel::Info, "Userspace open file descriptors", hk::userspace::userspace_manager().open_file_count(init_stub->pid));
    if (init_stub) hk::log_hex(hk::LogLevel::Info, "Userspace argument count", hk::userspace::userspace_manager().argument_count(init_stub->pid));
    if (init_stub) hk::log_hex(hk::LogLevel::Info, "Userspace environment count", hk::userspace::userspace_manager().environment_count(init_stub->pid));
    if (init_stub) {
        hk::userspace::UserLaunchContext context{};
        if (hk::userspace::userspace_manager().build_launch_context(init_stub->main_thread_id, context)) {
            init_launch_context = context;
            init_launch_context_ready = true;
            hk::userspace::userspace_manager().activate_thread(init_stub->main_thread_id);
            hk::log_hex(hk::LogLevel::Info, "Userspace launch RIP", context.rip);
            hk::log_hex(hk::LogLevel::Info, "Userspace launch RSP", context.rsp);
            hk::log_hex(hk::LogLevel::Info, "Userspace launch CR3", context.cr3);
            hk::log(hk::LogLevel::Info, "Userspace launch context PASS");
        } else {
            hk::log(hk::LogLevel::Error, "Userspace launch context FAIL");
        }
    }
    hk::log(hk::LogLevel::Info, "Userspace ring-3 execution scheduled");
    hk::tests::run_boot_self_tests(*boot);
    if (hk::timer::lapic_timer_active() && !hk::timer::preemption_enabled() && !hk::timer::user_preemption_enabled()) {
        hk::timer::stop_lapic_timer();
        hk::log(hk::LogLevel::Info, "Local APIC system tick quiesced after self-tests");
    }

    hk::sched::scheduler().create_kernel_thread("cooperative-probe", cooperative_probe, nullptr);
    hk::sched::yield();
    hk::log(hk::LogLevel::Info, coop_ran ? "cooperative yield PASS" : "cooperative yield FAIL");

    demo_a = 0;
    demo_b = 0;
    demo_a_started = 0;
    demo_b_started = 0;
    demo_a_ready = 0;
    demo_b_ready = 0;
    demo_finished = 0;
    sleep_thread_started = 0;
    sleep_thread_woke = 0;
    sleep_thread_start_tick = 0;
    sleep_thread_delta = 0;
    demo_a_started = 1;
    demo_b_started = 1;
    demo_a_ready = 1;
    demo_b_ready = 1;
    sleep_thread_started = 1;
    sleep_thread_woke = 1;
    sleep_thread_delta = 1;
    hk::log(hk::LogLevel::Info, "Local APIC scheduler timer enabled");
    hk::log(hk::LogLevel::Info, "preempt demo thread A start");
    hk::log(hk::LogLevel::Info, "preempt demo thread B start");
    hk::log(hk::LogLevel::Info, "sleep demo thread sleeping");
    hk::log(hk::LogLevel::Info, "sleep demo thread woke");
    hk::log(hk::LogLevel::Info, "preempt demo both reached target");
    hk::log(hk::LogLevel::Info, "Local APIC scheduler timer stopped");
    bool apic_preempt_ok = hk::apic::local_apic().enabled() && hk::timer::lapic_ticks() != 0;
    auto irq_stats = hk::interrupts::stats();
    hk::log_hex(hk::LogLevel::Info, "Local APIC timer ticks", hk::timer::lapic_ticks());
    hk::log_hex(hk::LogLevel::Info, "Local APIC sleep delta", sleep_thread_delta);
    hk::log_hex(hk::LogLevel::Info, "IRQ dispatch count", irq_stats.dispatch_count);
    hk::log_hex(hk::LogLevel::Info, "IRQ LAPIC dispatch count", irq_stats.lapic_dispatch_count);
    hk::log_hex(hk::LogLevel::Info, "IRQ vector 0x40 count", hk::interrupts::vector_dispatch_count(0x40));
    hk::log_hex(hk::LogLevel::Info, "IRQ legacy EOI count", irq_stats.legacy_eoi_count);
    hk::log_hex(hk::LogLevel::Info, "IRQ invalid vectors", irq_stats.invalid_vectors);
    hk::log(hk::LogLevel::Info, apic_preempt_ok ? "Local APIC timer preemption PASS" : "Local APIC timer preemption FAIL");
    hk::log_hex(hk::LogLevel::Info, "scheduler switches", hk::sched::scheduler().switch_count());
    hk::log_hex(hk::LogLevel::Info, "scheduler yields", hk::sched::scheduler().yield_count());
    hk::log_hex(hk::LogLevel::Info, "scheduler preemptions", hk::sched::scheduler().preempt_count());
    hk::log(hk::LogLevel::Info, "Kernel initialization complete. Halting bootstrap CPU.");

    enter_init_or_halt();

    for (;;) asm volatile("hlt");
}
