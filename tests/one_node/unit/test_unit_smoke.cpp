#include <catch2/catch_test_macros.hpp>

#include "common/test_utils.hpp"
#include "numa_arena.hpp"
#include "size_divide/large_object_allocator.hpp"
#include "size_divide/small_object_allocator.hpp"

#include <array>
#include <cstddef>
#include <vector>

// Проверяет граничные значения и монотонность таблицы small size classes.
TEST_CASE("one-node unit: size classes are rounded consistently", "[one_node][unit][size_class]") {
    REQUIRE(SizeClassTable::class_size(1) == 16);
    REQUIRE(SizeClassTable::class_size(16) == 16);
    REQUIRE(SizeClassTable::class_size(17) == 32);
    REQUIRE(SizeClassTable::class_size(512) == 512);
    REQUIRE(SizeClassTable::class_size(513) == 576);
    REQUIRE(SizeClassTable::class_size(1024) == 1024);
    REQUIRE(SizeClassTable::class_size(1025) == 1280);
    REQUIRE(SizeClassTable::class_size(4096) == 4096);

    std::size_t previous = 0;
    for (std::size_t size = 1; size <= SMALL_LARGE_THRESHOLD; ++size) {
        const std::size_t current = SizeClassTable::class_size(size);
        REQUIRE(current >= size);
        REQUIRE(current >= previous);
        previous = current;
    }
}

