#pragma once

#include <cstddef>

struct ArenaStats {
    size_t allocated_small = 0;
    size_t allocated_large = 0;
};