#include <catch2/catch_test_macros.hpp>

#include "common/test_utils.hpp"
#include "numa_arena/numa_arena.hpp"
#include "numa_manager/numa_manager.hpp"

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

// Verifies that arenas on multiple nodes route small, zero, large, and over-aligned requests correctly.
TEST_CASE("multi-node unit: arena selects allocation paths per NUMA node", "[multi_node][unit][arena]") {
    const auto nodes = numa_test::two_test_nodes();

    for (int node : nodes) {
        NumaArena arena(node);
        REQUIRE(arena.node_id() == node);

        void* small = arena.allocate(128, alignof(std::max_align_t));
        auto* small_header = BlockHeader::from_user_ptr(small);
        REQUIRE(small_header->node_id == static_cast<std::uint32_t>(node));
        REQUIRE(small_header->size_class == SizeClassTable::class_size(128));
        numa_test::touch_memory(small, 128);
        arena.deallocate(small);

        void* zero = arena.allocate(0, alignof(std::max_align_t));
        auto* zero_header = BlockHeader::from_user_ptr(zero);
        REQUIRE(zero_header->node_id == static_cast<std::uint32_t>(node));
        REQUIRE(zero_header->size_class == SizeClassTable::class_size(1));
        numa_test::touch_memory(zero, 1);
        arena.deallocate(zero);

        void* large = arena.allocate(SMALL_LARGE_THRESHOLD + 1, alignof(std::max_align_t));
        auto* large_header = BlockHeader::from_user_ptr(large);
        REQUIRE(large_header->node_id == static_cast<std::uint32_t>(node));
        REQUIRE(large_header->size_class == 0);
        numa_test::touch_memory(large, SMALL_LARGE_THRESHOLD + 1);
        arena.deallocate(large);

        void* over_aligned = arena.allocate(128, 64);
        auto* aligned_header = BlockHeader::from_user_ptr(over_aligned);
        REQUIRE(numa_test::is_aligned(over_aligned, 64));
        REQUIRE(aligned_header->node_id == static_cast<std::uint32_t>(node));
        REQUIRE(aligned_header->size_class == 0);
        arena.deallocate(over_aligned);
    }
}

// Verifies foreign small frees can be routed back to the owner arena through NumaManager.
TEST_CASE("multi-node unit: arena routes cross-node frees to owner arena", "[multi_node][unit][arena][foreign]") {
    const auto nodes = numa_test::two_test_nodes();
    auto& manager = NumaManager::instance();

    void* ptr = manager.arena_for_node(nodes[0]).allocate(256, alignof(std::max_align_t));
    REQUIRE(ptr != nullptr);
    REQUIRE(BlockHeader::from_user_ptr(ptr)->node_id == static_cast<std::uint32_t>(nodes[0]));
    numa_test::touch_memory(ptr, 256);

    manager.arena_for_node(nodes[1]).deallocate(ptr);

    void* next = manager.arena_for_node(nodes[0]).allocate(256, alignof(std::max_align_t));
    REQUIRE(next != nullptr);
    numa_test::touch_memory(next, 256, 0x3C);
    manager.arena_for_node(nodes[0]).deallocate(next);
}

// Verifies standalone no-sync arenas work for single-threaded allocation cycles on each node.
TEST_CASE("multi-node unit: arena supports standalone no-sync policy", "[multi_node][unit][arena][no_sync]") {
    const auto nodes = numa_test::two_test_nodes();

    for (int node : nodes) {
        NumaArena arena(node, false, false, false);

        for (std::size_t size : numa_test::mixed_sizes()) {
            void* ptr = arena.allocate(size, alignof(std::max_align_t));
            REQUIRE(ptr != nullptr);
            numa_test::touch_memory(ptr, size);
            arena.deallocate(ptr);
        }
    }
}

// Verifies synchronized arenas tolerate concurrent allocation/write/free cycles.
TEST_CASE("multi-node unit: arena sync mode supports concurrent cycles", "[multi_node][unit][arena][thread]") {
    const auto nodes = numa_test::two_test_nodes();

    for (int node : nodes) {
        NumaArena arena(node, false, true, false);
        std::atomic<bool> failed{false};
        std::vector<std::thread> threads;

        for (int thread_id = 0; thread_id < 4; ++thread_id) {
            threads.emplace_back([&arena, &failed, thread_id] {
                for (std::size_t i = 0; i < 256; ++i) {
                    const auto sizes = numa_test::mixed_sizes();
                    const std::size_t size = sizes[(i + static_cast<std::size_t>(thread_id)) % sizes.size()];
                    void* ptr = arena.allocate(size, alignof(std::max_align_t));
                    if (!ptr) {
                        failed.store(true);
                        return;
                    }

                    numa_test::touch_memory(ptr, size, static_cast<unsigned char>(thread_id + 1));
                    arena.deallocate(ptr);
                }
            });
        }

        for (auto& thread : threads) {
            thread.join();
        }

        REQUIRE_FALSE(failed.load());
    }
}
