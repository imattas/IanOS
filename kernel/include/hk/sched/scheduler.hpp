#pragma once
#include <stddef.h>
#include <stdint.h>
namespace hk::sched {
using ThreadEntry = void (*)(void*);
enum class ThreadState : uint8_t { Ready, Running, Blocked, Sleeping, Dead };
struct Thread {
    uint64_t id;
    ThreadState state;
    uint64_t* kernel_stack_base;
    uint64_t* kernel_stack_top;
    uint64_t* saved_rsp;
    ThreadEntry entry;
    void* argument;
    const char* name;
    uint64_t affinity_mask;
    uint64_t switches;
    uint64_t wake_tick;
};

struct ThreadSnapshot {
    uint64_t id;
    ThreadState state;
    const char* name;
    uint64_t affinity_mask;
    uint64_t switches;
    uint64_t wake_tick;
};

class Scheduler {
public:
    void initialize();
    Thread* create_kernel_thread(const char* name, ThreadEntry entry, void* argument, uint64_t affinity_mask = ~0ull);
    Thread* current_thread() { return current_; }
    size_t thread_count() const { return count_; }
    uint64_t switch_count() const { return switch_count_; }
    uint64_t yield_count() const { return yield_count_; }
    uint64_t preempt_count() const { return preempt_count_; }
    void yield();
    uint64_t* schedule_from_interrupt(uint64_t* interrupted_rsp);
    void exit_current_thread();
    void block_current_thread();
    void sleep_current_until(uint64_t wake_tick);
    void wake_sleepers(uint64_t now_tick);
    uint64_t ready_count() const;
    uint64_t sleeping_count() const;
    uint64_t dead_count() const;
    bool snapshot_thread(size_t index, ThreadSnapshot& out) const;
private:
    Thread threads_[32]{};
    size_t count_ = 0;
    uint64_t next_id_ = 1;
    uint64_t switch_count_ = 0;
    uint64_t yield_count_ = 0;
    uint64_t preempt_count_ = 0;
    Thread* current_ = nullptr;
    Thread* idle_ = nullptr;
    size_t current_index_ = 0;
    Thread* pick_next();
    void switch_from_current(ThreadState previous_state);
};
Scheduler& scheduler();
void yield();
void sleep_current_until(uint64_t wake_tick);
}
