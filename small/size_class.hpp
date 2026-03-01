#pragma once

#include "slab.hpp"
#include <vector>

class SizeClass {
public:
    void* allocate();
    void  deallocate(void* ptr);

private:
    Slab* allocate_from_current();
    Slab* create_new_slab();

    size_t block_size_;
    int    node_id_;

    Slab* current_;
    std::vector<Slab*> slabs_;  // TODO: заменить на partial/full lists
};