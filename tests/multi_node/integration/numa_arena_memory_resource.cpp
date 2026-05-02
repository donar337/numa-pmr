#include <catch2/catch_test_macros.hpp>

#include "common/test_utils.hpp"
#include "numa_arena_memory_resource.hpp"
#include "numa_memory_resource.hpp"

#include <atomic>
#include <memory_resource>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>


namespace {

void exercise_direct_allocations(std::pmr::memory_resource& resource) {
    for (std::size_t size : numa_test::mixed_sizes()) {
        numa_test::allocate_touch_free(resource, size);
    }
}

void exercise_alignment(std::pmr::memory_resource& resource) {
    constexpr std::size_t size = 128;
    constexpr std::size_t alignment = 64;

    void* ptr = resource.allocate(size, alignment);
    REQUIRE(numa_test::is_aligned(ptr, alignment));
    numa_test::touch_memory(ptr, size);
    resource.deallocate(ptr, size, alignment);
}

void exercise_pmr_containers(std::pmr::memory_resource* resource) {
    std::pmr::vector<int> values(resource);
    std::pmr::unordered_map<int, std::pmr::string> rows(resource);

    for (int i = 0; i < 512; ++i) {
        values.push_back(i);
    }
    rows.reserve(64);
    for (int i = 0; i < 64; ++i) {
        std::pmr::string payload(resource);
        payload.append(static_cast<std::size_t>((i % 23) + 1), static_cast<char>('a' + (i % 26)));
        rows.emplace(i, std::move(payload));
    }

    REQUIRE(values.size() == 512);
    REQUIRE(rows.size() == 64);
    REQUIRE(rows.at(22).size() == 23);
}

} // namespace

// Verifies standalone arena resources can be constructed for explicit NUMA nodes.
TEST_CASE("multi-node integration: numa_arena_memory_resource constructs per node", "[multi_node][integration][arena_pmr]") {
    const auto nodes = numa_test::two_test_nodes();

    for (int node : nodes) {
        numa_arena_memory_resource resource(node);
        REQUIRE(resource.node_id() == node);
        REQUIRE(resource.sync());
        exercise_direct_allocations(resource);
        exercise_alignment(resource);
    }
}

// Verifies PMR containers operate on standalone arena resources for multiple nodes.
TEST_CASE("multi-node integration: numa_arena_memory_resource works with PMR containers", "[multi_node][integration][arena_pmr]") {
    const auto nodes = numa_test::two_test_nodes();

    for (int node : nodes) {
        numa_arena_memory_resource resource(node);
        exercise_pmr_containers(&resource);
    }
}

// Verifies no-sync arena resources remain valid for single-threaded ownership.
TEST_CASE("multi-node integration: numa_arena_memory_resource supports no-sync mode", "[multi_node][integration][arena_pmr][no_sync]") {
    const auto nodes = numa_test::two_test_nodes();

    for (int node : nodes) {
        numa_arena_memory_resource resource(node, false);
        REQUIRE(resource.node_id() == node);
        REQUIRE_FALSE(resource.sync());

        exercise_direct_allocations(resource);
        exercise_pmr_containers(&resource);
    }
}

// Verifies do_pinning construction leaves the creating thread on the selected NUMA node.
TEST_CASE("multi-node integration: numa_arena_memory_resource can pin constructing thread", "[multi_node][integration][arena_pmr][pinning]") {
    const auto nodes = numa_test::two_test_nodes();

    for (int node : nodes) {
        numa_test::ScopedThreadPin pin(node);
        numa_arena_memory_resource resource(node, true, true);
        REQUIRE(resource.node_id() == node);
        numa_test::require_current_thread_on_node(node);
        exercise_direct_allocations(resource);
    }
}

// Verifies synchronized arena resources tolerate concurrent PMR allocation cycles.
TEST_CASE("multi-node integration: numa_arena_memory_resource sync mode supports concurrent cycles", "[multi_node][integration][arena_pmr][thread]") {
    const auto nodes = numa_test::two_test_nodes();

    for (int node : nodes) {
        numa_arena_memory_resource resource(node, true);
        std::atomic<bool> failed{false};
        std::vector<std::thread> threads;

        for (int thread_id = 0; thread_id < 4; ++thread_id) {
            threads.emplace_back([&resource, &failed, thread_id] {
                try {
                    for (std::size_t i = 0; i < 256; ++i) {
                        const auto sizes = numa_test::mixed_sizes();
                        const std::size_t size = sizes[(i + static_cast<std::size_t>(thread_id)) % sizes.size()];
                        numa_test::allocate_touch_free(resource, size);
                    }
                } catch (...) {
                    failed.store(true);
                }
            });
        }

        for (auto& thread : threads) {
            thread.join();
        }

        REQUIRE_FALSE(failed.load());
    }
}

// Verifies arena resource equality is identity-based and distinct from main NUMA resource.
TEST_CASE("multi-node integration: numa_arena_memory_resource compares by identity", "[multi_node][integration][arena_pmr][equality]") {
    const auto nodes = numa_test::two_test_nodes();
    numa_arena_memory_resource first(nodes[0]);
    numa_arena_memory_resource second(nodes[1]);
    numa_memory_resource numa_resource;

    REQUIRE(first.is_equal(first));
    REQUIRE_FALSE(first.is_equal(second));
    REQUIRE_FALSE(first.is_equal(numa_resource));
    REQUIRE_FALSE(numa_resource.is_equal(first));
}
