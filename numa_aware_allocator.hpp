#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <vector>
#include <array>
#include <memory>
#include <cassert>
#include <new>
#include <sys/mman.h>

// ============================================================
// CONFIG
// ============================================================

struct ArenaPolicy {
    static constexpr size_t small_large_threshold = 4096;      // 4 KB
    static constexpr size_t slab_size             = 64 * 1024; // 64 KB
};

// ============================================================
// POINTER UTILS
// ============================================================

namespace pointer_utils {

inline void* add_bytes(void* p, size_t offset) noexcept {
    return static_cast<void*>(static_cast<char*>(p) + offset);
}

inline void* sub_bytes(void* p, size_t offset) noexcept {
    return static_cast<void*>(static_cast<char*>(p) - offset);
}

} // namespace pointer_utils


// ============================================================
// BLOCK HEADER
// ============================================================

struct BlockHeader {
    uint32_t node_id;
    uint32_t size_class;   // 0 = large
    uint64_t size;         // only for large allocations

    static BlockHeader* from_user_ptr(void* p) noexcept {
        return reinterpret_cast<BlockHeader*>(
            pointer_utils::sub_bytes(p, sizeof(BlockHeader))
        );
    }

    void* to_user_ptr() noexcept {
        return pointer_utils::add_bytes(this, sizeof(BlockHeader));
    }
};


// ============================================================
// VIRTUAL MEMORY BACKEND (MVP: mmap + first-touch)
// ============================================================

class VirtualMemory {
public:
    static void* reserve(size_t size) {
        void* ptr = mmap(nullptr, size,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS,
                         -1, 0);
        if (ptr == MAP_FAILED)
            throw std::bad_alloc();
        return ptr;
    }

    static void release(void* ptr, size_t size) {
        munmap(ptr, size);
    }

    static void bind_to_node(void*, size_t, int) {
        // TODO: mbind / strict NUMA binding
        // MVP: rely on first-touch
    }
};


// ============================================================
// SIZE CLASS TABLE (MVP: simple step = 16)
// ============================================================

class SizeClassTable {
public:
    static constexpr size_t kMaxSmallSize = ArenaPolicy::small_large_threshold;
    static constexpr size_t kAlignment    = 16;

    static size_t class_size(size_t size) {
        size_t aligned = (size + kAlignment - 1) / kAlignment * kAlignment;
        return aligned;
    }
};


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
        void* mem = VirtualMemory::reserve(ArenaPolicy::slab_size);
        VirtualMemory::bind_to_node(mem, ArenaPolicy::slab_size, node_id);

        auto* slab = reinterpret_cast<Slab*>(mem);
        slab->init(block_size, node_id);
        return slab;
    }

    void* allocate_block() {
        auto* header = header_ptr();
        if (!header->free_list)
            return nullptr;

        void* block = header->free_list;
        header->free_list = *reinterpret_cast<void**>(block);
        header->free_count--;
        return block;
    }

    void free_block(void* block) {
        auto* header = header_ptr();
        *reinterpret_cast<void**>(block) = header->free_list;
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

        size_t usable =
            ArenaPolicy::slab_size - sizeof(SlabHeader);

        size_t total_block_size =
            sizeof(BlockHeader) + block_size;

        header->capacity = usable / total_block_size;
        header->free_count = header->capacity;

        char* ptr = reinterpret_cast<char*>(this) + sizeof(SlabHeader);

        header->free_list = nullptr;

        for (size_t i = 0; i < header->capacity; ++i) {
            void* block = ptr + i * total_block_size;
            *reinterpret_cast<void**>(block) = header->free_list;
            header->free_list = block;
        }
    }

    SlabHeader* header_ptr() const noexcept {
        return reinterpret_cast<SlabHeader*>(
            const_cast<Slab*>(this)
        );
    }
};


// ============================================================
// SIZE CLASS (MVP)
// ============================================================

class SizeClass {
public:
    SizeClass(size_t block_size, int node_id)
        : block_size_(block_size), node_id_(node_id), current_(nullptr) {}

    void* allocate() {
        if (current_ && current_->has_free())
            return current_->allocate_block();

        current_ = Slab::create(block_size_, node_id_);
        slabs_.push_back(current_);
        return current_->allocate_block();
    }

