#pragma once

#include <vector>

class NumaTopologyManager {
public:
    static NumaTopologyManager& instance() noexcept;

    int current_node_from_cpu() const noexcept;
    int node_count() const noexcept;
    bool is_valid_node(int node_id) const noexcept;
    int normalize_node_id(int node_id) const noexcept;
    bool pin_current_thread_to_node(int node_id) const noexcept;
    bool unpin_current_thread() const noexcept;

private:
    NumaTopologyManager();

    void init_topology() noexcept;
    void init_single_node_topology() noexcept;

    static bool apply_affinity_from_cpus(const std::vector<int>& cpus) noexcept;
    static bool apply_affinity_to_all_cpus(int cpu_count) noexcept;

    int cpu_count_ = 1;
    int node_count_ = 1;
    std::vector<int> cpu_to_node_;
    std::vector<std::vector<int>> node_to_cpus_;
    std::vector<bool> node_present_;
};
