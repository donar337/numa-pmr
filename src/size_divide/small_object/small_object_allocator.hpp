#pragma once

#include "../block.hpp"
#include <algorithm>
#include <array>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

/**
 * Slab is a contiguous memory region allocated from the OS.
 * It is used to store small objects with same size class.
*/ 
class Slab {
public:
    struct SlabHeader { // 24 bytes on 64-bit architecture
        uint32_t node_id;
        uint32_t block_size;
        uint32_t capacity;
        uint32_t free_count;
        void*    free_list;
    };

    static Slab* create(size_t block_size, int node_id);

    void* allocate_block();
    void free_block(void* block);
    bool has_free() const noexcept;
    bool is_empty() const noexcept;

private:
    void init(size_t block_size, int node_id);
    SlabHeader* header_ptr() const noexcept;
};

/**
 * STL-compatible allocator that backs containers (e.g. std::vector<Slab*, ...>)
 * with memory reserved via VirtualMemory and bound to a fixed NUMA node.
 *
 * Grandson this allocators has allocators inside, run and eat it, while it's fast.
 */
template <typename T>
class NodeLocalAllocator {
public:
    using value_type = T;

    explicit NodeLocalAllocator(int node_id = 0) noexcept
        : node_id_(node_id) {}

    template <typename U>
    NodeLocalAllocator(const NodeLocalAllocator<U>& other) noexcept
        : node_id_(other.node_id()) {}

    T* allocate(std::size_t n) {
        if (n > static_cast<std::size_t>(-1) / sizeof(T)) { // NOLINT(bugprone-sizeof-expression)
            throw std::bad_array_new_length();
        }

        size_t bytes = n * sizeof(T); // NOLINT(bugprone-sizeof-expression)
        void* mem = VirtualMemory::alloc_on_node(bytes, node_id_);

        return static_cast<T*>(mem);
    }

    void deallocate(T* p, std::size_t n) noexcept {
        VirtualMemory::release(p, n * sizeof(T)); // NOLINT(bugprone-sizeof-expression)
    }

    int node_id() const noexcept {
        return node_id_;
    }

private:
    int node_id_;
};

template <typename T, typename U>
bool operator==(const NodeLocalAllocator<T>& lhs,
                const NodeLocalAllocator<U>& rhs) noexcept {
    return lhs.node_id() == rhs.node_id();
}

template <typename T, typename U>
bool operator!=(const NodeLocalAllocator<T>& lhs,
                const NodeLocalAllocator<U>& rhs) noexcept {
    return !(lhs == rhs);
}


/**
 * Owns all slabs for one small-object size class.
 */
class SizeClass {
public:
    SizeClass(size_t block_size, int node_id);

    /**
     * Returns a pointer to one block in some slab (layout: BlockHeader at ptr,
     * user payload follows; see BlockHeader::to_user_ptr).
     *
     * Guarantees (under this instance's mutex): reuses free blocks from
     * current_ when possible, otherwise scans slabs_, otherwise creates a new
     * slab via Slab::create on node_id_.
     */
    void* allocate();
    void* allocate_existing();
    void* allocate_new_slab();

    /**
     * @param ptr  Block pointer previously returned by allocate() for this size class.
     *
     * Guarantees (under this instance's mutex): returns the block to its slab's
     * free list. If the slab becomes fully empty and another empty slab is already kept,
     * removes this slab, otherwise may retain the slab as current_.
     */
    void deallocate(void* ptr);
    void deallocate_batch(BlockHeader* head);

private:
    void release_extra_empty_slabs_locked();

    size_t block_size_;
    int    node_id_;
    Slab*  current_;
    std::vector<Slab*, NodeLocalAllocator<Slab*>> slabs_;
    std::mutex mutex_;
};


// ============================================================
// SIZE CLASS TABLE
// ============================================================

/**
 * Pure constexpr mapping between user-requested allocation sizes and small
 * slab size classes.
 */
