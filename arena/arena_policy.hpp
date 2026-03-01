#pragma once

#include <cstddef>

struct ArenaPolicy {
    size_t small_large_threshold = 4096;
    size_t slab_size = 64 * 1024;
};