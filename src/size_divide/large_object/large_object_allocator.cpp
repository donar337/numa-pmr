#include "large_object_allocator.hpp"

LargeObjectAllocator::~LargeObjectAllocator() {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    for (auto& span : cache_) {
        VirtualMemory::release(span.raw_ptr, span.size);
    }
}