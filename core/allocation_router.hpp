#pragma once

#include <cstddef>

class AllocationRouter {
public:
    virtual int select_node(size_t size) = 0;
};