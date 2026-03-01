#pragma once

#include <cstddef>

class Slab {
public:
    static Slab* create(size_t block_size, int node_id);

    void* allocate_block();
    void  free_block(void*);

    bool  has_free() const noexcept;

private:
    // TODO: layout
};