#pragma once

#include <cstddef>
#include <memory_resource>
#include "numa_arena.hpp"


/**
 * 
 */
class NumaMemoryResource : public std::pmr::memory_resource {
public:
    /**
     * Constucts new NumaMemoryResource object. 
     * ! All NumaMemoryResource objects are considered equal in terms of PMR equality !.
     *
     * @param do_pinning pin the current thread to the detected NUMA node when.
     * ! For thread will be used heuristic from its first context initialization (allocation call) !
     *
     * @param use_thread_cache use thread local cache.
     * ! For thread will be used heuristic from its first context initialization (allocation call) !
     */
    explicit NumaMemoryResource(bool do_pinning = false, bool use_thread_cache = true) noexcept
        : do_pinning_(do_pinning),
          use_thread_cache_(use_thread_cache)
    {}

protected:
    void* do_allocate(size_t bytes, size_t alignment) override {
        auto& context = ThreadNumaContext::current(do_pinning_, use_thread_cache_);
        return context.arena().allocate(bytes, alignment, context.small_cache());
    }

    void do_deallocate(void* p, size_t bytes, size_t alignment) override {
        if (!p) return;

        auto& context = ThreadNumaContext::current(do_pinning_, use_thread_cache_);
        context.arena().deallocate(p, context.small_cache());
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return dynamic_cast<const NumaMemoryResource*>(&other) != nullptr;
    }

private:
    bool do_pinning_;
    bool use_thread_cache_;
};

// TODO somehow slow down performance by ~20%
inline std::pmr::memory_resource* numa_memory_resource(
    bool do_pinning = false,
    bool use_thread_cache = true
) {
    static NumaMemoryResource default_resource(false, true);
    static NumaMemoryResource pinned_resource(true, true);
    static NumaMemoryResource no_cache_resource(false, false);
    static NumaMemoryResource pinned_no_cache_resource(true, false);

    if (do_pinning) {
        return use_thread_cache ? &pinned_resource : &pinned_no_cache_resource;
    }

    return use_thread_cache ? &default_resource : &no_cache_resource;
}