#include <catch2/catch_test_macros.hpp>

#include "common/test_utils.hpp"
#include "size_divide/small_object/small_object_allocator.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace {

void prepare_small_header(BlockHeader* header, int node, std::size_t size) {
    header->node_id = static_cast<std::uint32_t>(node);
    header->size_class = static_cast<std::uint32_t>(SizeClassTable::class_size(size == 0 ? 1 : size));
    header->size = 0;
}

} // namespace

// Verifies that small allocators on different nodes return writable slab blocks.
TEST_CASE("multi-node unit: small allocator creates writable node-local blocks", "[multi_node][unit][small]") {
    const auto nodes = numa_test::two_test_nodes();

    for (int node : nodes) {
        SmallObjectAllocator allocator(node);

        for (std::size_t size : numa_test::small_sizes()) {
            void* block = allocator.allocate(size);
            REQUIRE(block != nullptr);

            auto* header = static_cast<BlockHeader*>(block);
            prepare_small_header(header, node, size);
            REQUIRE(header->raw_ptr != nullptr);
            REQUIRE(header->total_size == SLAB_SIZE);

            numa_test::touch_memory(header->to_user_ptr(), size);
            allocator.deallocate(block);
        }
    }
}

// Verifies that freed small blocks can be reused within each node-local allocator.
TEST_CASE("multi-node unit: small allocator reuses freed blocks per NUMA node", "[multi_node][unit][small][reuse]") {
    const auto nodes = numa_test::two_test_nodes();
    constexpr std::size_t size = 128;

    for (int node : nodes) {
        SmallObjectAllocator allocator(node);

        void* first = allocator.allocate(size);
        void* second = allocator.allocate(size);
        prepare_small_header(static_cast<BlockHeader*>(first), node, size);
        prepare_small_header(static_cast<BlockHeader*>(second), node, size);

        allocator.deallocate(first);

        void* reused = allocator.allocate(size);
        REQUIRE(reused == first);
        prepare_small_header(static_cast<BlockHeader*>(reused), node, size);

        allocator.deallocate(second);
        allocator.deallocate(reused);
    }
}

// Verifies that batch deallocation returns a linked list of blocks to the right size class.
TEST_CASE("multi-node unit: small allocator deallocates batches by class index", "[multi_node][unit][small][batch]") {
    const auto nodes = numa_test::two_test_nodes();
    constexpr std::size_t size = 64;
    const std::size_t class_index = SizeClassTable::class_index_for_size(size);

    for (int node : nodes) {
        SmallObjectAllocator allocator(node);
        std::vector<BlockHeader*> blocks;

        for (int i = 0; i < 3; ++i) {
            auto* header = static_cast<BlockHeader*>(allocator.allocate(size));
            prepare_small_header(header, node, size);
            blocks.push_back(header);
        }

        for (std::size_t i = 0; i + 1 < blocks.size(); ++i) {
            *reinterpret_cast<BlockHeader**>(blocks[i]->to_user_ptr()) = blocks[i + 1];
        }
        *reinterpret_cast<BlockHeader**>(blocks.back()->to_user_ptr()) = nullptr;

        allocator.deallocate_batch(class_index, blocks.front());

        void* reused = allocator.allocate(size);
        REQUIRE(std::find(blocks.begin(), blocks.end(), static_cast<BlockHeader*>(reused)) != blocks.end());
        prepare_small_header(static_cast<BlockHeader*>(reused), node, size);
        allocator.deallocate(reused);
    }
}

// Verifies the single-threaded no-sync mode used by standalone arena resources.
TEST_CASE("multi-node unit: small allocator supports no-sync scoped use", "[multi_node][unit][small][no_sync]") {
    const auto nodes = numa_test::two_test_nodes();

    for (int node : nodes) {
        SmallObjectAllocator allocator(node, false);

        for (std::size_t size : numa_test::small_sizes()) {
            void* block = allocator.allocate(size);
            auto* header = static_cast<BlockHeader*>(block);
            prepare_small_header(header, node, size);
            numa_test::touch_memory(header->to_user_ptr(), size);
            allocator.deallocate(block);
        }
    }
}
