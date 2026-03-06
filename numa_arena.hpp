#pragma once

#include "size_divide/small_object_allocator.hpp"
#include "size_divide/large_object_allocator.hpp"
#include <vector>
#include <memory>
#include <thread>
#include <cassert>
#include <cstdint>
#include <sched.h>

// ============================================================
// NUMA ARENA
// ============================================================

class NumaArena {
public:
    explicit NumaArena(int node_id)
        : node_id_(node_id),
            small_(node_id),
            large_(node_id)
    {}

    void* allocate(size_t size, size_t alignment) {
        if (size <= SMALL_LARGE_THRESHOLD) {
            void* block = small_.allocate(size);

            auto* header = reinterpret_cast<BlockHeader*>(block);

            header->node_id = static_cast<uint32_t>(node_id_);
            header->size_class = static_cast<uint32_t>(size);
            header->size = 0;

            return header->to_user_ptr();
        }

        return large_.allocate(size, alignment);
    }

    void deallocate(void* ptr, size_t /*size*/, size_t /*alignment*/) {
        if (!ptr) return;

        auto* header = BlockHeader::from_user_ptr(ptr);

        if (header->size_class != 0) {
            small_.deallocate(header);
        } else {
            large_.deallocate(ptr, header->size);
        }
    }

    int node_id() const noexcept {
        return node_id_;
    }

private:
    int node_id_;
    SmallObjectAllocator small_;
    LargeObjectAllocator large_;
};


// ============================================================
// NUMA MANAGER
// ============================================================

class NumaManager {
public:
    static NumaManager& instance() {
        static NumaManager inst;
        return inst;
    }

    NumaArena& arena_for_current_thread() {
        int node = current_node();
        return *arenas_[node];
    }

    NumaArena& arena_for_node(int node_id) {
        assert(node_id >= 0 && node_id < node_count_);
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

private:
    NumaManager() {
        init_topology();
        init_arenas();
    }

    void init_topology() {
        cpu_count_ = std::thread::hardware_concurrency();

        if (cpu_count_ == 0)
            cpu_count_ = 1;

        node_count_ = 1;

        cpu_to_node_.resize(cpu_count_, 0);

        // TODO: реальная NUMA топология
    }

    void init_arenas() {
        arenas_.reserve(node_count_);

        for (int i = 0; i < node_count_; ++i) {
            arenas_.emplace_back(
                std::make_unique<NumaArena>(i)
            );
        }
    }

    int cpu_count_  = 0;
    int node_count_ = 0;

    std::vector<int> cpu_to_node_;

    std::vector<std::unique_ptr<NumaArena>> arenas_;
};