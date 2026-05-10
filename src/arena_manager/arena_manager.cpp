#include "arena_manager/arena_manager.hpp"

#include "numa_topology/numa_topology.hpp"
#include "virtual_memory/virtual_memory.hpp"

#include <new>

ArenaManager::ArenaManager() {
    init_arenas();
}

void ArenaManager::init_arenas() {
    const int node_count = NumaTopologyManager::instance().node_count();

    arenas_.clear();
    arenas_.reserve(node_count);

    for (int i = 0; i < node_count; ++i) {
        arenas_.emplace_back(create_arena_on_node(i, node_count > 1));
    }
}

NumaArenaPtr ArenaManager::create_arena_on_node(int node_id, bool foreign_freelist_enabled) {
    void* mem = VirtualMemory::alloc_on_node(sizeof(NumaArena), node_id);

    try {
        return NumaArenaPtr(new (mem) NumaArena(node_id, foreign_freelist_enabled));
    } catch (...) {
        VirtualMemory::release(mem, sizeof(NumaArena));
        throw;
    }
}
