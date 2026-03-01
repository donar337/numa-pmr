#pragma once

#include <cstddef>

class VirtualMemory {
public:
    static void* reserve(size_t size);
    static void  release(void* ptr, size_t size);

    static void  bind_to_node(void* ptr, size_t size, int node_id);
};