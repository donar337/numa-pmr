#include "thread_local/thread_local_cache.hpp"

#include "arena_manager/arena_manager.hpp"
#include "virtual_memory/virtual_memory.hpp"

#include <new>

namespace {

ThreadNumaContextOwner& current_owner() {
    static thread_local ThreadNumaContextOwner owner;
    return owner;
}

} // namespace

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
    return current_owner().get();
}

void ThreadLocalCache::configure_current(bool use_thread_cache) {
    current_owner().configure(use_thread_cache);
}

ThreadNumaContextOwner::~ThreadNumaContextOwner() noexcept {
    ThreadLocalCache::destroy(cache_);
}

ThreadLocalCache& ThreadNumaContextOwner::get() {
    ensure_cache(true);
    return *cache_;
}

void ThreadNumaContextOwner::configure(bool use_thread_cache) {
    ensure_cache(use_thread_cache);

    const int current_node = NumaTopologyManager::instance().current_node_from_cpu();
    if (cache_->node_id() == current_node) {
        cache_->set_use_thread_cache(use_thread_cache);
        return;
    }

    ThreadLocalCache* replacement = ThreadLocalCache::create_on_current_node(use_thread_cache);
    ThreadLocalCache::destroy(cache_);
    cache_ = replacement;
}

void ThreadNumaContextOwner::ensure_cache(bool use_thread_cache) {
    if (!cache_) {
        cache_ = ThreadLocalCache::create_on_current_node(use_thread_cache);
    }
}