    void deallocate(void* ptr) {
        Slab* slab = align_to_slab(ptr);
        slab->free_block(ptr);
    }

private:
    Slab* align_to_slab(void* ptr) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        uintptr_t base = addr & ~(ArenaPolicy::slab_size - 1);
        return reinterpret_cast<Slab*>(base);
    }

    size_t block_size_;
    int    node_id_;
    Slab*  current_;
    std::vector<Slab*> slabs_;  // TODO: partial/full separation
};


// ============================================================
// SMALL OBJECT ALLOCATOR
// ============================================================

class SmallObjectAllocator {
public:
    SmallObjectAllocator(int node_id)
        : node_id_(node_id)
    {}

    void* allocate(size_t size) {
        size_t cls_size = SizeClassTable::class_size(size);
        auto& sc = get_size_class(cls_size);
        return sc.allocate();
    }

    void deallocate(void* ptr, size_t size) {
        size_t cls_size = SizeClassTable::class_size(size);
        auto& sc = get_size_class(cls_size);
        sc.deallocate(ptr);
    }

private:
    SizeClass& get_size_class(size_t size) {
        auto it = classes_.find(size);
        if (it == classes_.end()) {
            auto res = classes_.emplace(
                size,
                SizeClass(size, node_id_)
            );
            return res.first->second;
        }
        return it->second;
    }

    int node_id_;
    std::unordered_map<size_t, SizeClass> classes_; // MVP
};


// ============================================================
// LARGE OBJECT ALLOCATOR
// ============================================================

class LargeObjectAllocator {
public:
    explicit LargeObjectAllocator(int node_id)
        : node_id_(node_id) {}

    void* allocate(size_t size, size_t) {
        size_t total = sizeof(BlockHeader) + size;

        void* mem = VirtualMemory::reserve(total);
        VirtualMemory::bind_to_node(mem, total, node_id_);

        auto* header = new(mem) BlockHeader{
            static_cast<uint32_t>(node_id_),
            0,
            size
        };

        return header->to_user_ptr();
    }

    void deallocate(void* ptr, size_t) {
        auto* header = BlockHeader::from_user_ptr(ptr);
        size_t total = sizeof(BlockHeader) + header->size;
        VirtualMemory::release(header, total);
    }

private:
    int node_id_;
};


// ============================================================
// NUMA ARENA
// ============================================================

class NumaArena {
public:
    explicit NumaArena(int node_id)
        : node_id_(node_id),
          small_(node_id),
          large_(node_id)
    {}

    void* allocate(size_t size, size_t alignment) {
        if (size <= ArenaPolicy::small_large_threshold) {
            void* block = small_.allocate(size);

            auto* header = new(block) BlockHeader{
                static_cast<uint32_t>(node_id_),
                static_cast<uint32_t>(size),
                0
            };

            return header->to_user_ptr();
        }

        return large_.allocate(size, alignment);
    }

    void deallocate(void* ptr, size_t size) {
        auto* header = BlockHeader::from_user_ptr(ptr);

        if (header->size_class != 0) {
            small_.deallocate(header, header->size_class);
        } else {
            large_.deallocate(ptr, size);
        }
    }

private:
    int node_id_;
    SmallObjectAllocator small_;
    LargeObjectAllocator large_;
};


// ============================================================
// NUMA MANAGER (MVP: single node stub)
// ============================================================

class NumaManager {
public:
    static NumaManager& instance() {
        static NumaManager inst;
        return inst;
    }

    NumaArena& arena_for_current_thread() {
        return *arenas_[0]; // TODO: real NUMA detection
    }

private:
    NumaManager() {
        arenas_.emplace_back(std::make_unique<NumaArena>(0));
    }

    std::vector<std::unique_ptr<NumaArena>> arenas_;
};


// ============================================================
// PMR MEMORY RESOURCE
// ============================================================

class NumaMemoryResource : public std::pmr::memory_resource {
protected:
    void* do_allocate(size_t bytes, size_t alignment) override {
        return NumaManager::instance()
            .arena_for_current_thread()
            .allocate(bytes, alignment);
    }

    void do_deallocate(void* p, size_t bytes, size_t alignment) override {
        NumaManager::instance()
            .arena_for_current_thread()
            .deallocate(p, bytes);
    }

    bool do_is_equal(const memory_resource& other) const noexcept override {
        return this == &other;
    }
};