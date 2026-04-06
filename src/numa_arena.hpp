#pragma once

#include "size_divide/small_object/small_object_allocator.hpp"
#include "size_divide/large_object/large_object_allocator.hpp"
#include <vector>
#include <memory>
#include <cstddef>
#include <stdexcept>
#include <sched.h>

/**
 * Per-NUMA-node allocation faсade, holds SmallObjectAllocator and LargeObjectAllocator for a NUMA node.
 */
class NumaArena {
public:
    explicit NumaArena(int node_id)
        : node_id_(node_id),
          small_(node_id),
          large_(node_id)
    {}

    void* allocate(size_t size, size_t alignment);
    void deallocate(void* ptr);

    int node_id() const noexcept {
        return node_id_;
    }

private:
    int node_id_;
    SmallObjectAllocator small_;
    LargeObjectAllocator large_;
};

struct NumaArenaDeleter {
    void operator()(NumaArena* arena) const noexcept;
};

using NumaArenaPtr = std::unique_ptr<NumaArena, NumaArenaDeleter>;

class NumaManager {
public:
    static NumaManager& instance() {
        static NumaManager inst;
        return inst;
    }

    NumaArena& arena_for_node(int node_id) {
        if (node_id < 0 || node_id >= node_count_) {
            throw std::out_of_range("invalid NUMA node id");
        }

        return *arenas_[node_id];
    }

    int current_node() const {
        int cpu = sched_getcpu();

        if (cpu < 0 || cpu >= static_cast<int>(cpu_to_node_.size())) {
            return 0;
        }

        return cpu_to_node_[cpu];
    }

    int node_count() const noexcept {
        return node_count_;
    }

    bool pin_current_thread_to_node(int node_id) const noexcept;

private:
    NumaManager();

    void init_topology();
    void init_single_node_topology();
    void init_arenas();
    static NumaArenaPtr create_arena_on_node(int node_id);

    int cpu_count_  = 0;
    int node_count_ = 0;

    std::vector<int> cpu_to_node_;
    std::vector<std::vector<int>> node_to_cpus_;

    std::vector<NumaArenaPtr> arenas_;
};