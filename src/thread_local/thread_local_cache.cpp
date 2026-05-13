#include "thread_local/thread_local_cache.hpp"

#include "arena_manager/arena_manager.hpp"
#include "virtual_memory/virtual_memory.hpp"

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

ThreadLocalCache* ThreadLocalCache::create_on_current_node(bool use_thread_cache) {
    auto& manager = ArenaManager::instance();
    auto& topology = NumaTopologyManager::instance();
    const int node_id = topology.current_node_from_cpu();

    void* mem = VirtualMemory::alloc_on_node(sizeof(ThreadLocalCache), node_id);

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
    return current(true);
}

ThreadLocalCache& ThreadLocalCache::current(bool use_thread_cache) {
    static thread_local ThreadNumaContextOwner owner(use_thread_cache);
    return owner.get();
}

ThreadNumaContextOwner::ThreadNumaContextOwner(bool use_thread_cache)
    : cache_(ThreadLocalCache::create_on_current_node(use_thread_cache))
{}

ThreadNumaContextOwner::~ThreadNumaContextOwner() noexcept {
    ThreadLocalCache::destroy(cache_);
}
