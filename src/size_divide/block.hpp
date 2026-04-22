#pragma once

#include "virtual_memory/virtual_memory.hpp"
#include <cassert>
#include <mutex>

// ============================================================
// SMALL OBJECT CONFIG
// ============================================================

#define SMALL_LARGE_THRESHOLD 4096        // 4 KB
#define SLAB_SIZE             (64 * 1024) // 64 KB
static_assert((SLAB_SIZE & (SLAB_SIZE - 1)) == 0);

struct SizeClassConfig {
    static constexpr size_t kNumBounds = 3;
    static constexpr size_t thresholds[kNumBounds] = {256, 1024, SMALL_LARGE_THRESHOLD};
    static constexpr size_t alignments[kNumBounds] = {16, 64, 256};

    static constexpr size_t max_cached_bytes_for_class(size_t class_size) noexcept {
        for (size_t i = 0; i < kNumBounds; ++i) {
            if (class_size <= thresholds[i]) {
                return thresholds[i]/2 * 1024;
            }
        }

        return SMALL_LARGE_THRESHOLD/2 * 1024;
    }
};

// ============================================================
// LARGE OBJECT CONFIG
// ============================================================

struct LargeObjectConfig {
    static constexpr size_t kLargeBinSizes[] = {
        8 * 1024,
        12 * 1024,
        16 * 1024,
        24 * 1024,
        32 * 1024,
        48 * 1024,
        64 * 1024,
        96 * 1024,
        128 * 1024,
        192 * 1024,
        256 * 1024,
        384 * 1024,
        512 * 1024,
        768 * 1024,
        1024 * 1024,
        2 * 1024 * 1024,
    };
    static constexpr size_t kLargeBinSizesCount = sizeof(kLargeBinSizes);
    static constexpr size_t kNumLargeBins = sizeof(kLargeBinSizes) / sizeof(kLargeBinSizes[0]);
    static constexpr size_t kMaxSpansPerLargeBin = 8;
    static constexpr size_t kMaxLargeCachedSpans = kNumLargeBins * kMaxSpansPerLargeBin;
    static constexpr size_t kMaxLargeCacheBytes = 64 * 1024 * 1024;
    static constexpr size_t kLargeBinSearchLimit = 3;

    static constexpr size_t npos = static_cast<size_t>(-1);

    static constexpr size_t class_size_for(size_t required_size) noexcept {
        if (required_size <= 8 * 1024) return 8 * 1024;
        if (required_size <= 12 * 1024) return 12 * 1024;
        if (required_size <= 16 * 1024) return 16 * 1024;
        if (required_size <= 24 * 1024) return 24 * 1024;
        if (required_size <= 32 * 1024) return 32 * 1024;
        if (required_size <= 48 * 1024) return 48 * 1024;
        if (required_size <= 64 * 1024) return 64 * 1024;
        if (required_size <= 96 * 1024) return 96 * 1024;
        if (required_size <= 128 * 1024) return 128 * 1024;
        if (required_size <= 192 * 1024) return 192 * 1024;
        if (required_size <= 256 * 1024) return 256 * 1024;
        if (required_size <= 384 * 1024) return 384 * 1024;
        if (required_size <= 512 * 1024) return 512 * 1024;
        if (required_size <= 768 * 1024) return 768 * 1024;
        if (required_size <= 1024 * 1024) return 1024 * 1024;
        if (required_size <= 2 * 1024 * 1024) return 2 * 1024 * 1024;
        return required_size;
    }

    static constexpr size_t bin_index_for(size_t span_size) noexcept {
        switch (span_size) {
            case 8 * 1024: return 0;
            case 12 * 1024: return 1;
            case 16 * 1024: return 2;
            case 24 * 1024: return 3;
            case 32 * 1024: return 4;
            case 48 * 1024: return 5;
            case 64 * 1024: return 6;
            case 96 * 1024: return 7;
            case 128 * 1024: return 8;
            case 192 * 1024: return 9;
            case 256 * 1024: return 10;
            case 384 * 1024: return 11;
            case 512 * 1024: return 12;
            case 768 * 1024: return 13;
            case 1024 * 1024: return 14;
            case 2 * 1024 * 1024: return 15;
            default: return npos;
        }
    }
};

// maximum fundamental alignment (usually 16 bytes, 8 on 32-bit architectures)
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

class OptionalMutexLock {
public:
    OptionalMutexLock(std::mutex& mutex, bool should_lock)
        : mutex_(should_lock ? &mutex : nullptr) {
        if (mutex_) {
            mutex_->lock();
        }
    }

    ~OptionalMutexLock() noexcept {
        if (mutex_) {
            mutex_->unlock();
        }
    }

    OptionalMutexLock(const OptionalMutexLock&) = delete;
    OptionalMutexLock& operator=(const OptionalMutexLock&) = delete;

private:
    std::mutex* mutex_;
};


// ============================================================
// BLOCK HEADER
// ============================================================

struct alignas(ALIGNMENT) BlockHeader { // 32 bytes
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