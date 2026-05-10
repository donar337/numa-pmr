// Ручная «песочница» для экспериментов с NUMA-аллокатором (не автотесты).
// Линковка: target numa_allocator (см. CMakeLists.txt).
// cmake -S . -B ./build -DNUMA_ALLOCATOR_BUILD_PLAYGROUND=ON
// cmake --build ./build --target numa_allocator_playground
// ./build/numa_allocator_playground

#include "numa_arena/numa_arena.hpp"
#include "numa_memory_resource.hpp"
#include "numa_topology/numa_topology.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory_resource>
#include <vector>

int main() {
    auto& topology = NumaTopologyManager::instance();

    std::cout << "node_count=" << topology.node_count()
              << " current_node=" << topology.current_node_from_cpu() << '\n';

    // Прямая работа с ареной (без singleton).
    {
        NumaArena arena(0);
        constexpr std::size_t kSmall = 256;
        void* small = arena.allocate(kSmall, alignof(std::max_align_t));
        std::memset(small, 0xCC, kSmall);
        arena.deallocate(small);

        constexpr std::size_t kLarge = SMALL_LARGE_THRESHOLD + 64 * 1024;
        constexpr std::size_t kAlign = 64;
        void* large = arena.allocate(kLarge, kAlign);
        std::memset(large, 0x33, kLarge);
        arena.deallocate(large);
    }

    // std::pmr поверх numa_memory_resource.
    {
        std::pmr::vector<std::uint8_t> bytes(get_numa_memory_resource());
        bytes.resize(8192);
        bytes[0] = 1;
        bytes.back() = 2;
    }

    std::cout << "allocator_playground ok\n";
    std::cout << sizeof(BlockHeader) << '\n';
    return 0;
}
