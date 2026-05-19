#include <catch2/catch_test_macros.hpp>

#define private public
#include "arena_manager/arena_manager.hpp"
#include "numa_topology/numa_thread_pin_guard.hpp"
#undef private

#include "common/test_utils.hpp"

#include <stdexcept>
#include <utility>
#include <vector>

// Verifies that ArenaManager discovers a real multi-node topology and exposes arenas.
TEST_CASE("multi-node unit: arena manager exposes multiple node arenas", "[multi_node][unit][arena_manager]") {
    const auto nodes = numa_test::two_test_nodes();
    auto& manager = ArenaManager::instance();
    auto& topology = NumaTopologyManager::instance();

    REQUIRE(topology.node_count() > nodes[1]);
    REQUIRE(topology.node_count() >= 2);

    for (int node : nodes) {
        REQUIRE(manager.arena_for_node(node).node_id() == node);
    }
}

// Verifies that invalid node IDs are rejected instead of silently falling back.
TEST_CASE(
    "multi-node unit: arena manager rejects invalid arena IDs",
    "[multi_node][unit][arena_manager][invalid]"
) {
    numa_test::require_real_numa_system();
    auto& manager = ArenaManager::instance();
    auto& topology = NumaTopologyManager::instance();

    REQUIRE_THROWS_AS(manager.arena_for_node(-1), std::out_of_range);
    REQUIRE_THROWS_AS(manager.arena_for_node(topology.node_count()), std::out_of_range);
}

// Verifies that current_node follows CPU affinity changes across test nodes.
TEST_CASE(
    "multi-node unit: arena manager reports current pinned node",
    "[multi_node][unit][arena_manager][pinning]"
) {
    const auto nodes = numa_test::two_test_nodes();
    auto& topology = NumaTopologyManager::instance();

    for (int node : nodes) {
        numa_test::ScopedThreadPin pin(node);
        numa_test::require_current_thread_on_node(node);
        REQUIRE(topology.current_node_from_cpu() == node);
    }
}

// Verifies topology-managed pinning succeeds for valid nodes and fails for invalid nodes.
TEST_CASE(
    "multi-node unit: arena manager pins current thread to valid nodes",
    "[multi_node][unit][arena_manager][pinning]"
) {
    const auto nodes = numa_test::two_test_nodes();
    auto& topology = NumaTopologyManager::instance();
    numa_test::ScopedAffinityRestore affinity_restore;

    for (int node : nodes) {
        const cpu_set_t previous = topology.pin_current_thread_to_node(node);
        REQUIRE(CPU_COUNT(&previous) != 0);
        numa_test::require_current_thread_on_node(node);
    }

    {
        cpu_set_t bad_set = topology.pin_current_thread_to_node(-1);
        REQUIRE_FALSE(CPU_COUNT(&bad_set) != 0);
    }
    {
        cpu_set_t bad_set = topology.pin_current_thread_to_node(topology.node_count());
        REQUIRE_FALSE(CPU_COUNT(&bad_set) != 0);
    }
}

// Verifies normalization, explicit affinity restore, and empty affinity rejection paths.
TEST_CASE(
    "multi-node unit: topology normalizes nodes and restores affinity",
    "[multi_node][unit][arena_manager][topology]"
) {
    const auto nodes = numa_test::two_test_nodes();
    auto& topology = NumaTopologyManager::instance();
    numa_test::ScopedAffinityRestore affinity_restore;

    REQUIRE(topology.is_valid_node(nodes[0]));
    REQUIRE_FALSE(topology.is_valid_node(-1));
    REQUIRE_FALSE(topology.is_valid_node(topology.node_count()));
    REQUIRE(topology.normalize_node_id(nodes[0]) == nodes[0]);

    {
        numa_test::ScopedThreadPin pin(nodes[1]);
        numa_test::require_current_thread_on_node(nodes[1]);
        REQUIRE(topology.normalize_node_id(-1) == nodes[1]);
    }

    cpu_set_t empty;
    CPU_ZERO(&empty);
    REQUIRE_FALSE(topology.set_affinity(empty));
    REQUIRE_FALSE(NumaTopologyManager::apply_affinity_from_cpus({}));
    REQUIRE_FALSE(NumaTopologyManager::apply_affinity_from_cpus(std::vector<int>{-1, CPU_SETSIZE}));
    REQUIRE_FALSE(NumaTopologyManager::apply_affinity_to_all_cpus(0));

    auto saved_node_to_cpus = topology.node_to_cpus_;
    topology.node_to_cpus_.clear();
    cpu_set_t missing_cpu_mapping = topology.pin_current_thread_to_node(nodes[0]);
    REQUIRE_FALSE(CPU_COUNT(&missing_cpu_mapping) != 0);
    topology.node_to_cpus_ = std::move(saved_node_to_cpus);

    REQUIRE(topology.unpin_current_thread());
}

// Verifies the public scoped pin guard restores the previous affinity on scope exit.
TEST_CASE(
    "multi-node unit: thread pin guard restores affinity",
    "[multi_node][unit][arena_manager][pinning]"
) {
    const auto nodes = numa_test::two_test_nodes();
    cpu_set_t previous;
    CPU_ZERO(&previous);
    REQUIRE(sched_getaffinity(0, sizeof(previous), &previous) == 0);

    {
        numa_thread_pin_guard pin(nodes[0]);
        REQUIRE(pin.pinned());
        REQUIRE(pin.node_id() == nodes[0]);
        numa_test::require_current_thread_on_node(nodes[0]);
    }

    cpu_set_t restored;
    CPU_ZERO(&restored);
    REQUIRE(sched_getaffinity(0, sizeof(restored), &restored) == 0);

    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        REQUIRE(CPU_ISSET(cpu, &restored) == CPU_ISSET(cpu, &previous));
    }
}
