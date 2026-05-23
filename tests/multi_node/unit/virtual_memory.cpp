#include <new>

#include <catch2/catch_test_macros.hpp>
#include <numa.h>

#include "common/test_utils.hpp"
#include "virtual_memory/virtual_memory.hpp"

// Verifies basic mapping behavior and page-aligned writable reservations.
TEST_CASE("multi-node unit: virtual memory reserves writable pages", "[multi_node][unit][virtual_memory]") {
    numa_test::require_real_numa_system();

    REQUIRE(VirtualMemory::align_up(0, 4096) == 0);
    REQUIRE(VirtualMemory::align_up(1, 4096) == 4096);
    REQUIRE(VirtualMemory::align_up(4096, 4096) == 4096);
    REQUIRE(VirtualMemory::align_up(4097, 4096) == 8192);

    void* page = VirtualMemory::reserve(0);
    REQUIRE(page != nullptr);
    REQUIRE(numa_test::is_aligned(page, VirtualMemory::page_size()));

    numa_test::touch_memory(page, 1);
    VirtualMemory::release(page, VirtualMemory::page_size());
}

// Verifies that alloc_on_node binds faulted pages to each requested NUMA node.
TEST_CASE("multi-node unit: virtual memory allocates physical pages on requested nodes", "[multi_node][unit][virtual_memory][placement]") {
    const auto nodes = numa_test::two_test_nodes();
    const std::size_t size = VirtualMemory::page_size() * 4;

    for (int node : nodes) {
        void* ptr = VirtualMemory::alloc_on_node(size, node);
        REQUIRE(ptr != nullptr);
        numa_test::require_pages_on_node(ptr, size, node);
        VirtualMemory::release(ptr, size);
    }
}

// Verifies that binding an existing mapping changes where newly faulted pages land.
TEST_CASE("multi-node unit: virtual memory bind_to_node applies required placement", "[multi_node][unit][virtual_memory][placement]") {
    const auto nodes = numa_test::two_test_nodes();
    const std::size_t size = VirtualMemory::page_size() * 2;

    for (int node : nodes) {
        void* ptr = VirtualMemory::reserve(size);
        REQUIRE(ptr != nullptr);
        REQUIRE(VirtualMemory::bind_to_node(ptr, size, node));
        numa_test::require_pages_on_node(ptr, size, node);
        VirtualMemory::release(ptr, size);
    }
}

// Verifies best-effort policies and advisory helpers that do not affect ownership.
TEST_CASE("multi-node unit: virtual memory supports policies and advisory calls", "[multi_node][unit][virtual_memory][policy]") {
    const auto nodes = numa_test::two_test_nodes();
    const std::size_t size = VirtualMemory::page_size() * 2;

    REQUIRE(VirtualMemory::bind_to_node(nullptr, size, nodes[0], VirtualMemory::NumaPolicy::FirstTouch));

    void* ptr = VirtualMemory::reserve(size);
    REQUIRE(ptr != nullptr);

    REQUIRE(VirtualMemory::bind_to_node(ptr, size, nodes[0], VirtualMemory::NumaPolicy::FirstTouch));
    (void)VirtualMemory::bind_to_node(ptr, size, nodes[0], VirtualMemory::NumaPolicy::Interleave);

    VirtualMemory::advise_hugepage(nullptr, size);
    VirtualMemory::advise_no_hugepage(nullptr, size);
    VirtualMemory::advise_release(nullptr, size);

    VirtualMemory::advise_hugepage(ptr, 0);
    VirtualMemory::advise_no_hugepage(ptr, size);
    VirtualMemory::advise_release(ptr, size);

    VirtualMemory::release(nullptr, size);
    VirtualMemory::release(ptr, 0);
    VirtualMemory::release(ptr, size);
}

// Verifies mandatory failure paths for invalid bindings and allocation targets.
TEST_CASE("multi-node unit: virtual memory rejects invalid NUMA bindings", "[multi_node][unit][virtual_memory][invalid]") {
    numa_test::require_real_numa_system();

    const int invalid_node = numa_max_node() + 1;
    void* ptr = VirtualMemory::reserve(VirtualMemory::page_size());
    REQUIRE(ptr != nullptr);

    REQUIRE_FALSE(VirtualMemory::bind_to_node(nullptr, VirtualMemory::page_size(), 0));
    REQUIRE_FALSE(VirtualMemory::bind_to_node(ptr, VirtualMemory::page_size(), invalid_node));
    REQUIRE_THROWS_AS(VirtualMemory::alloc_on_node(VirtualMemory::page_size(), invalid_node), std::bad_alloc);

    VirtualMemory::release(ptr, VirtualMemory::page_size());
}
