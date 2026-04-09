#include "numa_arena/numa_arena.hpp"

#include "numa_manager/numa_manager.hpp"
#include "virtual_memory/virtual_memory.hpp"

#include <new>
#include <cstdint>

// ============================================================
// NumaArena
// ============================================================

void NumaArenaDeleter::operator()(NumaArena* arena) const noexcept {
    if (!arena) return;

    arena->~NumaArena();
    VirtualMemory::release(arena, sizeof(NumaArena));
}

void* NumaArena::allocate(size_t size, size_t alignment) {
    if (size == 0) {
        size = 1;
    }

    if (size <= SMALL_LARGE_THRESHOLD && alignment <= ALIGNMENT) {
        size_t class_index = SizeClassTable::class_index_for_size(size);
        size_t class_size = kClassSizes[class_index];
        auto* header = reinterpret_cast<BlockHeader*>(
            small_.allocate_by_class_index(class_index)
        );

        header->node_id = static_cast<std::uint32_t>(node_id_);
        header->size_class = static_cast<std::uint32_t>(class_size);
        header->size = 0;

        return header->to_user_ptr();
    }

    return large_.allocate(size, alignment);
}

void NumaArena::deallocate(void* ptr) {
    if (!ptr) return;

    auto* header = BlockHeader::from_user_ptr(ptr);

    if (static_cast<int>(header->node_id) != node_id_) {
        // TODO foreign freelist
        NumaManager::instance()
            .arena_for_node(static_cast<int>(header->node_id))
            .deallocate(ptr);
        return;
    }

    if (header->size_class != 0) {
        small_.deallocate(header);
    } else {
        large_.deallocate(ptr);
    }
}