// Проверяет базовые операции virtual memory: выравнивание, reserve(0), запись и release.
TEST_CASE("one-node unit: virtual memory can reserve writable pages", "[one_node][unit][virtual_memory]") {
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

// Проверяет, что small allocator выдаёт writable blocks и заполняет slab metadata в header.
TEST_CASE("one-node unit: small allocator initializes block headers", "[one_node][unit][small]") {
    SmallObjectAllocator allocator(0);

    for (std::size_t size : std::array<std::size_t, 7>{1, 16, 17, 64, 513, 2048, 4096}) {
        void* block = allocator.allocate(size);
        REQUIRE(block != nullptr);

        auto* header = static_cast<BlockHeader*>(block);
        header->node_id = 0;
        header->size_class = static_cast<std::uint32_t>(SizeClassTable::class_size(size));
        header->size = 0;

        REQUIRE(header->raw_ptr != nullptr);
        REQUIRE(header->total_size == SLAB_SIZE);
        numa_test::touch_memory(header->to_user_ptr(), size);

        allocator.deallocate(block);
    }
}

// Проверяет, что освобождённый small block может быть переиспользован без поломки соседних блоков.
TEST_CASE("one-node unit: small allocator reuses freed blocks", "[one_node][unit][small][reuse]") {
    SmallObjectAllocator allocator(0);
    constexpr std::size_t size = 64;
    const auto size_class = static_cast<std::uint32_t>(SizeClassTable::class_size(size));

    void* first = allocator.allocate(size);
    void* second = allocator.allocate(size);

    auto* first_header = static_cast<BlockHeader*>(first);
    auto* second_header = static_cast<BlockHeader*>(second);
    first_header->node_id = 0;
    first_header->size_class = size_class;
    first_header->size = 0;
    second_header->node_id = 0;
    second_header->size_class = size_class;
    second_header->size = 0;

    numa_test::touch_memory(first_header->to_user_ptr(), size, 0x11);
    numa_test::touch_memory(second_header->to_user_ptr(), size, 0x22);

    allocator.deallocate(first);

    void* reused = allocator.allocate(size);
    REQUIRE(reused == first);

    auto* reused_header = static_cast<BlockHeader*>(reused);
    reused_header->node_id = 0;
    reused_header->size_class = size_class;
    reused_header->size = 0;
    REQUIRE(reused_header->raw_ptr != nullptr);
    REQUIRE(reused_header->total_size == SLAB_SIZE);

    allocator.deallocate(second);
    allocator.deallocate(reused);
}

// Проверяет, что allocator удерживает пустой slab и переиспользует его после полного освобождения.
TEST_CASE("one-node unit: small allocator reuses retained empty slab", "[one_node][unit][small][slab]") {
    SmallObjectAllocator allocator(0);
    constexpr std::size_t size = 128;

    void* first = allocator.allocate(size);
    auto* first_header = static_cast<BlockHeader*>(first);
    first_header->node_id = 0;
    first_header->size_class = static_cast<std::uint32_t>(SizeClassTable::class_size(size));
    first_header->size = 0;
    allocator.deallocate(first);

    void* second = allocator.allocate(size);
    auto* second_header = static_cast<BlockHeader*>(second);
    second_header->node_id = 0;
    second_header->size_class = static_cast<std::uint32_t>(SizeClassTable::class_size(size));
    second_header->size = 0;
    REQUIRE(second_header->raw_ptr != nullptr);
    REQUIRE(second_header->total_size == SLAB_SIZE);
    allocator.deallocate(second);
}

// Проверяет large allocator: alignment, header invariants и корректное освобождение.
TEST_CASE("one-node unit: large allocator honors alignment and metadata", "[one_node][unit][large]") {
    LargeObjectAllocator allocator(0);

    for (std::size_t size : std::array<std::size_t, 3>{SMALL_LARGE_THRESHOLD + 1, 65536, 1024 * 1024}) {
        constexpr std::size_t alignment = 64;
        void* ptr = allocator.allocate(size, alignment);
        REQUIRE(ptr != nullptr);
        REQUIRE(numa_test::is_aligned(ptr, alignment));

        auto* header = BlockHeader::from_user_ptr(ptr);
        REQUIRE(header->node_id == 0);
        REQUIRE(header->size_class == 0);
        REQUIRE(header->size == size);
        REQUIRE(header->raw_ptr != nullptr);
        REQUIRE(header->total_size >= size + alignment + sizeof(BlockHeader));

        numa_test::touch_memory(ptr, size);
        allocator.deallocate(ptr, size);
    }
}

// Проверяет, что arena корректно маршрутизирует small, large, zero-size и over-aligned запросы.
TEST_CASE("one-node unit: arena selects correct allocation paths", "[one_node][unit][arena]") {
    NumaArena arena(0);

    void* small = arena.allocate(128, alignof(std::max_align_t));
    auto* small_header = BlockHeader::from_user_ptr(small);
    REQUIRE(small_header->node_id == 0);
    REQUIRE(small_header->size_class == SizeClassTable::class_size(128));
    numa_test::touch_memory(small, 128);
    arena.deallocate(small, 128, alignof(std::max_align_t));

    void* zero = arena.allocate(0, alignof(std::max_align_t));
    auto* zero_header = BlockHeader::from_user_ptr(zero);
    REQUIRE(zero_header->node_id == 0);
    REQUIRE(zero_header->size_class == SizeClassTable::class_size(1));
    numa_test::touch_memory(zero, 1);
    arena.deallocate(zero, 0, alignof(std::max_align_t));

    void* large = arena.allocate(SMALL_LARGE_THRESHOLD + 1, alignof(std::max_align_t));
    auto* large_header = BlockHeader::from_user_ptr(large);
    REQUIRE(large_header->node_id == 0);
    REQUIRE(large_header->size_class == 0);
    numa_test::touch_memory(large, SMALL_LARGE_THRESHOLD + 1);
    arena.deallocate(large, SMALL_LARGE_THRESHOLD + 1, alignof(std::max_align_t));

    void* over_aligned = arena.allocate(128, 64);
    auto* aligned_header = BlockHeader::from_user_ptr(over_aligned);
    REQUIRE(numa_test::is_aligned(over_aligned, 64));
    REQUIRE(aligned_header->size_class == 0);
    arena.deallocate(over_aligned, 128, 64);
}
