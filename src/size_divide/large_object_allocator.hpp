#pragma once

#include "block.hpp"
#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <vector>

// ============================================================
// LARGE OBJECT ALLOCATOR
// ============================================================

enum class LargeCachePolicy {
    Exact,
    Bucketed
};

struct LargeCacheConfig {
    LargeCachePolicy policy = LargeCachePolicy::Exact;
    bool enable_stats = false;

    // TODO: Bucketed policy is experimental. Tune buckets with real workloads
    // before enabling it by default.
    size_t small_limit = 64 * 1024;
    size_t medium_limit = 1024 * 1024;

    size_t small_quantum = 4 * 1024;
    size_t medium_quantum = 64 * 1024;
    size_t large_quantum = 1024 * 1024;

    size_t max_cached_spans = 64;
    size_t max_cached_bytes = 64 * 1024 * 1024;
};

struct LargeCacheStats {
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t cached_spans;
    uint64_t cached_bytes;
    uint64_t requested_bytes;
    uint64_t bucket_bytes;
};

class LargeObjectAllocator {
public:
    explicit LargeObjectAllocator(int node_id)
        : node_id_(node_id),
          use_configured_policy_(false),
          max_cached_spans_(LargeCacheConfig{}.max_cached_spans),
          max_cached_bytes_(LargeCacheConfig{}.max_cached_bytes) {
        cache_.reserve(max_cached_spans_);
    }

    LargeObjectAllocator(int node_id, LargeCacheConfig config)
        : node_id_(node_id),
          config_(config),
          use_configured_policy_(true),
          max_cached_spans_(config_.max_cached_spans),
          max_cached_bytes_(config_.max_cached_bytes) {
        cache_.reserve(max_cached_spans_);
    }

    ~LargeObjectAllocator();

    void* allocate(size_t size, size_t alignment) {
        if (alignment < alignof(void*)) {
            alignment = alignof(void*);
        }

        if (alignment > std::numeric_limits<size_t>::max() - sizeof(BlockHeader) ||
            size > std::numeric_limits<size_t>::max() - alignment - sizeof(BlockHeader)) {
            throw std::bad_alloc();
        }

        size_t total = size + alignment + sizeof(BlockHeader);
        if (!use_configured_policy_) {
            const size_t total_size = exact_size_for(total);
            return allocate_from_span(size, alignment, total_size, acquire_span_exact(total_size));
        }

        const bool use_stats = config_.enable_stats;
        const size_t total_size = config_.policy == LargeCachePolicy::Exact
            ? exact_size_for(total)
            : bucketed_size_for(total);

        if (use_stats) {
            requested_bytes_.fetch_add(total, std::memory_order_relaxed);
            bucket_bytes_.fetch_add(total_size, std::memory_order_relaxed);
        }

        void* raw = use_stats
            ? acquire_span_with_stats(total_size)
            : acquire_span_exact(total_size);

        return allocate_from_span(size, alignment, total_size, raw);
    }

    void* allocate_from_span(size_t size, size_t alignment, size_t total_size, void* raw) {
        uintptr_t raw_addr = reinterpret_cast<uintptr_t>(raw);
        uintptr_t start = raw_addr + sizeof(BlockHeader);
        uintptr_t aligned = VirtualMemory::align_up(start, alignment);
        void* user_ptr = reinterpret_cast<void*>(aligned);

        auto* header = reinterpret_cast<BlockHeader*>(
            aligned - sizeof(BlockHeader)
        );

        header->node_id = static_cast<uint32_t>(node_id_);
        header->size_class = 0; // large
        header->size = size;

        header->raw_ptr = raw;
        header->total_size = total_size;

        return user_ptr;
    }

    void deallocate(void* ptr, size_t) {
        if (!ptr) return;

        auto* header = BlockHeader::from_user_ptr(ptr);

        if (!release_to_cache(header->raw_ptr, header->total_size)) {
            VirtualMemory::release(
                header->raw_ptr,
                header->total_size
            );
        }
    }

private:
    struct CachedSpan {
        void* raw_ptr;
        size_t bucket_size;
    };

public:
    LargeCacheStats cache_stats() const noexcept;

private:
    static size_t checked_align_up(size_t value, size_t alignment) {
        if (alignment == 0) {
            throw std::invalid_argument("large cache quantum must be non-zero");
        }

        if (value > std::numeric_limits<size_t>::max() - (alignment - 1)) {
            throw std::bad_alloc();
        }

        return VirtualMemory::align_up(value, alignment);
    }

    static size_t exact_size_for(size_t required_size) {
        return VirtualMemory::align_up(required_size, VirtualMemory::page_size());
    }

    size_t bucketed_size_for(size_t required_size) const {
        const size_t page_size = VirtualMemory::page_size();

        if (required_size <= config_.small_limit) {
            return checked_align_up(required_size, std::max(config_.small_quantum, page_size));
        }

        if (required_size <= config_.medium_limit) {
            return checked_align_up(required_size, std::max(config_.medium_quantum, page_size));
        }

        return checked_align_up(required_size, std::max(config_.large_quantum, page_size));
    }

    void* acquire_span_exact(size_t bucket_size) {
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);

            for (auto it = cache_.begin(); it != cache_.end(); ++it) {
                if (it->bucket_size == bucket_size) {
                    void* raw = it->raw_ptr;
                    cached_bytes_ -= it->bucket_size;
                    std::swap(*it, cache_.back());
                    cache_.pop_back();
                    return raw;
                }
            }
        }

        void* raw = VirtualMemory::reserve(bucket_size);

        VirtualMemory::bind_to_node(
            raw,
            bucket_size,
            node_id_,
            VirtualMemory::NumaPolicy::Bind
        );

        return raw;
    }

    void* acquire_span_with_stats(size_t bucket_size) {
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);

            for (auto it = cache_.begin(); it != cache_.end(); ++it) {
                if (it->bucket_size == bucket_size) {
                    void* raw = it->raw_ptr;
                    cached_bytes_ -= it->bucket_size;
                    std::swap(*it, cache_.back());
                    cache_.pop_back();
                    cache_hits_.fetch_add(1, std::memory_order_relaxed);
                    return raw;
                }
            }
        }

        cache_misses_.fetch_add(1, std::memory_order_relaxed);

        void* raw = VirtualMemory::reserve(bucket_size);

        VirtualMemory::bind_to_node(
            raw,
            bucket_size,
            node_id_,
            VirtualMemory::NumaPolicy::Bind
        );

        return raw;
    }

    bool release_to_cache(void* raw, size_t total_size) {
        std::lock_guard<std::mutex> lock(cache_mutex_);

        if (cache_.size() >= max_cached_spans_ ||
            cached_bytes_ + total_size > max_cached_bytes_) {
            return false;
        }

        cache_.push_back({raw, total_size});
        cached_bytes_ += total_size;
        return true;
    }

    int node_id_;
    LargeCacheConfig config_;
    bool use_configured_policy_;
    size_t max_cached_spans_;
    size_t max_cached_bytes_;
    mutable std::mutex cache_mutex_;
    std::vector<CachedSpan> cache_;
    size_t cached_bytes_ = 0;

    std::atomic<uint64_t> cache_hits_{0};
    std::atomic<uint64_t> cache_misses_{0};
    std::atomic<uint64_t> requested_bytes_{0};
    std::atomic<uint64_t> bucket_bytes_{0};
};