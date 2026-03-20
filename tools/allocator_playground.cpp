// Ручная «песочница» для экспериментов с NUMA-аллокатором (не автотесты).
// Линковка: target numa_allocator (см. CMakeLists.txt).
// cmake -S . -B ./build -DNUMA_ALLOCATOR_BUILD_PLAYGROUND=ON
// cmake --build ./build --target numa_allocator_playground
// ./build/numa_allocator_playground

#include "numa_arena.hpp"
#include "numa_aware_memory_resource.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory_resource>
#include <vector>

int main() {
    auto& mgr = NumaManager::instance();

    std::cout << "node_count=" << mgr.node_count()
              << " current_node=" << mgr.current_node() << '\n';

    // Прямая работа с ареной (без singleton).
    {
        NumaArena arena(0);
        constexpr std::size_t kSmall = 256;
        void* small = arena.allocate(kSmall, alignof(std::max_align_t));
        std::memset(small, 0xCC, kSmall);
        arena.deallocate(small, kSmall, alignof(std::max_align_t));

        constexpr std::size_t kLarge = SMALL_LARGE_THRESHOLD + 64 * 1024;
        constexpr std::size_t kAlign = 64;
        void* large = arena.allocate(kLarge, kAlign);
        std::memset(large, 0x33, kLarge);
        arena.deallocate(large, kLarge, kAlign);
    }

    // Singleton-менеджер: текущая арена потока.
    {
        void* p =
            mgr.arena_for_current_thread().allocate(512, alignof(std::max_align_t));
        std::memset(p, 0xEE, 512);
        mgr.arena_for_current_thread().deallocate(p, 512, alignof(std::max_align_t));
    }

    // std::pmr поверх NumaMemoryResource.
    {
        static NumaMemoryResource resource;
        std::pmr::vector<std::uint8_t> bytes(&resource);
        bytes.resize(8192);
        bytes[0] = 1;
        bytes.back() = 2;
    }

    std::cout << "allocator_playground ok\n";
    std::cout << sizeof(BlockHeader) << '\n';
    return 0;
}
