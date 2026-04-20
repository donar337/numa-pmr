#pragma once

#include "../block.hpp"
#include <algorithm>
#include <limits>
#include <mutex>

// ============================================================
// LARGE OBJECT ALLOCATOR
// ============================================================

class LargeObjectAllocator {
public:
    explicit LargeObjectAllocator(
        int node_id,
        size_t max_cached_spans = LargeObjectConfig::kMaxLargeCachedSpans,
        size_t max_cached_bytes = LargeObjectConfig::kMaxLargeCacheBytes)
        : node_id_(node_id),
          max_cached_spans_(max_cached_spans),
          max_cached_bytes_(max_cached_bytes) {}

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

        const size_t total = size + alignment + sizeof(BlockHeader);
        const size_t span_size = class_size_for(total);
        const CachedSpan span = acquire_span(span_size);
        return allocate_from_span(size, alignment, span.size, span.raw_ptr);
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
        void* raw_ptr = nullptr;
        size_t size = 0;
    };

    struct SpanBin {
        CachedSpan spans[LargeObjectConfig::kMaxSpansPerLargeBin];
        size_t count = 0;
        size_t bytes = 0;
    };

private:
    static size_t page_size_for(size_t required_size) {
        return VirtualMemory::align_up(required_size, VirtualMemory::page_size());
    }

    /**
     * Rounds required allocation bytes to a configured large span class.
     * Requests beyond the largest class keep their page-aligned size and bypass the cache on free.
     */
    static size_t class_size_for(size_t required_size) {
        const size_t page_size = page_size_for(required_size);
        const size_t class_size = LargeObjectConfig::class_size_for(page_size);
        return page_size_for(class_size);
    }

    /**
     * Obtains a writable span for this allocator's NUMA node.
     * Tries the requested bin first, then a bounded number of larger bins before mapping a new span.
     *
     * @param size  Class span length in bytes (page-aligned in normal allocate paths).
     *
     * @return  Span suitable for allocate_from_span.
     */
    CachedSpan acquire_span(size_t size) {
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);

            const size_t bin_index = LargeObjectConfig::bin_index_for(size);
            if (bin_index != LargeObjectConfig::npos) {
                const size_t last_bin = std::min(
                    LargeObjectConfig::kNumLargeBins - 1,
                    bin_index + LargeObjectConfig::kLargeBinSearchLimit);

                for (size_t i = bin_index; i <= last_bin; ++i) {
                    SpanBin& bin = bins_[i];
                    if (bin.count == 0) {
                        continue;
                    }

                    CachedSpan span = bin.spans[--bin.count];
                    bin.bytes -= span.size;
                    --cached_spans_;
                    cached_bytes_ -= span.size;
                    return span;
                }
            }
        }

        void* raw = VirtualMemory::alloc_on_node(size, node_id_);

        return {raw, size};
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

        const size_t bin_index = LargeObjectConfig::bin_index_for(total_size);
        if (bin_index == LargeObjectConfig::npos) {
            return false;
        }

        SpanBin& bin = bins_[bin_index];
        if (bin.count >= LargeObjectConfig::kMaxSpansPerLargeBin ||
            cached_spans_ >= max_cached_spans_ ||
            cached_bytes_ + total_size > max_cached_bytes_) {
            return false;
        }

        bin.spans[bin.count++] = {raw, total_size};
        bin.bytes += total_size;
        ++cached_spans_;
        cached_bytes_ += total_size;
        return true;
    }

    int node_id_;
    size_t max_cached_spans_;
    size_t max_cached_bytes_;
    mutable std::mutex cache_mutex_;
    SpanBin bins_[LargeObjectConfig::kNumLargeBins];
    size_t cached_spans_ = 0;
    size_t cached_bytes_ = 0;
};
