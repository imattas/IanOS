#include "hk/mm/heap.hpp"
#include "hk/mm/pmm.hpp"
#include "hk/mm/vmm.hpp"
#include "hk/lib/string.hpp"
#include "hk/sync/spinlock.hpp"

namespace hk::mm {

namespace {
hk::sync::SpinLock heap_lock;
constexpr uint64_t kHeapInitialPages = 16;
constexpr size_t kMinSplitPayload = 32;
uintptr_t align_ptr(uintptr_t value, size_t align) {
    return (value + align - 1) & ~(static_cast<uintptr_t>(align) - 1);
}
}

KernelHeap& heap() {
    static KernelHeap h;
    return h;
}

void KernelHeap::initialize() {
    hk::sync::LockGuard guard(heap_lock);
    if (!first_) expand(kHeapInitialPages * kPageSize);
}

bool KernelHeap::expand(size_t bytes) {
    size_t pages = align_up(bytes) / kPageSize;
    uint64_t virt = heap_end_;
    for (size_t i = 0; i < pages; ++i) {
        uint64_t phys = pmm().allocate_page();
        if (phys == 0) return false;
        auto mapped = vmm().map_page(virt + i * kPageSize, phys, PageWrite | PageGlobal);
        if (!mapped.ok) return false;
        memset(reinterpret_cast<void*>(virt + i * kPageSize), 0, kPageSize);
    }
    auto* block = reinterpret_cast<Block*>(virt);
    block->size = pages * kPageSize - sizeof(Block);
    block->free = true;
    block->next = nullptr;
    if (!first_) {
        first_ = block;
    } else {
        Block* tail = first_;
        while (tail->next) tail = tail->next;
        tail->next = block;
    }
    heap_end_ += pages * kPageSize;
    return true;
}

void* KernelHeap::allocate(size_t size, size_t alignment) {
    if (size == 0) return nullptr;
    if (alignment < 16) alignment = 16;
    hk::sync::LockGuard guard(heap_lock);
    for (;;) {
        for (Block* block = first_; block; block = block->next) {
            uintptr_t payload = reinterpret_cast<uintptr_t>(block) + sizeof(Block);
            uintptr_t aligned = align_ptr(payload, alignment);
            size_t padding = aligned - payload;
            if (block->free && block->size >= size + padding) {
                uintptr_t block_end = payload + block->size;
                uintptr_t used_end = aligned + size;
                if (block_end > used_end + sizeof(Block) + kMinSplitPayload) {
                    auto* next = reinterpret_cast<Block*>(used_end);
                    next->size = block_end - used_end - sizeof(Block);
                    next->free = true;
                    next->next = block->next;
                    block->next = next;
                    block->size = used_end - payload;
                }
                block->free = false;
                return reinterpret_cast<void*>(aligned);
            }
        }
        if (!expand(size + alignment + sizeof(Block))) return nullptr;
    }
}

void* KernelHeap::calloc(size_t count, size_t size) {
    size_t total = count * size;
    void* ptr = allocate(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void KernelHeap::free(void* ptr) {
    if (!ptr) return;
    hk::sync::LockGuard guard(heap_lock);
    uintptr_t raw = reinterpret_cast<uintptr_t>(ptr);
    for (Block* block = first_; block; block = block->next) {
        uintptr_t begin = reinterpret_cast<uintptr_t>(block) + sizeof(Block);
        uintptr_t end = begin + block->size;
        if (raw >= begin && raw < end) {
            block->free = true;
            while (block->next && block->next->free) {
                uintptr_t block_end = reinterpret_cast<uintptr_t>(block) + sizeof(Block) + block->size;
                if (block_end != reinterpret_cast<uintptr_t>(block->next)) break;
                block->size += sizeof(Block) + block->next->size;
                block->next = block->next->next;
            }
            for (Block* prev = first_; prev && prev->next; prev = prev->next) {
                if (prev->next == block && prev->free) {
                    uintptr_t prev_end = reinterpret_cast<uintptr_t>(prev) + sizeof(Block) + prev->size;
                    if (prev_end == reinterpret_cast<uintptr_t>(block)) {
                        prev->size += sizeof(Block) + block->size;
                        prev->next = block->next;
                    }
                    break;
                }
            }
            return;
        }
    }
}

HeapStats KernelHeap::stats() const {
    hk::sync::LockGuard guard(heap_lock);
    HeapStats out{};
    out.heap_start = heap_start_;
    out.heap_end = heap_end_;
    out.heap_bytes = heap_end_ >= heap_start_ ? heap_end_ - heap_start_ : 0;
    for (Block* block = first_; block; block = block->next) {
        ++out.block_count;
        if (block->free) {
            ++out.free_blocks;
            out.free_bytes += block->size;
            if (block->size > out.largest_free_block) out.largest_free_block = block->size;
        } else {
            ++out.used_blocks;
            out.used_bytes += block->size;
        }
    }
    return out;
}

void* kmalloc(size_t size) { return heap().allocate(size); }
void* kcalloc(size_t count, size_t size) { return heap().calloc(count, size); }
void* kmalloc_aligned(size_t size, size_t alignment) { return heap().allocate(size, alignment); }
void kfree(void* ptr) { heap().free(ptr); }

} // namespace hk::mm
