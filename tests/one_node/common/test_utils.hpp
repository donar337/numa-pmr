#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory_resource>

namespace numa_test {

inline void touch_memory(void* ptr, std::size_t size, unsigned char value = 0xA5) {
    std::memset(ptr, value, size == 0 ? 1 : size);
}

inline bool is_aligned(void* ptr, std::size_t alignment) {
    if (alignment == 0) {
        return true;
    }

    return reinterpret_cast<std::uintptr_t>(ptr) % alignment == 0;
}

constexpr std::array<std::size_t, 12> small_sizes() {
    return {0, 1, 8, 16, 17, 63, 64, 65, 511, 512, 513, 4096};
}

constexpr std::array<std::size_t, 8> mixed_sizes() {
    return {1, 32, 513, 1024, 2048, 4096, 4097, 8192};
}

inline void allocate_touch_free(std::pmr::memory_resource& resource,
                                std::size_t size,
                                std::size_t alignment = alignof(std::max_align_t)) {
    void* ptr = resource.allocate(size, alignment);
    touch_memory(ptr, size);
    resource.deallocate(ptr, size, alignment);
}

} // namespace numa_test
