#pragma once

#include <cstddef>
#include <memory_resource>
#include "thread_local/thread_local_cache.hpp"


/**
 * Main NUMA-aware PMR resource using ArenaManager and optional ThreadLocalCache.
 */
class numa_memory_resource : public std::pmr::memory_resource {
public:
    /**
     * Constructs new numa_memory_resource object.
     * ! All numa_memory_resource objects are considered equal in terms of PMR equality !.
     *
     * @param do_pinning pin the current thread to the detected NUMA node when.
     * ! For thread will be used heuristic from its first context initialization (allocation call) !
     *
     * @param use_thread_cache use thread local cache.
     * ! For thread will be used heuristic from its first context initialization (allocation call) !
     */
    explicit numa_memory_resource(bool do_pinning = false, bool use_thread_cache = true) noexcept
        : do_pinning_(do_pinning),
          use_thread_cache_(use_thread_cache)
    {}

protected:
    void* do_allocate(size_t bytes, size_t alignment) override {
        auto& cache = ThreadLocalCache::current(do_pinning_, use_thread_cache_);
        return cache.allocate(bytes, alignment);
    }

    void do_deallocate(void* p, size_t bytes, size_t alignment) override {
        if (!p) return;

        auto& cache = ThreadLocalCache::current(do_pinning_, use_thread_cache_);
        cache.deallocate(p);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return dynamic_cast<const numa_memory_resource*>(&other) != nullptr;
    }

private:
    bool do_pinning_;
    bool use_thread_cache_;
};

// TODO somehow slow down performance by ~20%
inline std::pmr::memory_resource* get_numa_memory_resource(
    bool do_pinning = false,
    bool use_thread_cache = true
) {
    static numa_memory_resource default_resource(false, true);
    static numa_memory_resource pinned_resource(true, true);
    static numa_memory_resource no_cache_resource(false, false);
    static numa_memory_resource pinned_no_cache_resource(true, false);

    if (do_pinning) {
        return use_thread_cache ? &pinned_resource : &pinned_no_cache_resource;
    }

    return use_thread_cache ? &default_resource : &no_cache_resource;
}