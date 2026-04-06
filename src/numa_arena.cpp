#include "numa_arena.hpp"

#include <new>
#include <stdexcept>
#include <thread>
#include <cstdint>

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
    if (size == 0) {
        size = 1;
    }

    if (size <= SMALL_LARGE_THRESHOLD && alignment <= ALIGNMENT) {
        size_t class_index = SizeClassTable::class_index_for_size(size);
        size_t class_size = kClassSizes[class_index];
        auto* header = reinterpret_cast<BlockHeader*>(
            small_.allocate_by_class_index(class_index)
        );

        header->node_id = static_cast<std::uint32_t>(node_id_);
        header->size_class = static_cast<std::uint32_t>(class_size);
        header->size = 0;

        return header->to_user_ptr();
    }

    return large_.allocate(size, alignment);
}

void NumaArena::deallocate(void* ptr) {
    if (!ptr) return;

    auto* header = BlockHeader::from_user_ptr(ptr);

    if (static_cast<int>(header->node_id) != node_id_) {
        // TODO foreign freelist
        NumaManager::instance()
            .arena_for_node(static_cast<int>(header->node_id))
            .deallocate(ptr);
        return;
    }

    if (header->size_class != 0) {
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
