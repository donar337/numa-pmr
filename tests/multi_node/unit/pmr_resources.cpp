#include <cstddef>
#include <limits>
#include <memory_resource>
#include <new>

#include <catch2/catch_test_macros.hpp>
#include <numa.h>

#include "common/test_utils.hpp"
#include "numa_arena_memory_resource.hpp"
#include "numa_memory_resource.hpp"
#include "numa_simple_memory_resource.hpp"

namespace {

void exercise_small_zero_large_cycle(std::pmr::memory_resource& resource) {
    constexpr std::size_t small_size = 128;
    constexpr std::size_t large_size = SMALL_LARGE_THRESHOLD + 1;

    void* zero = resource.allocate(0, 1);
    REQUIRE(zero != nullptr);
    numa_test::touch_memory(zero, 1);
    resource.deallocate(zero, 0, 1);

    void* small = resource.allocate(small_size, alignof(std::max_align_t));
    REQUIRE(small != nullptr);
    numa_test::touch_memory(small, small_size);
    resource.deallocate(small, small_size, alignof(std::max_align_t));

    void* large = resource.allocate(large_size, alignof(std::max_align_t));
    REQUIRE(large != nullptr);
    numa_test::touch_memory(large, large_size);
    resource.deallocate(large, large_size, alignof(std::max_align_t));
}

} // namespace

// Verifies simple PMR resources through the public memory_resource API.
TEST_CASE("multi-node unit: simple PMR resource covers allocation modes", "[multi_node][unit][pmr][simple]") {
    const auto nodes = numa_test::two_test_nodes();

    numa_simple_memory_resource default_resource;
    exercise_small_zero_large_cycle(default_resource);

    for (int node : nodes) {
        numa_simple_memory_resource fixed_node_resource(node);
        exercise_small_zero_large_cycle(fixed_node_resource);
    }

    {
        numa_test::ScopedThreadPin pin(nodes[0]);
        auto current_node_resource = numa_simple_memory_resource::current_node_per_allocation();
        exercise_small_zero_large_cycle(current_node_resource);
    }

    numa_simple_memory_resource normalized_resource(numa_max_node() + 1);
    exercise_small_zero_large_cycle(normalized_resource);

    numa_simple_memory_resource other(nodes[1]);
    REQUIRE(default_resource.is_equal(other));
    REQUIRE_FALSE(default_resource.is_equal(*std::pmr::new_delete_resource()));

    REQUIRE_THROWS_AS(
        default_resource.allocate(std::numeric_limits<std::size_t>::max(), alignof(std::max_align_t)),
        std::bad_alloc
    );
}

// Verifies the main cached PMR resource and its static accessors in unit coverage.
TEST_CASE("multi-node unit: cached PMR resource covers cache modes", "[multi_node][unit][pmr][cache]") {
    numa_test::require_real_numa_system();

    numa_memory_resource cached_resource(true);
    exercise_small_zero_large_cycle(cached_resource);
    REQUIRE(cached_resource.node_id() >= 0);

    numa_memory_resource no_cache_resource(false);
    exercise_small_zero_large_cycle(no_cache_resource);

    auto* cached_singleton = get_numa_memory_resource(true);
    auto* no_cache_singleton = get_numa_memory_resource(false);
    REQUIRE(cached_singleton != nullptr);
    REQUIRE(no_cache_singleton != nullptr);
    REQUIRE(cached_singleton != no_cache_singleton);

    void* ptr = cached_singleton->allocate(256, alignof(std::max_align_t));
    REQUIRE(ptr != nullptr);
    numa_test::touch_memory(ptr, 256);
    cached_singleton->deallocate(ptr, 256, alignof(std::max_align_t));

    REQUIRE(cached_resource.is_equal(no_cache_resource));
    REQUIRE_FALSE(cached_resource.is_equal(*std::pmr::new_delete_resource()));
}

// Verifies standalone arena PMR resources, including normalized nodes and equality.
TEST_CASE("multi-node unit: arena PMR resource covers ownership modes", "[multi_node][unit][pmr][arena]") {
    const auto nodes = numa_test::two_test_nodes();

    {
        numa_test::ScopedThreadPin pin(nodes[0]);
        numa_arena_memory_resource default_resource(true);
        REQUIRE(default_resource.node_id() == nodes[0]);
        REQUIRE(default_resource.sync());
        exercise_small_zero_large_cycle(default_resource);
    }

    numa_arena_memory_resource no_sync_resource(nodes[1], false);
    REQUIRE(no_sync_resource.node_id() == nodes[1]);
    REQUIRE_FALSE(no_sync_resource.sync());
    exercise_small_zero_large_cycle(no_sync_resource);

    numa_arena_memory_resource normalized_resource(numa_max_node() + 1, true);
    REQUIRE(normalized_resource.node_id() >= 0);
    exercise_small_zero_large_cycle(normalized_resource);

    numa_arena_memory_resource other(nodes[0], true);
    REQUIRE(no_sync_resource.is_equal(no_sync_resource));
    REQUIRE_FALSE(no_sync_resource.is_equal(other));
    REQUIRE_FALSE(no_sync_resource.is_equal(*std::pmr::new_delete_resource()));
}
