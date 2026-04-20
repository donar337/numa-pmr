#include "numa_manager/numa_manager.hpp"

#include "virtual_memory/virtual_memory.hpp"

#include <new>
#include <thread>

#include <numa.h>

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
        arenas_.emplace_back(create_arena_on_node(i, node_count_ > 1));
    }
}

NumaArenaPtr NumaManager::create_arena_on_node(int node_id, bool foreign_freelist_enabled) {
    void* mem = VirtualMemory::alloc_on_node(sizeof(NumaArena), node_id);

    try {
        return NumaArenaPtr(new (mem) NumaArena(node_id, foreign_freelist_enabled));
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
