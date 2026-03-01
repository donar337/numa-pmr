#pragma once

#include <cstddef>

namespace pointer_utils {

template<typename T>
T* add_bytes(void* p, size_t offset);

template<typename T>
T* subtract_bytes(void* p, size_t offset);

}