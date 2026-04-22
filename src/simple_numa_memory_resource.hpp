#pragma once

#include "numa_topology/numa_topology.hpp"
#include "virtual_memory/virtual_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <new>

/**
 * A simple NUMA-aware PMR upstream resource.
 *
 * Each allocation maps a fresh span from the OS, binds it to a NUMA node, and
 * each deallocation unmaps that span. This resource intentionally does not use
 * NumaMemoryResource internals, arenas, thread-local caches etc.
 */
class SimpleNumaMemoryResource : public std::pmr::memory_resource {
public:
    /**
     * Fixes the resource to the NUMA node of the CPU where it constructs.
     * All later allocations use that stored node, even from other threads.
     */
    SimpleNumaMemoryResource() noexcept
        : node_id_(numa_topology::current_node_from_cpu()),
          exact_calculate_(false)
    {}

    /**
     * Fixes the resource to @param node_id when it exists. Invalid node ids
     * fall back to the same current-node selection as the default constructor.
     */
    explicit SimpleNumaMemoryResource(int node_id) noexcept
        : node_id_(numa_topology::normalize_node_id(node_id)),
          exact_calculate_(false)
    {}

    /**
     * Creates a resource that recalculates the current CPU's NUMA node for
     * every allocation. The stored node_id_ is only an initial fallback value.
     *
     * ! Extremely slow in normal conditions, you must well know what you are doing !
     */
    static SimpleNumaMemoryResource current_node_per_allocation() noexcept {
        return SimpleNumaMemoryResource(numa_topology::current_node_from_cpu(), true);
    }

protected:
    void* do_allocate(size_t bytes, size_t alignment) override {
        if (bytes == 0) {
            bytes = 1;
        }

        if (alignment < alignof(void*)) {
            alignment = alignof(void*);
        }

        if (alignment > std::numeric_limits<size_t>::max() - sizeof(AllocationHeader) ||
            bytes > std::numeric_limits<size_t>::max() - alignment - sizeof(AllocationHeader)) {
            throw std::bad_alloc();
        }

        const size_t total_size = VirtualMemory::align_up(
            bytes + alignment + sizeof(AllocationHeader),
            VirtualMemory::page_size()
        );
        const int node = exact_calculate_ ? numa_topology::current_node_from_cpu() : node_id_;
        void* raw = VirtualMemory::alloc_on_node(total_size, node);

        return allocate_from_span(raw, total_size, bytes, alignment);
    }

    void do_deallocate(void* p, size_t bytes, size_t alignment) override {
        (void)bytes;
        (void)alignment;

        if (!p) {
            return;
        }

        auto* header = reinterpret_cast<AllocationHeader*>(
            reinterpret_cast<std::uintptr_t>(p) - sizeof(AllocationHeader)
        );

        VirtualMemory::release(header->raw_ptr, header->total_size);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return dynamic_cast<const SimpleNumaMemoryResource*>(&other) != nullptr;
    }

private:
    struct AllocationHeader {
        void* raw_ptr;
        size_t total_size;
    };

    SimpleNumaMemoryResource(int node_id, bool exact_calculate) noexcept
        : node_id_(node_id),
          exact_calculate_(exact_calculate)
    {}

    static void* allocate_from_span(void* raw, size_t total_size, size_t bytes, size_t alignment) {
        const auto raw_addr = reinterpret_cast<std::uintptr_t>(raw);
        const auto start = VirtualMemory::align_up(raw_addr + sizeof(AllocationHeader), alignment);

        if (start + bytes > raw_addr + total_size) {
            VirtualMemory::release(raw, total_size);
            throw std::bad_alloc();
        }

        auto* header = reinterpret_cast<AllocationHeader*>(start - sizeof(AllocationHeader));
        header->raw_ptr = raw;
        header->total_size = total_size;

        return reinterpret_cast<void*>(start);
    }

    int node_id_;
    bool exact_calculate_;
};
