#pragma once

#include <stdexcept>
#include <vector>

#include "numa_arena/numa_arena.hpp"

class ArenaManager {
public:
    static ArenaManager& instance() {
        static ArenaManager inst;
        return inst;
    }

    NumaArena& arena_for_node(int node_id) {
        if (node_id < 0 || node_id >= static_cast<int>(arenas_.size())) {
            throw std::out_of_range("invalid NUMA node id");
        }

        return *arenas_[node_id];
    }

private:
    ArenaManager();

    void init_arenas();
    static NumaArenaPtr create_arena_on_node(int node_id, bool foreign_freelist_enabled);

    std::vector<NumaArenaPtr> arenas_;
};
