#pragma once

#include "numa_arena/numa_arena.hpp"
#include "numa_topology/numa_topology.hpp"
#include "virtual_memory/virtual_memory.hpp"

#include <cstddef>
#include <memory_resource>
#include <new>

/**
 * PMR resource that owns a standalone NUMA-bound arena.
 *
 * Each instance owns its arena and is only equal to itself. It intentionally
 * does not use ArenaManager or ThreadLocalCache.
 */
class numa_arena_memory_resource : public std::pmr::memory_resource {
public:
    explicit numa_arena_memory_resource(bool sync = true)
        : numa_arena_memory_resource(NumaTopologyManager::instance().current_node_from_cpu(), sync)
    {}

    /**
     * Creates a standalone arena on a NUMA node.
     *
     * @param node_id the NUMA node to create the arena on. The overload without
     * node_id uses the node of the current CPU. Invalid node_id values also fall
     * back to that current node. 
     * @param sync when true, the internal small/large allocators use mutexes and
     * can be shared between threads; when false, locking is disabled for
     * single-threaded ownership.
     */
    numa_arena_memory_resource(int node_id, bool sync = true)
        : node_id_(NumaTopologyManager::instance().normalize_node_id(node_id)),
          sync_(sync),
          arena_(create_arena(node_id_))
    {}

    numa_arena_memory_resource(const numa_arena_memory_resource&) = delete;
    numa_arena_memory_resource& operator=(const numa_arena_memory_resource&) = delete;
    numa_arena_memory_resource(numa_arena_memory_resource&&) = delete;
    numa_arena_memory_resource& operator=(numa_arena_memory_resource&&) = delete;

    int node_id() const noexcept {
        return node_id_;
    }

    bool sync() const noexcept {
        return sync_;
    }

protected:
    void* do_allocate(size_t bytes, size_t alignment) override {
        return arena_->allocate(bytes, alignment);
    }

    void do_deallocate(void* p, size_t bytes, size_t alignment) override {
        (void)bytes;
        (void)alignment;

        if (!p) {
            return;
        }

        arena_->deallocate(p);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

private:
    NumaArenaPtr create_arena(int node_id) {
        void* mem = VirtualMemory::alloc_on_node(sizeof(NumaArena), node_id);

        try {
            return NumaArenaPtr(new (mem) NumaArena(
                node_id,
                false,
                sync_,
                false
            ));
        } catch (...) {
            VirtualMemory::release(mem, sizeof(NumaArena));
            throw;
        }
    }

    int node_id_;
    bool sync_;
    NumaArenaPtr arena_;
};
