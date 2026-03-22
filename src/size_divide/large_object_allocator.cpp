#include "large_object_allocator.hpp"

LargeObjectAllocator::~LargeObjectAllocator() {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    for (auto& span : cache_) {
        VirtualMemory::release(span.raw_ptr, span.bucket_size);
    }
}

LargeCacheStats LargeObjectAllocator::cache_stats() const noexcept {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    return {
        cache_hits_.load(std::memory_order_relaxed),
        cache_misses_.load(std::memory_order_relaxed),
        static_cast<uint64_t>(cache_.size()),
        static_cast<uint64_t>(cached_bytes_),
        requested_bytes_.load(std::memory_order_relaxed),
        bucket_bytes_.load(std::memory_order_relaxed),
    };
}
