#pragma once

#include "block.hpp"
#include <algorithm>
#include <array>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

// ============================================================
// SLAB
// ============================================================

class Slab {
public:
    struct SlabHeader {
        uint32_t node_id;
        uint32_t block_size;
        uint32_t capacity;
        uint32_t free_count;
        void*    free_list;
    };

    static Slab* create(size_t block_size, int node_id) {
        void* mem = VirtualMemory::reserve(SLAB_SIZE);

        VirtualMemory::bind_to_node(
            mem,
            SLAB_SIZE,
            node_id,
            VirtualMemory::NumaPolicy::Bind
        );

        auto* slab = reinterpret_cast<Slab*>(mem);
        slab->init(block_size, node_id);
        return slab;
    }

    void* allocate_block() {
        auto* header = header_ptr();
        if (!header->free_list)
            return nullptr;

        void* block = header->free_list;

        auto* bh = reinterpret_cast<BlockHeader*>(block);
        bh->raw_ptr = this;
        bh->total_size = SLAB_SIZE;

        void* user_ptr = bh->to_user_ptr();
        
        header->free_list = *reinterpret_cast<void**>(user_ptr);
        header->free_count--;
        
        return block;
    }

    void free_block(void* block) {
        auto* header = header_ptr();
        auto* bh = reinterpret_cast<BlockHeader*>(block);
        void* user_ptr = bh->to_user_ptr();

        *reinterpret_cast<void**>(user_ptr) = header->free_list;
        header->free_list = block;
        header->free_count++;
    }

    bool has_free() const noexcept {
        return header_ptr()->free_count > 0;
    }

private:
    void init(size_t block_size, int node_id) {
        auto* header = header_ptr();
        header->node_id   = node_id;
        header->block_size = block_size;

        char* raw = reinterpret_cast<char*>(this) + sizeof(SlabHeader);
        char* ptr = reinterpret_cast<char*>(
            VirtualMemory::align_up(reinterpret_cast<uintptr_t>(raw), ALIGNMENT)
        );

        size_t usable = SLAB_SIZE - (ptr - reinterpret_cast<char*>(this));

        size_t payload_size = std::max(block_size, sizeof(void*));

        size_t total_block_size =
            VirtualMemory::align_up(sizeof(BlockHeader) + payload_size, ALIGNMENT);

        header->capacity = usable / total_block_size;
        header->free_count = header->capacity;

        header->free_list = nullptr;

        for (size_t i = 0; i < header->capacity; ++i) {
            char* raw_block = ptr + i * total_block_size;

            auto* bh = reinterpret_cast<BlockHeader*>(raw_block);

            void* user_ptr = bh->to_user_ptr();

            *reinterpret_cast<void**>(user_ptr) = header->free_list;

            header->free_list = raw_block;
        }
    }

    SlabHeader* header_ptr() const noexcept {
        return reinterpret_cast<SlabHeader*>(
            const_cast<Slab*>(this)
        );
    }
};


// ============================================================
// NODE LOCAL METADATA ALLOCATOR
// ============================================================

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
        void* mem = VirtualMemory::reserve(bytes);

        VirtualMemory::bind_to_node(
            mem,
            bytes,
            node_id_,
            VirtualMemory::NumaPolicy::Bind
        );

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


// ============================================================
// SIZE CLASS
// ============================================================

class SizeClass {
public:
    SizeClass(size_t block_size, int node_id)
        : block_size_(block_size),
          node_id_(node_id),
          current_(nullptr),
          slabs_(NodeLocalAllocator<Slab*>(node_id)) {}

    void* allocate() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (current_ && current_->has_free())
            return current_->allocate_block();

        for (auto* slab : slabs_) {
            if (slab->has_free()) {
                current_ = slab;
                return current_->allocate_block();
            }
        }

        current_ = Slab::create(block_size_, node_id_);
        slabs_.push_back(current_);
        return current_->allocate_block();
    }

    void deallocate(void* ptr) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto* block_header = reinterpret_cast<BlockHeader*>(ptr);
        auto* slab = static_cast<Slab*>(block_header->raw_ptr);

        if (!slab) {
            throw std::invalid_argument("small allocation has no owning slab");
        }

        slab->free_block(ptr);

        auto* header = reinterpret_cast<Slab::SlabHeader*>(slab);

        if (header->free_count == header->capacity) {
            if (current_ == slab)
                current_ = nullptr;

            auto it = std::find(slabs_.begin(), slabs_.end(), slab);
            if (it != slabs_.end()) {
                std::swap(*it, slabs_.back());
                slabs_.pop_back();
            }

            VirtualMemory::release(slab, SLAB_SIZE);
        }
    }

private:
    size_t block_size_;
    int    node_id_;
    Slab*  current_;
    std::vector<Slab*, NodeLocalAllocator<Slab*>> slabs_;
    std::mutex mutex_;
};


// ============================================================
// SIZE CLASS TABLE
// ============================================================

class SizeClassTable {
public:
    static constexpr size_t kMaxSmallSize = SMALL_LARGE_THRESHOLD;

    static constexpr size_t class_size(size_t size) {
        for (size_t i = 0; i < SizeClassConfig::kNumBounds; ++i) {
            if (size <= SizeClassConfig::thresholds[i]) {
                return VirtualMemory::align_up(size, SizeClassConfig::alignments[i]);
            }
        }
        return VirtualMemory::align_up(size, SizeClassConfig::alignments[SizeClassConfig::kNumBounds - 1]);
    }
};


// ============================================================
// SMALL OBJECT ALLOCATOR
// ============================================================

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

class SmallObjectAllocator {
public:
    explicit SmallObjectAllocator(int node_id)
        : SmallObjectAllocator(node_id, std::make_index_sequence<kNumSizeClasses>{})
    {}

private:
    template <std::size_t... I>
    SmallObjectAllocator(int node_id, std::index_sequence<I...>)
        : node_id_(node_id),
          classes_{SizeClass(kClassSizes[I], node_id)...}
    {}

public:

    void* allocate(size_t size) {
        size_t cls_size = SizeClassTable::class_size(size);
        auto& sc = get_size_class(cls_size);
        return sc.allocate();
    }

    void deallocate(void* block) {
        auto* header = reinterpret_cast<BlockHeader*>(block);
        size_t cls_size = header->size_class;
    
        auto& sc = get_size_class(cls_size);
        sc.deallocate(block);
    }

private:
    SizeClass& get_size_class(size_t size) {
        for (size_t i = 0; i < kNumSizeClasses; ++i) {
            if (kClassSizes[i] == size)
                return classes_[i];
        }
    
        throw std::out_of_range("invalid small allocation size class");
    }

    int node_id_;
    std::array<SizeClass, kNumSizeClasses> classes_;
};