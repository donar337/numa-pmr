#include <catch2/catch_test_macros.hpp>

#include "common/test_utils.hpp"
#include "size_divide/large_object/large_object_allocator.hpp"

#include <array>
#include <cstddef>

namespace {

std::size_t expected_large_span_size(std::size_t user_size, std::size_t alignment) {
    const std::size_t required = user_size + alignment + sizeof(BlockHeader);
    const std::size_t page_size = VirtualMemory::align_up(required, VirtualMemory::page_size());
    const std::size_t class_size = LargeObjectConfig::class_size_for(page_size);
    return VirtualMemory::align_up(class_size, VirtualMemory::page_size());
}

} // namespace

// Verifies large allocation metadata, alignment, and writable payloads on each node.
TEST_CASE("multi-node unit: large allocator honors node metadata and alignment", "[multi_node][unit][large]") {
    const auto nodes = numa_test::two_test_nodes();

    for (int node : nodes) {
        LargeObjectAllocator allocator(node);

        for (std::size_t size : numa_test::large_sizes()) {
            constexpr std::size_t alignment = 64;
            void* ptr = allocator.allocate(size, alignment);
            REQUIRE(ptr != nullptr);
            REQUIRE(numa_test::is_aligned(ptr, alignment));

            auto* header = BlockHeader::from_user_ptr(ptr);
            REQUIRE(header->node_id == static_cast<std::uint32_t>(node));
            REQUIRE(header->size_class == 0);
            REQUIRE(header->size == size);
            REQUIRE(header->raw_ptr != nullptr);
            REQUIRE(header->total_size >= size + alignment + sizeof(BlockHeader));

            numa_test::touch_memory(ptr, size);
            allocator.deallocate(ptr);
        }
    }
}

// Verifies that compatible large sizes reuse cached spans within the same node.
TEST_CASE("multi-node unit: large allocator reuses class-size spans", "[multi_node][unit][large][reuse]") {
    const auto nodes = numa_test::two_test_nodes();
    constexpr std::size_t alignment = alignof(std::max_align_t);
    constexpr std::size_t user_size = 8192;
    constexpr std::size_t nearby_user_size = 9000;
    const std::size_t expected_span = expected_large_span_size(user_size, alignment);
    REQUIRE(expected_span == expected_large_span_size(nearby_user_size, alignment));

    for (int node : nodes) {
        LargeObjectAllocator allocator(node, 4, 1024 * 1024);

        void* first = allocator.allocate(user_size, alignment);
        auto* first_header = BlockHeader::from_user_ptr(first);
        void* first_raw = first_header->raw_ptr;
        REQUIRE(first_header->total_size == expected_span);

        allocator.deallocate(first);

        void* second = allocator.allocate(nearby_user_size, alignment);
        auto* second_header = BlockHeader::from_user_ptr(second);
        REQUIRE(second_header->raw_ptr == first_raw);
        REQUIRE(second_header->total_size == expected_span);
        REQUIRE(second_header->size == nearby_user_size);

        allocator.deallocate(second);
    }
}

// Verifies that an empty exact bin can reuse a bounded larger cached span.
TEST_CASE("multi-node unit: large allocator reuses nearest larger cached span", "[multi_node][unit][large][reuse]") {
    const auto nodes = numa_test::two_test_nodes();
    constexpr std::size_t alignment = alignof(std::max_align_t);
    constexpr std::size_t small_user_size = 8192;
    constexpr std::size_t larger_user_size = 16384;
    const std::size_t larger_expected_span = expected_large_span_size(larger_user_size, alignment);

    for (int node : nodes) {
        LargeObjectAllocator allocator(node, 4, 1024 * 1024);

        void* larger = allocator.allocate(larger_user_size, alignment);
        auto* larger_header = BlockHeader::from_user_ptr(larger);
        void* larger_raw = larger_header->raw_ptr;
        REQUIRE(larger_header->total_size == larger_expected_span);

        allocator.deallocate(larger);

        void* smaller = allocator.allocate(small_user_size, alignment);
        auto* smaller_header = BlockHeader::from_user_ptr(smaller);
        REQUIRE(smaller_header->raw_ptr == larger_raw);
        REQUIRE(smaller_header->total_size == larger_expected_span);
        REQUIRE(smaller_header->size == small_user_size);

        allocator.deallocate(smaller);
    }
}

// Verifies single-threaded no-sync span reuse on multiple NUMA nodes.
TEST_CASE("multi-node unit: large allocator supports no-sync reuse", "[multi_node][unit][large][no_sync]") {
    const auto nodes = numa_test::two_test_nodes();
    constexpr std::size_t alignment = alignof(std::max_align_t);
    constexpr std::size_t user_size = 8192;

    for (int node : nodes) {
        LargeObjectAllocator allocator(node, 4, 1024 * 1024, false);

        void* first = allocator.allocate(user_size, alignment);
        auto* first_header = BlockHeader::from_user_ptr(first);
        void* first_raw = first_header->raw_ptr;

        allocator.deallocate(first);

        void* second = allocator.allocate(user_size, alignment);
        auto* second_header = BlockHeader::from_user_ptr(second);
        REQUIRE(second_header->raw_ptr == first_raw);

        allocator.deallocate(second);
    }
}
