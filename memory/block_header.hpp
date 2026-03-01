#pragma once

#include <cstdint>

struct BlockHeader {
    uint32_t node_id;
    uint32_t size_class;
    uint64_t size;

    static BlockHeader* from_user_ptr(void* p) noexcept;
    void* to_user_ptr() noexcept;
};