#pragma once

#include <sched.h>

#include <numa.h>

namespace numa_topology {

inline int current_node_from_cpu() noexcept {
    if (numa_available() < 0) {
        return 0;
    }

    const int cpu = sched_getcpu();
    if (cpu < 0) {
        return 0;
    }

    const int node = numa_node_of_cpu(cpu);
    return node >= 0 ? node : 0;
}

inline bool is_valid_node(int node_id) noexcept {
    if (node_id < 0) {
        return false;
    }

    if (numa_available() < 0) {
        return node_id == 0;
    }

    if (node_id > numa_max_node()) {
        return false;
    }

    return numa_all_nodes_ptr != nullptr &&
           numa_bitmask_isbitset(numa_all_nodes_ptr, static_cast<unsigned int>(node_id)) != 0;
}

inline int normalize_node_id(int node_id) noexcept {
    return is_valid_node(node_id) ? node_id : current_node_from_cpu();
}

inline bool pin_current_thread_to_node(int node_id) noexcept {
    if (!is_valid_node(node_id)) {
        return false;
    }

    if (numa_available() < 0) {
        return node_id == 0;
    }

    struct bitmask* cpus = numa_allocate_cpumask();
    if (!cpus) {
        return false;
    }

    const int ret = numa_node_to_cpus(node_id, cpus);
    if (ret != 0) {
        numa_free_cpumask(cpus);
        return false;
    }

    cpu_set_t affinity;
    CPU_ZERO(&affinity);

    bool has_cpu = false;
    for (unsigned int cpu = 0; cpu < cpus->size && cpu < CPU_SETSIZE; ++cpu) {
        if (numa_bitmask_isbitset(cpus, cpu)) {
            CPU_SET(static_cast<int>(cpu), &affinity);
            has_cpu = true;
        }
    }

    numa_free_cpumask(cpus);

    if (!has_cpu) {
        return false;
    }

    return sched_setaffinity(0, sizeof(affinity), &affinity) == 0;
}

} // namespace numa_topology
