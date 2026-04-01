#include "numa_arena.hpp"

#include <new>
#include <stdexcept>
#include <thread>

#include <numa.h>

// ============================================================
// NumaArena
// ============================================================

void NumaArenaDeleter::operator()(NumaArena* arena) const noexcept {
    if (!arena) return;

    arena->~NumaArena();
    VirtualMemory::release(arena, sizeof(NumaArena));
}

void* NumaArena::allocate(size_t size, size_t alignment) {
    auto& context = ThreadNumaContext::current();
    return allocate(size, alignment, context.small_cache());
}

void NumaArena::deallocate(void* ptr) {
    auto& context = ThreadNumaContext::current();
    deallocate(ptr, &context.small_cache());
}

void NumaArena::deallocate(void* ptr, ThreadLocalSmallCache* cache) {
    if (!ptr) return;

    auto* header = BlockHeader::from_user_ptr(ptr);

    if (static_cast<int>(header->node_id) != node_id_) {
        // TODO foreign freelist
        NumaManager::instance()
            .arena_for_node(static_cast<int>(header->node_id))
            .deallocate(ptr, nullptr);
        return;
    }

    if (header->size_class != 0) {
        size_t class_index = SizeClassTable::class_index_for_size(header->size_class);
        if (cache && cache->push(header, class_index)) {
            return;
        }

        small_.deallocate(header);
    } else {
        large_.deallocate(ptr);
    }
}

// ============================================================
// NumaManager
// ============================================================

NumaManager::NumaManager() {
    init_topology();
    init_arenas();
}

void NumaManager::init_topology() {
    if (numa_available() < 0) {
        init_single_node_topology();
        return;
    }

    cpu_count_ = numa_num_configured_cpus();

    if (cpu_count_ == 0)
        cpu_count_ = 1;

    int max_node = numa_max_node();
    node_count_ = max_node >= 0 ? max_node + 1 : 1;

    if (node_count_ == 0)
        node_count_ = 1;

    cpu_to_node_.resize(cpu_count_, 0);
    node_to_cpus_.assign(node_count_, {});

    for (int cpu = 0; cpu < cpu_count_; ++cpu) {
        int node = numa_node_of_cpu(cpu);

        if (node >= 0 && node < node_count_) {
            cpu_to_node_[cpu] = node;
        }

        node_to_cpus_[cpu_to_node_[cpu]].push_back(cpu);
    }
}

void NumaManager::init_single_node_topology() {
    cpu_count_ = std::thread::hardware_concurrency();

    if (cpu_count_ == 0)
        cpu_count_ = 1;

    node_count_ = 1;
    cpu_to_node_.assign(cpu_count_, 0);
    node_to_cpus_.assign(node_count_, {});

    for (int cpu = 0; cpu < cpu_count_; ++cpu) {
        node_to_cpus_[0].push_back(cpu);
    }
}

void NumaManager::init_arenas() {
    arenas_.reserve(node_count_);

    for (int i = 0; i < node_count_; ++i) {
        arenas_.emplace_back(create_arena_on_node(i));
    }
}

NumaArenaPtr NumaManager::create_arena_on_node(int node_id) {
    void* mem = VirtualMemory::reserve(sizeof(NumaArena));

    VirtualMemory::bind_to_node(
        mem,
        sizeof(NumaArena),
        node_id,
        VirtualMemory::NumaPolicy::Bind
    );

    try {
        return NumaArenaPtr(new (mem) NumaArena(node_id));
    } catch (...) {
        VirtualMemory::release(mem, sizeof(NumaArena));
        throw;
    }
}

bool NumaManager::pin_current_thread_to_node(int node_id) const noexcept {
    if (node_id < 0 || node_id >= node_count_) {
        return false;
    }

    const auto& cpus = node_to_cpus_[node_id];
    if (cpus.empty()) {
        return false;
    }

    cpu_set_t affinity;
    CPU_ZERO(&affinity);

    bool has_cpu = false;
    for (int cpu : cpus) {
        if (cpu >= 0 && cpu < CPU_SETSIZE) {
            CPU_SET(cpu, &affinity);
            has_cpu = true;
        }
    }

    if (!has_cpu) {
        return false;
    }

    return sched_setaffinity(0, sizeof(affinity), &affinity) == 0;
}

ThreadLocalSmallCache* NumaManager::create_thread_cache_on_node(int node_id) {
    void* mem = VirtualMemory::reserve(sizeof(ThreadLocalSmallCache));

    VirtualMemory::bind_to_node(
        mem,
        sizeof(ThreadLocalSmallCache),
        node_id,
        VirtualMemory::NumaPolicy::Bind
    );

    try {
        return new (mem) ThreadLocalSmallCache();
    } catch (...) {
        VirtualMemory::release(mem, sizeof(ThreadLocalSmallCache));
        throw;
    }
}

ThreadNumaContext::ThreadNumaContext(
    NumaManager& manager,
    int node_id,
    bool use_thread_cache
)
    : node_id_(node_id),
      use_thread_cache_(use_thread_cache),
      arena_(nullptr),
      small_cache_(nullptr) {
    arena_ = &manager.arena_for_node(node_id_);
    // TODO: use_thread_cache_ is stored for the future cache bypass path.
    small_cache_ = manager.create_thread_cache_on_node(node_id_);
}

ThreadNumaContext::~ThreadNumaContext() noexcept {
    if (!small_cache_) return;

    small_cache_->flush(*arena_);
    small_cache_->~ThreadLocalSmallCache();
    VirtualMemory::release(small_cache_, sizeof(ThreadLocalSmallCache));
}

ThreadNumaContext* ThreadNumaContext::create_on_current_node(
    NumaManager& manager,
    bool do_pinning,
    bool use_thread_cache
) {
    int node_id = manager.current_node();
    if (do_pinning) {
        manager.pin_current_thread_to_node(node_id);
    }

    void* mem = VirtualMemory::reserve(sizeof(ThreadNumaContext));

    VirtualMemory::bind_to_node(
        mem,
        sizeof(ThreadNumaContext),
        node_id,
        VirtualMemory::NumaPolicy::Bind
    );

    try {
        return new (mem) ThreadNumaContext(manager, node_id, use_thread_cache);
    } catch (...) {
        VirtualMemory::release(mem, sizeof(ThreadNumaContext));
        throw;
    }
}

void ThreadNumaContext::destroy(ThreadNumaContext* context) noexcept {
    if (!context) return;

    context->~ThreadNumaContext();
    VirtualMemory::release(context, sizeof(ThreadNumaContext));
}

class ThreadNumaContextOwner {
public:
    ThreadNumaContextOwner(bool do_pinning, bool use_thread_cache)
        : context_(ThreadNumaContext::create_on_current_node(
              NumaManager::instance(),
              do_pinning,
              use_thread_cache
          ))
    {}

    ~ThreadNumaContextOwner() noexcept {
        ThreadNumaContext::destroy(context_);
    }

    ThreadNumaContext& get() noexcept {
        return *context_;
    }

private:
    ThreadNumaContext* context_;
};

// HONORABLE MENTION due to "first in thread wins" policy arguments will be ignored.
ThreadNumaContext& ThreadNumaContext::current() {
    return current(false, true);
}

ThreadNumaContext& ThreadNumaContext::current(bool do_pinning, bool use_thread_cache) {
    static thread_local ThreadNumaContextOwner owner(do_pinning, use_thread_cache);
    return owner.get();
}
