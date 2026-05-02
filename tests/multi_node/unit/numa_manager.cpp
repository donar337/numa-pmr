#include <catch2/catch_test_macros.hpp>

#include "common/test_utils.hpp"
#include "numa_manager/numa_manager.hpp"

#include <stdexcept>

// Verifies that NumaManager discovers a real multi-node topology and exposes arenas.
TEST_CASE("multi-node unit: numa manager exposes multiple node arenas", "[multi_node][unit][numa_manager]") {
    const auto nodes = numa_test::two_test_nodes();
    auto& manager = NumaManager::instance();

    REQUIRE(manager.node_count() > nodes[1]);
    REQUIRE(manager.node_count() >= 2);

    for (int node : nodes) {
        REQUIRE(manager.arena_for_node(node).node_id() == node);
    }
}

// Verifies that invalid node IDs are rejected instead of silently falling back.
TEST_CASE("multi-node unit: numa manager rejects invalid arena IDs", "[multi_node][unit][numa_manager][invalid]") {
    numa_test::require_real_numa_system();
    auto& manager = NumaManager::instance();

    REQUIRE_THROWS_AS(manager.arena_for_node(-1), std::out_of_range);
    REQUIRE_THROWS_AS(manager.arena_for_node(manager.node_count()), std::out_of_range);
}

// Verifies that current_node follows CPU affinity changes across test nodes.
TEST_CASE("multi-node unit: numa manager reports current pinned node", "[multi_node][unit][numa_manager][pinning]") {
    const auto nodes = numa_test::two_test_nodes();
    auto& manager = NumaManager::instance();

    for (int node : nodes) {
        numa_test::ScopedThreadPin pin(node);
        numa_test::require_current_thread_on_node(node);
        REQUIRE(manager.current_node() == node);
    }
}

// Verifies manager-owned pinning succeeds for valid nodes and fails for invalid nodes.
TEST_CASE("multi-node unit: numa manager pins current thread to valid nodes", "[multi_node][unit][numa_manager][pinning]") {
    const auto nodes = numa_test::two_test_nodes();
    auto& manager = NumaManager::instance();
    numa_test::ScopedAffinityRestore affinity_restore;

    for (int node : nodes) {
        REQUIRE(manager.pin_current_thread_to_node(node));
        numa_test::require_current_thread_on_node(node);
    }

    REQUIRE_FALSE(manager.pin_current_thread_to_node(-1));
    REQUIRE_FALSE(manager.pin_current_thread_to_node(manager.node_count()));
}
