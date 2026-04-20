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
        FirstTouch, // unused
        Bind,
        Interleave  // unused
    };

    // Maps a private read/write anonymous region. `size` is rounded up to a
    // whole number of pages; if `size == 0`, reserves one page. Throws
    // std::bad_alloc if mmap fails.
    static void* reserve(size_t size);

    // Maps memory and applies a NUMA policy to it. If binding fails, releases
    // the mapping and throws std::bad_alloc.
    static void* alloc_on_node(size_t size,
                               int node,
                               NumaPolicy policy = NumaPolicy::Bind);

    // Unmaps [ptr, ptr + align_up(size, page_size())). No-op if `ptr` is null
    // or `size == 0`. Pass a covering length of the original `reserve`.
    static void release(void* ptr, size_t size);

    /**
     * Applies a NUMA policy to the memory region [ptr, ptr + align_up(size, page_size())) using mbind.
     * The function does not migrate any existing pages (flags == 0).
     *
     * - FirstTouch: No syscall is performed; returns true. Leaves kernel's default behavior.
     * - Bind: Applies MPOL_BIND policy to the specified `node`.
     * - Interleave: Applies MPOL_INTERLEAVE policy with the same mask (same as Bind for now).
     *
     * Returns false if `ptr` is null, if `node` is out of range for the nodemask encoding, or if mbind fails.
     */
    static bool bind_to_node(void* ptr,
                             size_t size,
                             int node,
                             NumaPolicy policy = NumaPolicy::Bind);

    // unused
    // Kernel hint: prefer transparent huge pages for this range. Best-effort.
    static void advise_hugepage(void* ptr, size_t size);

    // unused
    // Kernel hint: avoid huge pages for this range.
    static void advise_no_hugepage(void* ptr, size_t size);

    // unused
    // Hint that physical pages can be dropped; virtual mapping stays. On
    // anonymous memory, content becomes zero-filled on next fault.
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
    static bool numa_is_available() noexcept;
    static unsigned long max_nodes();
};
