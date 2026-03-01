#pragma once

#include "size_class.hpp"
#include <cstddef>
#include <array>

#define kNumSizeClasses 16

class SmallObjectAllocator {
public:
    void* allocate(size_t size);
    void  deallocate(void* ptr, size_t size);

private:
    SizeClass& select_size_class(size_t size);

    std::array<SizeClass, kNumSizeClasses> classes_;
};  