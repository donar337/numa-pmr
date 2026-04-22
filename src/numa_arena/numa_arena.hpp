#pragma once

#include "size_divide/small_object/small_object_allocator.hpp"
#include "size_divide/large_object/large_object_allocator.hpp"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <cstddef>

/**
 * Per-NUMA-node allocation faсade, holds SmallObjectAllocator and LargeObjectAllocator for a NUMA node.
 */
class NumaArena {
public:
    explicit NumaArena(
        int node_id,
        bool foreign_freelist_enabled = true,
        bool sync = true,
        bool route_foreign_deallocations = true
    )
        : node_id_(node_id),
          foreign_freelist_enabled_(foreign_freelist_enabled),
          route_foreign_deallocations_(route_foreign_deallocations),
          small_(node_id, sync),
          large_(node_id,
                 LargeObjectConfig::kMaxLargeCachedSpans,
                 LargeObjectConfig::kMaxLargeCacheBytes,
                 sync)
    {}
    ~NumaArena() noexcept;

    void* allocate(size_t size, size_t alignment);
    void deallocate(void* ptr);

    int node_id() const noexcept {
        return node_id_;
    }

private:
    struct ForeignBatch {
        BlockHeader* head = nullptr;
        size_t count = 0;
        size_t bytes = 0;
    };

    struct ForeignBin {
        BlockHeader* head = nullptr;
        size_t count = 0;
        size_t bytes = 0;
        std::mutex mutex;
    };

    static void drain_foreign_bin_callback(void* context, size_t class_index);
    static size_t foreign_high_watermark(size_t class_size) noexcept;
    static size_t foreign_low_watermark(size_t class_size) noexcept;
    static BlockHeader*& next_foreign(BlockHeader* header) noexcept;

    void deallocate_foreign(void* ptr, BlockHeader* header);
    void enqueue_foreign_small(BlockHeader* header);
    void drain_foreign_bin(size_t class_index);
    void drain_all_foreign() noexcept;
    void drain_foreign_batch(size_t class_index, ForeignBatch batch);

    ForeignBatch detach_all_foreign_locked(ForeignBin& bin);
    ForeignBatch detach_foreign_until_low_locked(
        ForeignBin& bin,
        size_t class_size,
        size_t low_watermark
    );

    int node_id_;
    bool foreign_freelist_enabled_;
    bool route_foreign_deallocations_;
    SmallObjectAllocator small_;
    LargeObjectAllocator large_;
    std::array<ForeignBin, kNumSizeClasses> foreign_bins_;
    std::atomic<size_t> foreign_bytes_{0};
};

struct NumaArenaDeleter {
    void operator()(NumaArena* arena) const noexcept;
};

using NumaArenaPtr = std::unique_ptr<NumaArena, NumaArenaDeleter>;