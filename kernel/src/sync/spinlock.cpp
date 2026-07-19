#include "hk/sync/spinlock.hpp"

namespace hk::sync {

void SpinLock::lock() {
    while (__atomic_exchange_n(&value_, 1u, __ATOMIC_ACQUIRE) != 0) {
        while (__atomic_load_n(&value_, __ATOMIC_RELAXED) != 0) {
            asm volatile("pause");
        }
    }
}

void SpinLock::unlock() {
    __atomic_store_n(&value_, 0u, __ATOMIC_RELEASE);
}

uint64_t irq_save() {
    uint64_t flags;
    asm volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

void irq_restore(uint64_t rflags) {
    if (rflags & (1ull << 9)) asm volatile("sti" : : : "memory");
}

} // namespace hk::sync
