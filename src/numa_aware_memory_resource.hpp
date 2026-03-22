#pragma once

#include <cstddef>
#include <memory_resource>
#include "numa_arena.hpp"


// ============================================================
// PMR MEMORY RESOURCE
// ============================================================

class NumaMemoryResource : public std::pmr::memory_resource {
protected:
    void* do_allocate(size_t bytes, size_t alignment) override {
        return NumaManager::instance()
            .arena_for_current_thread()
            .allocate(bytes, alignment);
    }

    void do_deallocate(void* p, size_t bytes, size_t alignment) override {
        if (!p) return;

        auto* header = BlockHeader::from_user_ptr(p);

        NumaManager::instance()
            .arena_for_node(static_cast<int>(header->node_id))
            .deallocate(p, bytes, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};