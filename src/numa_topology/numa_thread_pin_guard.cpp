#include "numa_topology/numa_thread_pin_guard.hpp"

numa_thread_pin_guard::numa_thread_pin_guard(int node_id) noexcept
    : node_id_(node_id),
      previous_affinity_(NumaTopologyManager::instance().pin_current_thread_to_node(node_id)),
      active_(CPU_COUNT(&previous_affinity_) != 0)
{}

numa_thread_pin_guard::~numa_thread_pin_guard() noexcept {
    restore();
}

bool numa_thread_pin_guard::restore() noexcept {
    if (!active_) {
        return false;
    }

    active_ = false;
    return NumaTopologyManager::instance().set_affinity(previous_affinity_);
}
