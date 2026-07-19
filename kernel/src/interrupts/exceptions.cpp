#include "hk/interrupts/exceptions.hpp"
#include "hk/console.hpp"
#include "hk/log.hpp"
#include "hk/interrupts/irq.hpp"
#include "hk/apic/apic.hpp"
#include "hk/cpu/runtime.hpp"
#include "hk/sched/scheduler.hpp"
#include "hk/smp/smp.hpp"
#include "hk/syscall/syscall.hpp"
#include "hk/timer/pit.hpp"
#include "hk/drivers/ps2_keyboard.hpp"
#include "hk/userspace/userspace.hpp"

extern "C" void pit_note_tick();
extern "C" void lapic_note_tick();

namespace hk::interrupts {

namespace {
uint64_t read_cr2() {
    uint64_t value;
    asm volatile("mov %%cr2, %0" : "=r"(value));
    return value;
}

void write_error_bit(const char* name, bool value) {
    hk::serial_write("  ");
    hk::serial_write(name);
    hk::serial_write(value ? "=yes\n" : "=no\n");
}

void write_cr3(uint64_t value) {
    asm volatile("mov %0, %%cr3" :: "r"(value) : "memory");
}

uint64_t user_preempt_switch_log_count = 0;

void save_user_registers(hk::userspace::UserThread& thread, const ExceptionFrame& frame) {
    thread.r15 = frame.r15;
    thread.r14 = frame.r14;
    thread.r13 = frame.r13;
    thread.r12 = frame.r12;
    thread.r11 = frame.r11;
    thread.r10 = frame.r10;
    thread.r9 = frame.r9;
    thread.r8 = frame.r8;
    thread.rbp = frame.rbp;
    thread.rdi = frame.rdi;
    thread.rsi = frame.rsi;
    thread.rdx = frame.rdx;
    thread.rcx = frame.rcx;
    thread.rbx = frame.rbx;
    thread.rax = frame.rax;
    thread.rflags = frame.rflags;
}

void restore_user_registers(ExceptionFrame& frame, const hk::userspace::UserThread& thread) {
    frame.r15 = thread.r15;
    frame.r14 = thread.r14;
    frame.r13 = thread.r13;
    frame.r12 = thread.r12;
    frame.r11 = thread.r11;
    frame.r10 = thread.r10;
    frame.r9 = thread.r9;
    frame.r8 = thread.r8;
    frame.rbp = thread.rbp;
    frame.rdi = thread.rdi;
    frame.rsi = thread.rsi;
    frame.rdx = thread.rdx;
    frame.rcx = thread.rcx;
    frame.rbx = thread.rbx;
    frame.rax = thread.rax;
}

bool yield_user_thread_from_frame(ExceptionFrame* frame) {
    if ((frame->cs & 3) == 0) return false;
    auto& users = hk::userspace::userspace_manager();
    uint64_t current_tid = users.active_tid();
    if (current_tid == 0) return false;
    if (!users.save_active_thread_frame(frame->rip, frame->rsp)) return false;
    if (auto* current = users.find_thread(current_tid)) save_user_registers(*current, *frame);
    if (!users.mark_active_thread_runnable()) return false;

    hk::userspace::UserLaunchContext next{};
    if (!users.select_next_runnable_thread(next) || next.tid == current_tid) {
        users.activate_thread(current_tid);
        return false;
    }
    if (!users.activate_thread(next.tid)) {
        users.activate_thread(current_tid);
        return false;
    }
    auto* next_thread = users.find_thread(next.tid);
    if (!next_thread) {
        users.activate_thread(current_tid);
        return false;
    }

    restore_user_registers(*frame, *next_thread);
    frame->rip = next.rip;
    frame->rsp = next.rsp;
    frame->rflags = next.rflags;
    frame->cs = next.cs;
    frame->ss = next.ss;
    write_cr3(next.cr3);
    hk::log_hex(hk::LogLevel::Info, "Userspace yield switched TID", next.tid);
    hk::log_hex(hk::LogLevel::Info, "Userspace yield switched PID", next.pid);
    return true;
}

bool block_user_pipe_syscall_from_frame(ExceptionFrame* frame, bool write_end, uint32_t fd) {
    if ((frame->cs & 3) == 0) return false;
    auto& users = hk::userspace::userspace_manager();
    uint64_t current_tid = users.active_tid();
    if (current_tid == 0) return false;
    uint32_t pipe_id = 0;
    if (!users.resolve_pipe_fd(users.active_pid(), fd, write_end, pipe_id)) return false;
    uint64_t syscall_buffer = frame->rsi;
    uint64_t syscall_size = frame->rdx;

    hk::userspace::UserLaunchContext next{};
    if (!users.select_next_runnable_thread(next) || next.tid == current_tid) return false;
    auto* next_thread = users.find_thread(next.tid);
    if (!next_thread) return false;

    frame->rax = 0;
    frame->rdx = hybrid::kSyscallErrorWouldBlock;
    if (!users.save_active_thread_frame(frame->rip, frame->rsp)) return false;
    if (auto* current = users.find_thread(current_tid)) save_user_registers(*current, *frame);
    if (!users.block_active_thread_on_pipe(pipe_id, write_end, fd, syscall_buffer, syscall_size)) return false;

    if (!users.activate_thread(next.tid)) {
        if (auto* current = users.find_thread(current_tid)) {
            current->state = hk::userspace::UserThreadState::Running;
            current->block_reason = hk::userspace::UserBlockReason::None;
            current->wait_pipe_id = 0;
            current->wait_process_id = 0;
            current->wait_fd = 0;
            current->wait_buffer = 0;
            current->wait_size = 0;
        }
        return false;
    }

    restore_user_registers(*frame, *next_thread);
    frame->rip = next.rip;
    frame->rsp = next.rsp;
    frame->rflags = next.rflags;
    frame->cs = next.cs;
    frame->ss = next.ss;
    write_cr3(next.cr3);
    hk::log_hex(hk::LogLevel::Info, write_end ? "Userspace pipe write blocked TID" : "Userspace pipe read blocked TID", current_tid);
    hk::log_hex(hk::LogLevel::Info, "Userspace pipe wake target TID", next.tid);
    return true;
}

bool block_user_wait_syscall_from_frame(ExceptionFrame* frame, uint64_t target_pid) {
    if ((frame->cs & 3) == 0 || target_pid == 0) return false;
    auto& users = hk::userspace::userspace_manager();
    uint64_t current_tid = users.active_tid();
    if (current_tid == 0 || !users.process_wait_would_block(users.active_pid(), target_pid)) return false;

    hk::userspace::UserLaunchContext next{};
    if (!users.select_next_runnable_thread(next) || next.tid == current_tid) return false;
    auto* next_thread = users.find_thread(next.tid);
    if (!next_thread) return false;

    frame->rax = 0;
    frame->rdx = hybrid::kSyscallErrorWouldBlock;
    if (!users.save_active_thread_frame(frame->rip, frame->rsp)) return false;
    if (auto* current = users.find_thread(current_tid)) save_user_registers(*current, *frame);
    if (!users.block_active_thread_on_process(target_pid)) return false;

    if (!users.activate_thread(next.tid)) {
        if (auto* current = users.find_thread(current_tid)) {
            current->state = hk::userspace::UserThreadState::Running;
            current->block_reason = hk::userspace::UserBlockReason::None;
            current->wait_pipe_id = 0;
            current->wait_process_id = 0;
            current->wait_fd = 0;
            current->wait_buffer = 0;
            current->wait_size = 0;
        }
        return false;
    }

    if (!users.update_user_preemption_gate()) return false;

    restore_user_registers(*frame, *next_thread);
    frame->rip = next.rip;
    frame->rsp = next.rsp;
    frame->rflags = next.rflags;
    frame->cs = next.cs;
    frame->ss = next.ss;
    write_cr3(next.cr3);
    hk::log_hex(hk::LogLevel::Info, "Userspace wait blocked TID", current_tid);
    hk::log_hex(hk::LogLevel::Info, "Userspace wait target PID", target_pid);
    hk::log_hex(hk::LogLevel::Info, "Userspace wait switch TID", next.tid);
    return true;
}

bool block_user_wait_any_syscall_from_frame(ExceptionFrame* frame) {
    if ((frame->cs & 3) == 0) return false;
    auto& users = hk::userspace::userspace_manager();
    uint64_t current_tid = users.active_tid();
    if (current_tid == 0 || !users.process_wait_any_would_block(users.active_pid())) return false;

    hk::userspace::UserLaunchContext next{};
    if (!users.select_next_runnable_thread(next) || next.tid == current_tid) return false;
    auto* next_thread = users.find_thread(next.tid);
    if (!next_thread) return false;

    frame->rax = 0;
    frame->rdx = hybrid::kSyscallErrorWouldBlock;
    if (!users.save_active_thread_frame(frame->rip, frame->rsp)) return false;
    if (auto* current = users.find_thread(current_tid)) save_user_registers(*current, *frame);
    if (!users.block_active_thread_on_any_process()) return false;

    if (!users.activate_thread(next.tid)) {
        if (auto* current = users.find_thread(current_tid)) {
            current->state = hk::userspace::UserThreadState::Running;
            current->block_reason = hk::userspace::UserBlockReason::None;
            current->wait_pipe_id = 0;
            current->wait_process_id = 0;
            current->wait_fd = 0;
            current->wait_buffer = 0;
            current->wait_size = 0;
        }
        return false;
    }

    if (!users.update_user_preemption_gate()) return false;

    restore_user_registers(*frame, *next_thread);
    frame->rip = next.rip;
    frame->rsp = next.rsp;
    frame->rflags = next.rflags;
    frame->cs = next.cs;
    frame->ss = next.ss;
    write_cr3(next.cr3);
    hk::log_hex(hk::LogLevel::Info, "Userspace wait-any blocked TID", current_tid);
    hk::log_hex(hk::LogLevel::Info, "Userspace wait-any switch TID", next.tid);
    return true;
}

bool block_user_sleep_syscall_from_frame(ExceptionFrame* frame, uint64_t delta_ticks) {
    if ((frame->cs & 3) == 0 || delta_ticks == 0) return false;
    auto& users = hk::userspace::userspace_manager();
    uint64_t current_tid = users.active_tid();
    if (current_tid == 0) return false;

    hk::userspace::UserLaunchContext next{};
    if (!users.select_next_runnable_thread(next) || next.tid == current_tid) return false;
    auto* next_thread = users.find_thread(next.tid);
    if (!next_thread) return false;

    if (!hk::timer::lapic_timer_active()) hk::timer::start_lapic_system_tick(0x400000);
    uint64_t wake_tick = hk::timer::ticks() + delta_ticks;
    frame->rax = 0;
    frame->rdx = hybrid::kSyscallErrorNone;
    if (!users.save_active_thread_frame(frame->rip, frame->rsp)) return false;
    if (auto* current = users.find_thread(current_tid)) save_user_registers(*current, *frame);
    if (!users.block_active_thread_until(wake_tick)) return false;

    if (!users.activate_thread(next.tid)) {
        if (auto* current = users.find_thread(current_tid)) {
            current->state = hk::userspace::UserThreadState::Running;
            current->block_reason = hk::userspace::UserBlockReason::None;
            current->wait_pipe_id = 0;
            current->wait_process_id = 0;
            current->wait_wake_tick = 0;
            current->wait_fd = 0;
            current->wait_buffer = 0;
            current->wait_size = 0;
        }
        return false;
    }

    restore_user_registers(*frame, *next_thread);
    frame->rip = next.rip;
    frame->rsp = next.rsp;
    frame->rflags = next.rflags;
    frame->cs = next.cs;
    frame->ss = next.ss;
    write_cr3(next.cr3);
    hk::log(hk::LogLevel::Info, "Userspace sleep using Local APIC tick");
    hk::log_hex(hk::LogLevel::Info, "Userspace sleep blocked TID", current_tid);
    hk::log_hex(hk::LogLevel::Info, "Userspace sleep wake tick", wake_tick);
    hk::log_hex(hk::LogLevel::Info, "Userspace sleep switch TID", next.tid);
    return true;
}

bool preempt_user_thread_from_frame(ExceptionFrame* frame) {
    if ((frame->cs & 3) == 0) return false;
    auto& users = hk::userspace::userspace_manager();
    uint64_t current_tid = users.active_tid();
    if (current_tid == 0) return false;
    if (!users.save_active_thread_frame(frame->rip, frame->rsp)) return false;
    if (auto* current = users.find_thread(current_tid)) save_user_registers(*current, *frame);
    if (!users.mark_active_thread_runnable()) return false;

    hk::userspace::UserLaunchContext next{};
    if (!users.select_next_runnable_thread(next) || next.tid == current_tid) {
        users.activate_thread(current_tid);
        return false;
    }
    if (!users.activate_thread(next.tid)) {
        users.activate_thread(current_tid);
        return false;
    }
    users.note_user_preempt_switch(current_tid, next.tid);
    auto* next_thread = users.find_thread(next.tid);
    if (!next_thread) {
        users.activate_thread(current_tid);
        return false;
    }

    restore_user_registers(*frame, *next_thread);
    frame->rip = next.rip;
    frame->rsp = next.rsp;
    frame->rflags = next.rflags;
    frame->cs = next.cs;
    frame->ss = next.ss;
    write_cr3(next.cr3);
    if (user_preempt_switch_log_count < 8) {
        hk::log_hex(hk::LogLevel::Info, "Userspace APIC preempt switched TID", next.tid);
        hk::log_hex(hk::LogLevel::Info, "Userspace APIC preempt switched PID", next.pid);
        ++user_preempt_switch_log_count;
    }
    return true;
}

bool exit_user_thread_from_frame(ExceptionFrame* frame, uint64_t code) {
    if ((frame->cs & 3) == 0) return false;
    auto& users = hk::userspace::userspace_manager();
    uint64_t exiting_pid = users.active_pid();
    if (exiting_pid == 0) return false;
    if (!users.exit_process(exiting_pid, code)) return false;

    hk::userspace::UserLaunchContext next{};
    if (!users.select_next_runnable_thread(next)) return false;
    if (!users.activate_thread(next.tid)) return false;
    auto* next_thread = users.find_thread(next.tid);
    if (!next_thread) return false;

    restore_user_registers(*frame, *next_thread);
    frame->rip = next.rip;
    frame->rsp = next.rsp;
    frame->rflags = next.rflags;
    frame->cs = next.cs;
    frame->ss = next.ss;
    write_cr3(next.cr3);
    hk::log_hex(hk::LogLevel::Info, "Userspace scheduled after exit PID", exiting_pid);
    hk::log_hex(hk::LogLevel::Info, "Userspace exit switched TID", next.tid);
    return true;
}
}

void handle_exception(ExceptionFrame* frame) {
    hk::log_hex(hk::LogLevel::Error, "exception vector", frame->vector);
    hk::log_hex(hk::LogLevel::Error, "exception error", frame->error);
    hk::log_hex(hk::LogLevel::Error, "exception rip", frame->rip);
    hk::log_hex(hk::LogLevel::Error, "exception rsp", frame->rsp);
    hk::log_hex(hk::LogLevel::Error, "exception rflags", frame->rflags);
    if (frame->vector == 14) {
        uint64_t cr2 = read_cr2();
        hk::log_hex(hk::LogLevel::Error, "page fault cr2", cr2);
        write_error_bit("present", frame->error & 1);
        write_error_bit("write", frame->error & 2);
        write_error_bit("user", frame->error & 4);
        write_error_bit("reserved-bit", frame->error & 8);
        write_error_bit("instruction-fetch", frame->error & 16);
        write_error_bit("protection-key", frame->error & 32);
        write_error_bit("shadow-stack", frame->error & (1ull << 6));
        write_error_bit("sgx", frame->error & (1ull << 15));
    } else if (frame->vector == 13) {
        hk::log(hk::LogLevel::Error, "general protection fault");
    } else if (frame->vector == 6) {
        hk::log(hk::LogLevel::Error, "invalid opcode fault");
    } else if (frame->vector == 8) {
        hk::log(hk::LogLevel::Error, "double fault on TSS IST1 stack");
    }
    console().write("Fatal exception. System halted.\n");
    for (;;) asm volatile("cli; hlt");
}
}

