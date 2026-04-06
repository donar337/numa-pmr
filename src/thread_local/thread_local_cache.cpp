#include "thread_local/thread_local_cache.hpp"

#include "virtual_memory.hpp"

#include <new>

ThreadLocalCache::ThreadLocalCache(
    NumaArena& arena,
    int node_id,
    bool use_thread_cache
) noexcept
    : node_id_(node_id),
      use_thread_cache_(use_thread_cache),
      arena_(&arena),
      bins_{}
{}

ThreadLocalCache::~ThreadLocalCache() noexcept {
    flush();
}

ThreadLocalCache* ThreadLocalCache::create_on_current_node(
    bool do_pinning,
    bool use_thread_cache
) {
    auto& manager = NumaManager::instance();
    const int node_id = manager.current_node();

    if (do_pinning) {
        manager.pin_current_thread_to_node(node_id);
    }

    void* mem = VirtualMemory::reserve(sizeof(ThreadLocalCache));

    VirtualMemory::bind_to_node(
        mem,
        sizeof(ThreadLocalCache),
        node_id,
        VirtualMemory::NumaPolicy::Bind
    );

    try {
        return new (mem) ThreadLocalCache(
            manager.arena_for_node(node_id),
            node_id,
            use_thread_cache
        );
    } catch (...) {
        VirtualMemory::release(mem, sizeof(ThreadLocalCache));
        throw;
    }
}

void ThreadLocalCache::destroy(ThreadLocalCache* cache) noexcept {
    if (!cache) {
        return;
    }

    cache->~ThreadLocalCache();
    VirtualMemory::release(cache, sizeof(ThreadLocalCache));
}

void ThreadLocalCache::flush() noexcept {
    for (auto& bin : bins_) {
        while (bin.head) {
            BlockHeader* header = bin.head;
            auto** next = reinterpret_cast<BlockHeader**>(header->to_user_ptr());
            bin.head = *next;
            --bin.count;
            bin.bytes -= header->size_class;

            try {
                arena_->deallocate(header->to_user_ptr());
            } catch (...) {
            }
        }
    }
}

ThreadLocalCache& ThreadLocalCache::current() {
    return current(false, true);
}

ThreadLocalCache& ThreadLocalCache::current(bool do_pinning, bool use_thread_cache) {
    static thread_local ThreadNumaContextOwner owner(do_pinning, use_thread_cache);
    return owner.get();
}

ThreadNumaContextOwner::ThreadNumaContextOwner(bool do_pinning, bool use_thread_cache)
    : cache_(ThreadLocalCache::create_on_current_node(do_pinning, use_thread_cache))
{}

ThreadNumaContextOwner::~ThreadNumaContextOwner() noexcept {
    ThreadLocalCache::destroy(cache_);
}
