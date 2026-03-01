#pragma once

#include <cstddef>

class LargeObjectAllocator {
public:
    explicit LargeObjectAllocator(int node_id);

    void* allocate(size_t size, size_t alignment);
    void  deallocate(void* ptr, size_t size);

private:
    int node_id_;
};