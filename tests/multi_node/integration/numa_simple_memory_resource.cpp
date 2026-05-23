#include <atomic>
#include <memory_resource>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <numa.h>

#include "common/test_utils.hpp"
#include "numa_simple_memory_resource.hpp"

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
    for (int i = 0; i < 512; ++i) {
        values.push_back(i);
    }
    REQUIRE(values.size() == 512);
    REQUIRE(values.back() == 511);

    std::pmr::unordered_map<int, std::pmr::string> rows(resource);
    rows.reserve(64);
    for (int i = 0; i < 64; ++i) {
        std::pmr::string payload(resource);
        payload.append(static_cast<std::size_t>((i % 17) + 1), static_cast<char>('a' + (i % 26)));
        rows.emplace(i, std::move(payload));
    }

    REQUIRE(rows.size() == 64);
    REQUIRE(rows.at(16).size() == 17);
}

} // namespace

// Verifies direct allocation, zero-size, and alignment cycles on fixed-node simple resources.
TEST_CASE("multi-node integration: numa_simple_memory_resource handles direct cycles on each node", "[multi_node][integration][simple_pmr]") {
    const auto nodes = numa_test::two_test_nodes();

    for (int node : nodes) {
        numa_simple_memory_resource resource(node);
        exercise_direct_allocations(resource);
        exercise_alignment(resource);
    }
}

// Verifies standard PMR containers can use simple resources created for different nodes.
TEST_CASE("multi-node integration: numa_simple_memory_resource works with PMR containers", "[multi_node][integration][simple_pmr]") {
    const auto nodes = numa_test::two_test_nodes();

    for (int node : nodes) {
        numa_simple_memory_resource resource(node);
        exercise_pmr_containers(&resource);
    }
}

// Verifies current-node-per-allocation mode follows pinned worker threads.
TEST_CASE("multi-node integration: numa_simple_memory_resource supports current-node allocation mode", "[multi_node][integration][simple_pmr][pinning]") {
    const auto nodes = numa_test::two_test_nodes();
    std::atomic<bool> failed{false};

    for (int node : nodes) {
        std::thread worker([node, &failed] {
            try {
                numa_test::ScopedThreadPin pin(node);
                numa_test::require_current_thread_on_node(node);
                auto resource = numa_simple_memory_resource::current_node_per_allocation();
                exercise_direct_allocations(resource);
                exercise_pmr_containers(&resource);
            } catch (...) {
                failed.store(true);
            }
        });
        worker.join();
    }

    REQUIRE_FALSE(failed.load());
}

// Verifies invalid node construction falls back to a usable current-node resource.
TEST_CASE("multi-node integration: numa_simple_memory_resource normalizes invalid nodes", "[multi_node][integration][simple_pmr][invalid]") {
    numa_test::require_real_numa_system();

    numa_simple_memory_resource resource(numa_max_node() + 1);
    exercise_direct_allocations(resource);
    exercise_pmr_containers(&resource);
}

// Verifies simple resource PMR equality is type-based.
TEST_CASE("multi-node integration: numa_simple_memory_resource instances compare equal", "[multi_node][integration][simple_pmr][equality]") {
    const auto nodes = numa_test::two_test_nodes();
    numa_simple_memory_resource first(nodes[0]);
    numa_simple_memory_resource second(nodes[1]);

    REQUIRE(first.is_equal(second));
}
