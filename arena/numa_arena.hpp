#pragma once

#include <cstddef>
#include "../small/small_object_allocator.hpp"
#include "../large/large_object_allocator.hpp"

class NumaArena {
public:
    explicit NumaArena(int node_id);

    void* allocate(size_t size, size_t alignment);
    void  deallocate(void* ptr, size_t size);

    int node_id() const noexcept;

private:
    int node_id_;

    SmallObjectAllocator small_;
    LargeObjectAllocator large_;
};