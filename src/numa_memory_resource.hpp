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
     *
     * @param use_thread_cache use thread local cache.
     */
    explicit numa_memory_resource(bool use_thread_cache = true)
        : use_thread_cache_(use_thread_cache) {
        ThreadLocalCache::configure_current(use_thread_cache_);
    }

    int node_id() const {
        return ThreadLocalCache::current().node_id();
    }

protected:
    void* do_allocate(size_t bytes, size_t alignment) override {
        auto& cache = ThreadLocalCache::current();
        return cache.allocate(bytes, alignment);
    }

    void do_deallocate(void* p, size_t bytes, size_t alignment) override {
        if (!p) return;

        auto& cache = ThreadLocalCache::current();
        cache.deallocate(p);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return dynamic_cast<const numa_memory_resource*>(&other) != nullptr;
    }

private:
    bool use_thread_cache_;
};

// TODO somehow slow down performance by ~20%
inline std::pmr::memory_resource* get_numa_memory_resource(bool use_thread_cache = true) {
    static numa_memory_resource default_resource(true);
    static numa_memory_resource no_cache_resource(false);

    ThreadLocalCache::configure_current(use_thread_cache);
    return use_thread_cache ? &default_resource : &no_cache_resource;
}