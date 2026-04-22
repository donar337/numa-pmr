#include "large_object_allocator.hpp"

LargeObjectAllocator::~LargeObjectAllocator() {
    OptionalMutexLock lock(cache_mutex_, sync_);

    for (const auto& bin : bins_) {
        for (size_t i = 0; i < bin.count; ++i) {
            const auto& span = bin.spans[i];
            VirtualMemory::release(span.raw_ptr, span.size);
        }
    }
}