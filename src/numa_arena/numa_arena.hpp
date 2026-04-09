#pragma once

#include "size_divide/small_object/small_object_allocator.hpp"
#include "size_divide/large_object/large_object_allocator.hpp"

#include <memory>
#include <cstddef>

/**
 * Per-NUMA-node allocation faсade, holds SmallObjectAllocator and LargeObjectAllocator for a NUMA node.
 */
class NumaArena {
public:
    explicit NumaArena(int node_id)
        : node_id_(node_id),
          small_(node_id),
          large_(node_id)
    {}

    void* allocate(size_t size, size_t alignment);
    void deallocate(void* ptr);

    int node_id() const noexcept {
        return node_id_;
    }

private:
    int node_id_;
    SmallObjectAllocator small_;
    LargeObjectAllocator large_;
};

struct NumaArenaDeleter {
    void operator()(NumaArena* arena) const noexcept;
};

using NumaArenaPtr = std::unique_ptr<NumaArena, NumaArenaDeleter>;