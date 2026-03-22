#pragma once

#include <cstddef>
#include <cstdint>

#include <unistd.h>

// ============================================================
// VirtualMemory
// ============================================================

class VirtualMemory {
public:
    enum class NumaPolicy {
        FirstTouch,
        Bind,
        Interleave
    };

    static void* reserve(size_t size);
    static void release(void* ptr, size_t size);
    static bool bind_to_node(void* ptr,
                             size_t size,
                             int node,
                             NumaPolicy policy = NumaPolicy::Bind);
    static void advise_hugepage(void* ptr, size_t size);
    static void advise_no_hugepage(void* ptr, size_t size);
    static void advise_release(void* ptr, size_t size);
    static size_t page_size() {
        static size_t ps = sysconf(_SC_PAGESIZE);
        return ps;
    }

    static constexpr size_t align_up(size_t x, size_t align) {
        if (align == 0) {
            return x;
        }

        return ((x + align - 1) / align) * align;
    }

private:
    static unsigned long max_nodes();
};