#include "virtual_memory.hpp"

#include <new>

#include <sys/mman.h>

#include <numa.h>
#include <numaif.h>

void* VirtualMemory::reserve(size_t size) {
    if (size == 0) {
        size = page_size();
    }

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

void* VirtualMemory::alloc_on_node(size_t size, int node, NumaPolicy policy) {
    void* ptr = reserve(size);

    if (!bind_to_node(ptr, size, node, policy)) {
        release(ptr, size);
        throw std::bad_alloc();
    }

    return ptr;
}

void VirtualMemory::release(void* ptr, size_t size) {
    if (!ptr) return;
    if (size == 0) return;

    size = align_up(size, page_size());
    munmap(ptr, size);
}

bool VirtualMemory::bind_to_node(void* ptr,
                                 size_t size,
                                 int node,
                                 NumaPolicy policy) {
    if (policy == NumaPolicy::FirstTouch) {
        return true;
    }

    if (!ptr) {
        return false;
    }

    if (!numa_is_available()) {
        return node == 0;
    }

    size = align_up(size, page_size());

    if (node < 0 || static_cast<unsigned long>(node) >= max_nodes()) {
        return false;
    }

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

    return ret == 0;
}

void VirtualMemory::advise_hugepage(void* ptr, size_t size) {
    if (!ptr) return;

    size = align_up(size, page_size());
    madvise(ptr, size, MADV_HUGEPAGE);
}

void VirtualMemory::advise_no_hugepage(void* ptr, size_t size) {
    if (!ptr) return;

    size = align_up(size, page_size());
    madvise(ptr, size, MADV_NOHUGEPAGE);
}

void VirtualMemory::advise_release(void* ptr, size_t size) {
    if (!ptr) return;

    size = align_up(size, page_size());
    madvise(ptr, size, MADV_DONTNEED);
}

bool VirtualMemory::numa_is_available() noexcept {
    static const bool available = numa_available() >= 0;
    return available;
}

unsigned long VirtualMemory::max_nodes() {
    return sizeof(unsigned long) * 8;
}
