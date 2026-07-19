#include "hk/sched/scheduler.hpp"
#include "hk/mm/pmm.hpp"
#include "hk/mm/vmm.hpp"
#include "hk/log.hpp"
#include "hk/sync/spinlock.hpp"
#include "hk/timer/pit.hpp"

extern "C" void context_switch(uint64_t** old_rsp, uint64_t* new_rsp);

namespace hk::sched {
namespace {
hk::sync::SpinLock sched_lock;
constexpr uint64_t kKernelStackVirtBase = 0xffff800000400000ull;
constexpr uint64_t kKernelStackPages = 4;
constexpr uint64_t kKernelStackStride = kKernelStackPages * hk::mm::kPageSize;
extern "C" void thread_trampoline(ThreadEntry entry, void* argument) {
    entry(argument);
    scheduler().exit_current_thread();
}

void idle_entry(void*) {
    for (;;) asm volatile("hlt");
}
}

Scheduler& scheduler() {
    static Scheduler sched;
    return sched;
}

void Scheduler::initialize() {
    hk::sync::LockGuard guard(sched_lock);
    count_ = 0;
    next_id_ = 1;
    switch_count_ = 0;
    yield_count_ = 0;
    preempt_count_ = 0;
    current_ = nullptr;
    current_index_ = 0;
    Thread* bootstrap = &threads_[count_++];
    *bootstrap = Thread{next_id_++, ThreadState::Running, nullptr, nullptr, nullptr, nullptr, nullptr, "bootstrap", 1, 0, 0};
    current_ = bootstrap;
    idle_ = create_kernel_thread("idle", idle_entry, nullptr, 1);
    if (idle_) {
        idle_->state = ThreadState::Ready;
    }
}

Thread* Scheduler::create_kernel_thread(const char* name, ThreadEntry entry, void* argument, uint64_t affinity_mask) {
    uint64_t flags = hk::sync::irq_save();
    if (count_ >= 32 || entry == nullptr) {
        hk::sync::irq_restore(flags);
        return nullptr;
    }
    uint64_t stack_phys = hk::mm::pmm().allocate_contiguous(4);
    if (stack_phys == 0) {
        hk::sync::irq_restore(flags);
        return nullptr;
    }
    uint64_t stack_virt = kKernelStackVirtBase + count_ * kKernelStackStride;
    auto mapped = hk::mm::vmm().map_range(stack_virt, stack_phys, kKernelStackStride, hk::mm::PageWrite | hk::mm::PageGlobal);
    if (!mapped.ok) {
        hk::sync::irq_restore(flags);
        return nullptr;
    }
    auto* stack_base = reinterpret_cast<uint64_t*>(stack_virt);
    auto* stack_top = reinterpret_cast<uint64_t*>(stack_virt + kKernelStackStride - 64);
    auto* frame = stack_top - 22;
    for (int i = 0; i < 22; ++i) frame[i] = 0;
    frame[9] = reinterpret_cast<uint64_t>(entry);     // rdi
    frame[10] = reinterpret_cast<uint64_t>(argument); // rsi
    frame[15] = 0;                                    // synthetic vector
    frame[16] = 0;                                    // synthetic error
    frame[17] = reinterpret_cast<uint64_t>(thread_trampoline);
    frame[18] = 0x08;
    frame[19] = 0x202;
    frame[20] = reinterpret_cast<uint64_t>(stack_top);
    frame[21] = 0x10;
    Thread* thread = &threads_[count_++];
    *thread = Thread{next_id_++, ThreadState::Ready, stack_base, stack_top, frame, entry, argument, name, affinity_mask, 0, 0};
    hk::sync::irq_restore(flags);
    return thread;
}

Thread* Scheduler::pick_next() {
    if (count_ == 0) return nullptr;
    for (size_t step = 1; step <= count_; ++step) {
        size_t index = (current_index_ + step) % count_;
        Thread& candidate = threads_[index];
        if (&candidate != idle_ && candidate.state == ThreadState::Ready) {
            current_index_ = index;
            return &candidate;
        }
    }
    return idle_;
}

void Scheduler::switch_from_current(ThreadState previous_state) {
    Thread* previous = current_;
    if (!previous) return;
    previous->state = previous_state;
    Thread* next = pick_next();
    if (!next || next == previous) {
        previous->state = ThreadState::Running;
        return;
    }
    current_ = next;
    current_->state = ThreadState::Running;
    ++switch_count_;
    ++current_->switches;
    context_switch(&previous->saved_rsp, current_->saved_rsp);
}

uint64_t* Scheduler::schedule_from_interrupt(uint64_t* interrupted_rsp) {
    if (!current_) return interrupted_rsp;
    current_->saved_rsp = interrupted_rsp;
    if (current_->state == ThreadState::Running) current_->state = ThreadState::Ready;
    wake_sleepers(hk::timer::ticks());
    Thread* next = pick_next();
    if (!next || next == current_) {
        if (current_) current_->state = ThreadState::Running;
        return interrupted_rsp;
    }
    current_ = next;
    current_->state = ThreadState::Running;
    ++switch_count_;
    ++preempt_count_;
    ++current_->switches;
    return current_->saved_rsp;
}

void Scheduler::yield() {
    uint64_t flags = hk::sync::irq_save();
    if (!current_) {
        hk::sync::irq_restore(flags);
        return;
    }
    ++yield_count_;
    switch_from_current(ThreadState::Ready);
    hk::sync::irq_restore(flags);
}

void Scheduler::exit_current_thread() {
    hk::sync::irq_save();
    if (current_ && current_ != idle_) current_->state = ThreadState::Dead;
    Thread* next = pick_next();
    if (!next) next = idle_;
    Thread* previous = current_;
    current_ = next;
    current_->state = ThreadState::Running;
    ++switch_count_;
    context_switch(&previous->saved_rsp, current_->saved_rsp);
    for (;;) asm volatile("hlt");
}

void Scheduler::block_current_thread() {
    uint64_t flags = hk::sync::irq_save();
    switch_from_current(ThreadState::Blocked);
    hk::sync::irq_restore(flags);
}

void Scheduler::sleep_current_until(uint64_t wake_tick) {
    uint64_t flags = hk::sync::irq_save();
    if (current_ && current_ != idle_) current_->wake_tick = wake_tick;
    switch_from_current(ThreadState::Sleeping);
    hk::sync::irq_restore(flags);
}

void Scheduler::wake_sleepers(uint64_t now_tick) {
    for (size_t i = 0; i < count_; ++i) {
        if (threads_[i].state == ThreadState::Sleeping && threads_[i].wake_tick <= now_tick) {
            threads_[i].state = ThreadState::Ready;
            threads_[i].wake_tick = 0;
        }
    }
}

uint64_t Scheduler::ready_count() const {
    uint64_t total = 0;
    for (size_t i = 0; i < count_; ++i) if (threads_[i].state == ThreadState::Ready) ++total;
    return total;
}

uint64_t Scheduler::sleeping_count() const {
    uint64_t total = 0;
    for (size_t i = 0; i < count_; ++i) if (threads_[i].state == ThreadState::Sleeping) ++total;
    return total;
}

uint64_t Scheduler::dead_count() const {
    uint64_t total = 0;
    for (size_t i = 0; i < count_; ++i) if (threads_[i].state == ThreadState::Dead) ++total;
    return total;
}

bool Scheduler::snapshot_thread(size_t index, ThreadSnapshot& out) const {
    if (index >= count_) return false;
    const Thread& thread = threads_[index];
    out = ThreadSnapshot{thread.id, thread.state, thread.name, thread.affinity_mask, thread.switches, thread.wake_tick};
    return true;
}

void yield() {
    scheduler().yield();
}

void sleep_current_until(uint64_t wake_tick) {
    scheduler().sleep_current_until(wake_tick);
}
}
