#include <array>
#include <cstring>

#include <catch2/catch_test_macros.hpp>

#include "common/test_utils.hpp"
#include "size_divide/block.hpp"

// Verifies that BlockHeader can round-trip between header and user pointers.
TEST_CASE("multi-node unit: block header converts between header and user pointer", "[multi_node][unit][block]") {
    numa_test::require_real_numa_system();

    alignas(ALIGNMENT) std::array<unsigned char, 256> storage{};
    auto* header = reinterpret_cast<BlockHeader*>(storage.data());

    void* user = header->to_user_ptr();
    REQUIRE(BlockHeader::from_user_ptr(user) == header);
    REQUIRE(pointer_utils::add_bytes(header, sizeof(BlockHeader)) == user);
    REQUIRE(pointer_utils::sub_bytes(user, sizeof(BlockHeader)) == header);
}

// Verifies that allocator metadata remains adjacent to writable user payload.
TEST_CASE("multi-node unit: block metadata survives payload writes", "[multi_node][unit][block]") {
    numa_test::require_real_numa_system();

    alignas(ALIGNMENT) std::array<unsigned char, 256> storage{};
    auto* header = reinterpret_cast<BlockHeader*>(storage.data());
    header->node_id = 1;
    header->size_class = 64;
    header->size = 0;
    header->raw_ptr = storage.data();
    header->total_size = storage.size();

    std::memset(header->to_user_ptr(), 0x5A, 64);

    REQUIRE(header->node_id == 1);
    REQUIRE(header->size_class == 64);
    REQUIRE(header->size == 0);
    REQUIRE(header->raw_ptr == storage.data());
    REQUIRE(header->total_size == storage.size());
}

// Verifies that the header layout satisfies the alignment used by allocator blocks.
TEST_CASE("multi-node unit: block header layout preserves allocator alignment", "[multi_node][unit][block][alignment]") {
    numa_test::require_real_numa_system();

    REQUIRE(alignof(BlockHeader) == ALIGNMENT);
    REQUIRE(sizeof(BlockHeader) % ALIGNMENT == 0);
}
