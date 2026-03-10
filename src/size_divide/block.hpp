#pragma once

#include "../virtual_memory.hpp"
#include <cassert>

// ============================================================
// CONFIG
// ============================================================

#define SMALL_LARGE_THRESHOLD 4096        // 4 KB
#define SLAB_SIZE             (64 * 1024) // 64 KB
static_assert((SLAB_SIZE & (SLAB_SIZE - 1)) == 0);

struct SizeClassConfig {
    static constexpr size_t kNumBounds = 3;
    static constexpr size_t thresholds[kNumBounds] = {512, 1024, SMALL_LARGE_THRESHOLD};
    static constexpr size_t alignments[kNumBounds] = {16, 64, 256};
};

static constexpr size_t ALIGNMENT = alignof(std::max_align_t);

// ============================================================
// POINTER UTILS
// ============================================================

namespace pointer_utils {

inline void* add_bytes(void* p, size_t offset) noexcept {
    return static_cast<void*>(static_cast<char*>(p) + offset);
}

inline void* sub_bytes(void* p, size_t offset) noexcept {
    return static_cast<void*>(static_cast<char*>(p) - offset);
}

} // namespace pointer_utils


// ============================================================
// BLOCK HEADER
// ============================================================

struct alignas(ALIGNMENT) BlockHeader {
    uint32_t node_id;
    uint32_t size_class;   // 0 = large
    uint64_t size;         // only for large allocations

    void* raw_ptr;       // large: mmap base, small: owning slab
    size_t total_size;   // large: mmap size, small: slab size

    static BlockHeader* from_user_ptr(void* p) noexcept {
        return reinterpret_cast<BlockHeader*>(
            pointer_utils::sub_bytes(p, sizeof(BlockHeader))
        );
    }

    void* to_user_ptr() noexcept {
        return pointer_utils::add_bytes(this, sizeof(BlockHeader));
    }
};