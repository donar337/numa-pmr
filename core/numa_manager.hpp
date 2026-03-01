#pragma once

#include "../arena/numa_arena.hpp"
#include <vector>
#include <memory>

class NumaManager {
public:
    static NumaManager& instance();

    NumaArena& arena_for_node(int node_id);
    NumaArena& arena_for_current_thread();

    int current_node() const noexcept;

private:
    std::vector<std::unique_ptr<NumaArena>> arenas_;
};