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
 * does not use NumaManager or ThreadLocalCache.
 */
class ArenaMemoryResource : public std::pmr::memory_resource {
public:
    explicit ArenaMemoryResource(bool sync = true, bool do_pinning = false)
        : ArenaMemoryResource(numa_topology::current_node_from_cpu(), sync, do_pinning)
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
     * @param do_pinning when true, tries to pin the constructing thread to the
     * selected node.
     */
    ArenaMemoryResource(int node_id, bool sync = true, bool do_pinning = false)
        : node_id_(numa_topology::normalize_node_id(node_id)),
          sync_(sync),
          do_pinning_(do_pinning),
          arena_(create_arena(node_id_)) {
        if (do_pinning_) {
            numa_topology::pin_current_thread_to_node(node_id_);
        }
    }

    ArenaMemoryResource(const ArenaMemoryResource&) = delete;
    ArenaMemoryResource& operator=(const ArenaMemoryResource&) = delete;
    ArenaMemoryResource(ArenaMemoryResource&&) = delete;
    ArenaMemoryResource& operator=(ArenaMemoryResource&&) = delete;

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
    bool do_pinning_;
    NumaArenaPtr arena_;
};