extern "C" void exception_dispatch(hk::interrupts::ExceptionFrame* frame) {
    hk::interrupts::note_vector_dispatch(frame->vector);
    hk::interrupts::handle_exception(frame);
}

extern "C" uint64_t* interrupt_dispatch(hk::interrupts::ExceptionFrame* frame) {
    hk::interrupts::note_vector_dispatch(frame->vector);
    if (frame->vector < 32) {
        hk::interrupts::handle_exception(frame);
        return nullptr;
    }
    if (frame->vector < 48) {
        uint8_t irq = static_cast<uint8_t>(frame->vector - 32);
        uint64_t* next_rsp = nullptr;
        if (irq == 0) {
            pit_note_tick();
            hk::userspace::userspace_manager().wake_sleepers(hk::timer::ticks());
            if (hk::timer::preemption_enabled()) {
                next_rsp = hk::sched::scheduler().schedule_from_interrupt(reinterpret_cast<uint64_t*>(frame));
            }
        } else if (irq == 1) {
            hk::drivers::ps2_keyboard::handle_irq();
        }
        hk::interrupts::send_eoi(irq);
        return next_rsp;
    }
    if (frame->vector == 0x40) {
        lapic_note_tick();
        hk::cpu::runtime().note_scheduler_tick();
        if ((frame->cs & 3) != 0) hk::userspace::userspace_manager().note_current_user_tick();
        uint64_t user_sleepers = hk::userspace::userspace_manager().wake_sleepers(hk::timer::ticks());
        if (user_sleepers != 0) hk::log_hex(hk::LogLevel::Info, "Userspace Local APIC sleep wakes", user_sleepers);
        uint64_t* next_rsp = nullptr;
        if ((frame->cs & 3) != 0 && hk::timer::user_preemption_enabled() &&
            hk::userspace::userspace_manager().user_timeslice_expired()) {
            hk::interrupts::preempt_user_thread_from_frame(frame);
        } else if (hk::timer::preemption_enabled()) {
            next_rsp = hk::sched::scheduler().schedule_from_interrupt(reinterpret_cast<uint64_t*>(frame));
        }
        hk::apic::local_apic().eoi();
        return next_rsp;
    }
    if (frame->vector == 0x41) {
        hk::smp::handle_ipi();
        return nullptr;
    }
    if (frame->vector == 0x80) {
        if (frame->rax == static_cast<uint64_t>(hybrid::SyscallNumber::Yield)) {
            if (hk::interrupts::yield_user_thread_from_frame(frame)) return nullptr;
            frame->rax = 0;
            frame->rdx = hybrid::kSyscallErrorNone;
            return nullptr;
        }
        if (frame->rax == static_cast<uint64_t>(hybrid::SyscallNumber::Exit)) {
            if (hk::interrupts::exit_user_thread_from_frame(frame, frame->rdi)) return nullptr;
        }
        if (frame->rax == static_cast<uint64_t>(hybrid::SyscallNumber::SleepTicks) && frame->rdi != 0) {
            if (hk::interrupts::block_user_sleep_syscall_from_frame(frame, frame->rdi)) return nullptr;
        }
        uint64_t syscall_number = frame->rax;
        auto result = hk::syscall::dispatch(syscall_number, frame->rdi, frame->rsi, frame->rdx, frame->r10);
        if (result.error == hybrid::kSyscallErrorWouldBlock) {
            auto number = static_cast<hybrid::SyscallNumber>(syscall_number);
            if (number == hybrid::SyscallNumber::Read) {
                if (hk::interrupts::block_user_pipe_syscall_from_frame(frame, false, static_cast<uint32_t>(frame->rdi))) return nullptr;
            } else if (number == hybrid::SyscallNumber::WriteFile) {
                if (hk::interrupts::block_user_pipe_syscall_from_frame(frame, true, static_cast<uint32_t>(frame->rdi))) return nullptr;
            } else if (number == hybrid::SyscallNumber::Write &&
                       (frame->rdi == hybrid::kStdoutFd || frame->rdi == hybrid::kStderrFd) && frame->rdx != 0) {
                if (hk::interrupts::block_user_pipe_syscall_from_frame(frame, true, static_cast<uint32_t>(frame->rdi))) return nullptr;
            } else if (number == hybrid::SyscallNumber::Wait) {
                if (hk::interrupts::block_user_wait_syscall_from_frame(frame, frame->rdi)) return nullptr;
            } else if (number == hybrid::SyscallNumber::WaitAny) {
                if (hk::interrupts::block_user_wait_any_syscall_from_frame(frame)) return nullptr;
            }
        }
        frame->rax = result.value;
        frame->rdx = result.error;
        return nullptr;
    }
    return nullptr;
}
