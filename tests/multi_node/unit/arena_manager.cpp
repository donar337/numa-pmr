#include <catch2/catch_test_macros.hpp>

#include "common/test_utils.hpp"
#include "arena_manager/arena_manager.hpp"
#include "numa_topology/numa_thread_pin_guard.hpp"

#include <stdexcept>

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