class SizeClassTable {
public:
    /**
     * Rounds @param size up to the size-class granularity for the first bucket
     * where @param size <= thresholds[i].
     */
    static constexpr size_t class_size(size_t size) {
        for (size_t i = 0; i < SizeClassConfig::kNumBounds; ++i) {
            if (size <= SizeClassConfig::thresholds[i]) {
                return VirtualMemory::align_up(size, SizeClassConfig::alignments[i]);
            }
        }
        return VirtualMemory::align_up(size, SizeClassConfig::alignments[SizeClassConfig::kNumBounds - 1]);
    }

    /**
     * Dense index of class_size among all distinct values produced by class_size.
     *
     * @param size must be in [1, SMALL_LARGE_THRESHOLD], 0 will be treated as 1, otherwise throws std::out_of_range.
     */
    static constexpr size_t class_index_for_size(size_t size) {
        constexpr size_t first_threshold = SizeClassConfig::thresholds[0];
        constexpr size_t second_threshold = SizeClassConfig::thresholds[1];
        constexpr size_t first_alignment = SizeClassConfig::alignments[0];
        constexpr size_t second_alignment = SizeClassConfig::alignments[1];
        constexpr size_t third_alignment = SizeClassConfig::alignments[2];
        constexpr size_t first_count = first_threshold / first_alignment;
        constexpr size_t second_count = (second_threshold - first_threshold) / second_alignment;

        if (size == 0) {
            size = 1;
        }

        if (size <= first_threshold) {
            return VirtualMemory::align_up(size, first_alignment) / first_alignment - 1;
        }

        if (size <= second_threshold) {
            return first_count +
                (VirtualMemory::align_up(size, second_alignment) - first_threshold) / second_alignment -
                1;
        }

        if (size <= SMALL_LARGE_THRESHOLD) {
            return first_count + second_count +
                (VirtualMemory::align_up(size, third_alignment) - second_threshold) / third_alignment -
                1;
        }

        throw std::out_of_range("small allocation size exceeds threshold");
    }
};



// Derives how many distinct size classes wil be at compile time.
constexpr size_t count_distinct_size_classes() {
    size_t count = 0;
    size_t last = 0;

    for (size_t s = 1; s <= SMALL_LARGE_THRESHOLD; ++s) {
        size_t cls = SizeClassTable::class_size(s);
        if (cls != last) {
            last = cls;
            count++;
        }
    }
    return count;
}
static constexpr size_t kNumSizeClasses = count_distinct_size_classes();

/**
 * Ordered list of distinct class sizes.
 */
static constexpr std::array<size_t, kNumSizeClasses> kClassSizes = []() constexpr {
    std::array<size_t, kNumSizeClasses> arr{};

    size_t idx = 0;
    size_t last = 0;

    for (size_t s = 1; s <= SMALL_LARGE_THRESHOLD; ++s) {
        size_t cls = SizeClassTable::class_size(s);
        if (cls != last) {
            arr[idx++] = cls;
            last = cls;
        }
    }

    return arr;
}();

/**
 * Entry point for small allocations on one NUMA node.
 *
 * Contains one SizeClass per size class. Concurrent calls may run in parallel
 * when they target different size classes, same class is serialized inside SizeClass.
 */
class SmallObjectAllocator {
public:
    using SlowPathDrain = void (*)(void* context, size_t class_index);

    explicit SmallObjectAllocator(int node_id)
        : SmallObjectAllocator(node_id, std::make_index_sequence<kNumSizeClasses>{})
    {}

private:
    /** Implements the public constructor via a pack expansion over kClassSizes. */
    template <std::size_t... I>
    SmallObjectAllocator(int node_id, std::index_sequence<I...>)
        : node_id_(node_id),
          classes_{SizeClass(kClassSizes[I], node_id)...}
    {}

public:

    void* allocate(size_t size);
    void* allocate_by_class_index(size_t class_index);
    void* allocate_by_class_index(
        size_t class_index,
        SlowPathDrain drain,
        void* drain_context
    );
    void deallocate(void* block);
    void deallocate_batch(size_t class_index, BlockHeader* head);

private:
    SizeClass& get_size_class(size_t size);

    int node_id_;
    std::array<SizeClass, kNumSizeClasses> classes_;
};
