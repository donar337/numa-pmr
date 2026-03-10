#pragma once

#include "block.hpp"
#include <limits>

// ============================================================
// LARGE OBJECT ALLOCATOR
// ============================================================

class LargeObjectAllocator {
public:
    explicit LargeObjectAllocator(int node_id)
        : node_id_(node_id) {}

    void* allocate(size_t size, size_t alignment) {
        if (alignment < alignof(void*)) {
            alignment = alignof(void*);
        }

        if (alignment > std::numeric_limits<size_t>::max() - sizeof(BlockHeader) ||
            size > std::numeric_limits<size_t>::max() - alignment - sizeof(BlockHeader)) {
            throw std::bad_alloc();
        }

        size_t total = size + alignment + sizeof(BlockHeader);

        void* raw = VirtualMemory::reserve(total);

        VirtualMemory::bind_to_node(
            raw,
            total,
            node_id_,
            VirtualMemory::NumaPolicy::Bind
        );

        uintptr_t raw_addr = reinterpret_cast<uintptr_t>(raw);

        uintptr_t start = raw_addr + sizeof(BlockHeader);

        uintptr_t aligned =
            VirtualMemory::align_up(start, alignment);

        void* user_ptr = reinterpret_cast<void*>(aligned);

        auto* header = reinterpret_cast<BlockHeader*>(
            aligned - sizeof(BlockHeader)
        );

        header->node_id = static_cast<uint32_t>(node_id_);
        header->size_class = 0; // large
        header->size = size;

        header->raw_ptr = raw;
        header->total_size = total;

        return user_ptr;
    }

    void deallocate(void* ptr, size_t) {
        if (!ptr) return;

        auto* header = BlockHeader::from_user_ptr(ptr);

        VirtualMemory::release(
            header->raw_ptr,
            header->total_size
        );
    }

private:
    int node_id_;
};