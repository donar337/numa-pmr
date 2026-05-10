#include "numa_arena/numa_arena.hpp"

#include "arena_manager/arena_manager.hpp"
#include "virtual_memory/virtual_memory.hpp"

#include <new>
#include <cstdint>

// ============================================================
// NumaArena
// ============================================================

NumaArena::~NumaArena() noexcept {
    drain_all_foreign();
}

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
        void* block = (!foreign_freelist_enabled_ ||
                       foreign_bytes_.load(std::memory_order_relaxed) == 0)
            ? small_.allocate_by_class_index(class_index)
            : small_.allocate_by_class_index(
                class_index,
                &NumaArena::drain_foreign_bin_callback,
                this
            );
        auto* header = reinterpret_cast<BlockHeader*>(
            block
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

    if (static_cast<int>(header->node_id) != node_id_ && route_foreign_deallocations_) {
        ArenaManager::instance()
            .arena_for_node(static_cast<int>(header->node_id))
            .deallocate_foreign(ptr, header);
        return;
    }

    if (header->size_class != 0) {
        small_.deallocate(header);
    } else {
        large_.deallocate(ptr);
    }
}

void NumaArena::drain_foreign_bin_callback(void* context, size_t class_index) {
    static_cast<NumaArena*>(context)->drain_foreign_bin(class_index);
}

size_t NumaArena::foreign_high_watermark(size_t class_size) noexcept {
    return SizeClassConfig::max_cached_bytes_for_class(class_size);
}

size_t NumaArena::foreign_low_watermark(size_t class_size) noexcept {
    return foreign_high_watermark(class_size) / 2;
}

BlockHeader*& NumaArena::next_foreign(BlockHeader* header) noexcept {
    return *reinterpret_cast<BlockHeader**>(header->to_user_ptr());
}

void NumaArena::deallocate_foreign(void* ptr, BlockHeader* header) {
    if (foreign_freelist_enabled_ && header->size_class != 0) {
        enqueue_foreign_small(header);
    } else {
        if (header->size_class != 0) {
            small_.deallocate(header);
        } else {
            large_.deallocate(ptr);
        }
    }
}

void NumaArena::enqueue_foreign_small(BlockHeader* header) {
    const size_t class_size = header->size_class;
    const size_t class_index = SizeClassTable::class_index_for_size(class_size);
    const size_t high = foreign_high_watermark(class_size);
    const size_t low = foreign_low_watermark(class_size);

    ForeignBatch overflow_batch;
    auto& bin = foreign_bins_[class_index];

    {
        std::lock_guard<std::mutex> lock(bin.mutex);

        next_foreign(header) = bin.head;
        bin.head = header;
        ++bin.count;
        bin.bytes += class_size;
        foreign_bytes_.fetch_add(class_size, std::memory_order_relaxed);

        if (bin.bytes > high) {
            overflow_batch = detach_foreign_until_low_locked(bin, class_size, low);
        }
    }

    if (overflow_batch.bytes != 0) {
        foreign_bytes_.fetch_sub(overflow_batch.bytes, std::memory_order_relaxed);
    }
    drain_foreign_batch(class_index, overflow_batch);
}

void NumaArena::drain_foreign_bin(size_t class_index) {
    if (class_index >= kNumSizeClasses) {
        throw std::out_of_range("invalid small allocation size class");
    }

    ForeignBatch batch;
    auto& bin = foreign_bins_[class_index];

    {
        std::lock_guard<std::mutex> lock(bin.mutex);
        batch = detach_all_foreign_locked(bin);
    }

    if (batch.bytes != 0) {
        foreign_bytes_.fetch_sub(batch.bytes, std::memory_order_relaxed);
    }
    drain_foreign_batch(class_index, batch);
}

void NumaArena::drain_all_foreign() noexcept {
    for (size_t class_index = 0; class_index < foreign_bins_.size(); ++class_index) {
        ForeignBatch batch;
        auto& bin = foreign_bins_[class_index];

        {
            std::lock_guard<std::mutex> lock(bin.mutex);
            batch = detach_all_foreign_locked(bin);
        }

        if (batch.bytes != 0) {
            foreign_bytes_.fetch_sub(batch.bytes, std::memory_order_relaxed);
        }

        try {
            drain_foreign_batch(class_index, batch);
        } catch (...) {
        }
    }
}

void NumaArena::drain_foreign_batch(size_t class_index, ForeignBatch batch) {
    if (!batch.head) {
        return;
    }

    small_.deallocate_batch(class_index, batch.head);
}

NumaArena::ForeignBatch NumaArena::detach_all_foreign_locked(ForeignBin& bin) {
    ForeignBatch batch{bin.head, bin.count, bin.bytes};
    bin.head = nullptr;
    bin.count = 0;
    bin.bytes = 0;
    return batch;
}

NumaArena::ForeignBatch NumaArena::detach_foreign_until_low_locked(
    ForeignBin& bin,
    size_t class_size,
    size_t low_watermark
) {
    ForeignBatch batch;

    while (bin.head && bin.bytes > low_watermark) {
        BlockHeader* header = bin.head;
        bin.head = next_foreign(header);

        next_foreign(header) = batch.head;
        batch.head = header;
        ++batch.count;
        batch.bytes += class_size;

        --bin.count;
        bin.bytes -= class_size;
    }

    return batch;
}
