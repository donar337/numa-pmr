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
        auto& context = ThreadNumaContext::current();
        return context.arena().allocate(bytes, alignment, context.small_cache());
    }

    void do_deallocate(void* p, size_t bytes, size_t alignment) override {
        if (!p) return;

        auto& context = ThreadNumaContext::current();
        auto* header = BlockHeader::from_user_ptr(p);

        context
            .arena_for_node(static_cast<int>(header->node_id))
            .deallocate(p, context.small_cache());
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};