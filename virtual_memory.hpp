#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <algorithm>

#include <unistd.h>
#include <sys/mman.h>
#include <numaif.h>

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

    static void* reserve(size_t size) {
        size = align_up(size, page_size());

        void* ptr = mmap(nullptr,
                         size,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS,
                         -1,
                         0);

        if (ptr == MAP_FAILED) {
            throw std::bad_alloc();
        }

        return ptr;
    }

    static void release(void* ptr, size_t size) {
        if (!ptr) return;

        size = align_up(size, page_size());
        munmap(ptr, size);
    }

    static void bind_to_node(void* ptr,
                             size_t size,
                             int node,
                             NumaPolicy policy = NumaPolicy::Bind)
    {
        if (policy == NumaPolicy::FirstTouch) {
            return;
        }

        size = align_up(size, page_size());

        unsigned long nodemask = 1UL << node;

        int mode = MPOL_BIND;

        if (policy == NumaPolicy::Interleave) {
            mode = MPOL_INTERLEAVE;
        }

        long ret = mbind(ptr,
                         size,
                         mode,
                         &nodemask,
                         max_nodes(),
                         0);

        if (ret != 0) {
            throw std::runtime_error("mbind failed");
        }
    }

    static void advise_hugepage(void* ptr, size_t size) {
        if (!ptr) return;

        size = align_up(size, page_size());
        madvise(ptr, size, MADV_HUGEPAGE);
    }

    static void advise_no_hugepage(void* ptr, size_t size) {
        if (!ptr) return;

        size = align_up(size, page_size());
        madvise(ptr, size, MADV_NOHUGEPAGE);
    }

    static void advise_release(void* ptr, size_t size) {
        if (!ptr) return;

        size = align_up(size, page_size());
        madvise(ptr, size, MADV_DONTNEED);
    }

    static size_t page_size() {
        static size_t ps = sysconf(_SC_PAGESIZE);
        return ps;
    }

    static constexpr size_t align_up(size_t x, size_t align) {
        return (x + align - 1) & ~(align - 1);
    }

private:
    static unsigned long max_nodes() {
        return sizeof(unsigned long) * 8;
    }
};