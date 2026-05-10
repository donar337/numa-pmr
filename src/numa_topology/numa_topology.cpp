#include "numa_topology/numa_topology.hpp"
#include <sched.h>
#include <numa.h>
#include <thread>

NumaTopologyManager& NumaTopologyManager::instance() noexcept {
    static NumaTopologyManager inst;
    return inst;
}

NumaTopologyManager::NumaTopologyManager() {
    init_topology();
}

int NumaTopologyManager::current_node_from_cpu() const noexcept {
    const int cpu = sched_getcpu();

    if (cpu < 0 || cpu >= static_cast<int>(cpu_to_node_.size())) {
        return 0;
    }

    return cpu_to_node_[cpu];
}

int NumaTopologyManager::node_count() const noexcept {
    return node_count_;
}

bool NumaTopologyManager::is_valid_node(int node_id) const noexcept {
    if (node_id < 0 || node_id >= node_count_) {
        return false;
    }

    if (node_id >= static_cast<int>(node_present_.size())) {
        return false;
    }

    return node_present_[node_id];
}

int NumaTopologyManager::normalize_node_id(int node_id) const noexcept {
    return is_valid_node(node_id) ? node_id : current_node_from_cpu();
}

bool NumaTopologyManager::pin_current_thread_to_node(int node_id) const noexcept {
    if (!is_valid_node(node_id)) {
        return false;
    }

    if (node_id >= static_cast<int>(node_to_cpus_.size())) {
        return false;
    }

    return apply_affinity_from_cpus(node_to_cpus_[node_id]);
}

bool NumaTopologyManager::unpin_current_thread() const noexcept {
    return apply_affinity_to_all_cpus(cpu_count_);
}

void NumaTopologyManager::init_topology() noexcept {
    if (numa_available() < 0) {
        init_single_node_topology();
        return;
    }

    cpu_count_ = numa_num_configured_cpus();
    if (cpu_count_ <= 0) {
        cpu_count_ = 1;
    }

    const int max_node = numa_max_node();
    node_count_ = max_node >= 0 ? max_node + 1 : 1;
    if (node_count_ <= 0) {
        node_count_ = 1;
    }

    cpu_to_node_.assign(cpu_count_, 0);
    node_to_cpus_.assign(node_count_, {});
    node_present_.assign(node_count_, false);

    if (numa_all_nodes_ptr != nullptr) {
        for (int node = 0; node < node_count_; ++node) {
            node_present_[node] =
                numa_bitmask_isbitset(numa_all_nodes_ptr, static_cast<unsigned int>(node)) != 0;
        }
    } else {
        for (int node = 0; node < node_count_; ++node) {
            node_present_[node] = true;
        }
    }

    for (int cpu = 0; cpu < cpu_count_; ++cpu) {
        int node = numa_node_of_cpu(cpu);
        if (node < 0 || node >= node_count_ || !node_present_[node]) {
            node = 0;
        }

        cpu_to_node_[cpu] = node;
        node_to_cpus_[node].push_back(cpu);
        node_present_[node] = true;
    }
}

void NumaTopologyManager::init_single_node_topology() noexcept {
    cpu_count_ = static_cast<int>(std::thread::hardware_concurrency());
    if (cpu_count_ <= 0) {
        cpu_count_ = 1;
    }

    node_count_ = 1;
    cpu_to_node_.assign(cpu_count_, 0);
    node_to_cpus_.assign(1, {});
    node_present_.assign(1, true);

    for (int cpu = 0; cpu < cpu_count_; ++cpu) {
        node_to_cpus_[0].push_back(cpu);
    }
}

bool NumaTopologyManager::apply_affinity_from_cpus(const std::vector<int>& cpus) noexcept {
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

bool NumaTopologyManager::apply_affinity_to_all_cpus(int cpu_count) noexcept {
    if (cpu_count <= 0) {
        return false;
    }

    cpu_set_t affinity;
    CPU_ZERO(&affinity);

    const int limit = cpu_count < CPU_SETSIZE ? cpu_count : CPU_SETSIZE;
    for (int cpu = 0; cpu < limit; ++cpu) {
        CPU_SET(cpu, &affinity);
    }

    return sched_setaffinity(0, sizeof(affinity), &affinity) == 0;
}
