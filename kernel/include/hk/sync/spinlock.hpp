#pragma once
#include <stdint.h>

namespace hk::sync {

class SpinLock {
public:
    void lock();
    void unlock();
private:
    uint32_t value_ = 0;
};

class LockGuard {
public:
    explicit LockGuard(SpinLock& lock) : lock_(lock) { lock_.lock(); }
    ~LockGuard() { lock_.unlock(); }
private:
    SpinLock& lock_;
};

uint64_t irq_save();
void irq_restore(uint64_t rflags);

} // namespace hk::sync
