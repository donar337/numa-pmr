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

class LargeObjectAllocator {
public:
    explicit LargeObjectAllocator(int node_id, size_t max_cached_spans = 64, size_t max_cached_bytes = 64 * 1024 * 1024)
        : node_id_(node_id),
          max_cached_spans_(max_cached_spans),
          max_cached_bytes_(max_cached_bytes) {
        cache_.reserve(max_cached_spans_);
    }

    ~LargeObjectAllocator();

    /**
     * Allocates a large object.
     *
     * @param size          Requested user size in bytes.
     * @param alignment     Required alignment.
     *
     * @return A pointer to the user region, aligned to @param alignment , with header immediately below it.
     */
    void* allocate(size_t size, size_t alignment) {
        if (alignment < alignof(void*)) {
            alignment = alignof(void*);
        }

        if (alignment > std::numeric_limits<size_t>::max() - sizeof(BlockHeader) ||
            size > std::numeric_limits<size_t>::max() - alignment - sizeof(BlockHeader)) {
            throw std::bad_alloc();
        }

        size_t total = size + alignment + sizeof(BlockHeader);
        const size_t total_size = exact_size_for(total);
        return allocate_from_span(size, alignment, total_size, acquire_span_exact(total_size));
    }

    /**
     * Places a BlockHeader and user payload inside a contiguous span obtained from the OS or cache.
     *
     * @param size         Requested user size in bytes.
     * @param alignment    Required alignment.
     * @param span_size    Full span size. Must cover header, alignment padding, and @param size.
     * @param raw          Base address of the span.
     *
     * @return A pointer to the user region, aligned to @param alignment , with header immediately below it.
     */
    void* allocate_from_span(size_t size, size_t alignment, size_t span_size, void* raw) {
        uintptr_t raw_addr = reinterpret_cast<uintptr_t>(raw);
        uintptr_t start = VirtualMemory::align_up(raw_addr + sizeof(BlockHeader), alignment);
        if (start + size > raw_addr + span_size) {
            throw std::bad_alloc();
        }

        void* user_ptr = reinterpret_cast<void*>(start);

        auto* header = reinterpret_cast<BlockHeader*>(
            start - sizeof(BlockHeader)
        );

        header->node_id = static_cast<uint32_t>(node_id_);
        header->size_class = 0; // large
        header->size = size;

        header->raw_ptr = raw;
        header->total_size = span_size;

        return user_ptr;
    }

    /**
     * Returns a large allocation to the per-allocator span cache or unmaps the backing span.
     *
     * @param ptr   User pointer previously returned by allocate().
     */
    void deallocate(void* ptr) {
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
        size_t size;
    };

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

    /**
     * Obtains a writable span of exactly @param size bytes for this allocator's NUMA node.
     * Tryes to find a span in the cache, if cant, maps new one.
     *
     * @param size  Span length in bytes (page-aligned in normal allocate paths).
     *
     * @return  Base pointer suitable for allocate_from_span.
     */
    void* acquire_span_exact(size_t size) {
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);

            for (auto it = cache_.begin(); it != cache_.end(); ++it) {
                if (it->size == size) {
                    void* raw = it->raw_ptr;
                    cached_bytes_ -= it->size;
                    std::swap(*it, cache_.back());
                    cache_.pop_back();
                    return raw;
                }
            }
        }

        void* raw = VirtualMemory::reserve(size);

        VirtualMemory::bind_to_node(
            raw,
            size,
            node_id_,
            VirtualMemory::NumaPolicy::Bind
        );

        return raw;
    }

    /**
     * Places a span into the cache if it fits.
     *
     * @param raw           Base address of the span.
     * @param total_size    Full span size.
     *
     * @return True if the span was cached, false if it was not.
     */
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
    size_t max_cached_spans_;
    size_t max_cached_bytes_;
    mutable std::mutex cache_mutex_;
    std::vector<CachedSpan> cache_;
    size_t cached_bytes_ = 0;
};