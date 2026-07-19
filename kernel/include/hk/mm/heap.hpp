#pragma once
#include <stddef.h>
#include <stdint.h>

namespace hk::mm {

struct HeapStats {
    uint64_t heap_start;
    uint64_t heap_end;
    uint64_t heap_bytes;
    uint64_t block_count;
    uint64_t used_blocks;
    uint64_t free_blocks;
    uint64_t used_bytes;
    uint64_t free_bytes;
    uint64_t largest_free_block;
};

class KernelHeap {
public:
    void initialize();
    void* allocate(size_t size, size_t alignment = 16);
    void* calloc(size_t count, size_t size);
    void free(void* ptr);
    uint64_t heap_start() const { return heap_start_; }
    uint64_t heap_end() const { return heap_end_; }
    HeapStats stats() const;
private:
    struct Block {
        size_t size;
        bool free;
        Block* next;
    };
    uint64_t heap_start_ = 0xffff800000000000ull;
    uint64_t heap_end_ = 0xffff800000000000ull;
    Block* first_ = nullptr;
    bool expand(size_t bytes);
};

KernelHeap& heap();
void* kmalloc(size_t size);
void* kcalloc(size_t count, size_t size);
void* kmalloc_aligned(size_t size, size_t alignment);
void kfree(void* ptr);

} // namespace hk::mm
