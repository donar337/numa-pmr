#pragma once

#include "size_divide/small_object_allocator.hpp"
#include "size_divide/large_object_allocator.hpp"
#include <array>
#include <vector>
#include <memory>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <sched.h>

class ThreadLocalSmallCache {
public:
    ~ThreadLocalSmallCache() noexcept;

    BlockHeader* pop(size_t class_index, int node_id) noexcept {
        auto& bin = bins_[class_index];
        auto** link = &bin.head;

        while (*link) {
            auto* current = *link;
            auto** next = reinterpret_cast<BlockHeader**>(current->to_user_ptr());

            if (static_cast<int>(current->node_id) == node_id) {
                *link = *next;
                --bin.count;
                bin.bytes -= current->size_class;
                return current;
            }

            link = next;
        }

        return nullptr;
    }

    bool push(BlockHeader* header, size_t class_index) noexcept {
        auto& bin = bins_[class_index];
        const size_t class_size = header->size_class;

        if (bin.bytes + class_size > SizeClassConfig::max_cached_bytes_for_class(class_size)) {
            return false;
        }

        auto** next = reinterpret_cast<BlockHeader**>(header->to_user_ptr());
        *next = bin.head;
        bin.head = header;
        ++bin.count;
        bin.bytes += class_size;
        return true;
    }

    void flush() noexcept;

private:
    struct CacheBin {
        BlockHeader* head = nullptr;
        size_t count = 0;
        size_t bytes = 0;
    };

    std::array<CacheBin, kNumSizeClasses> bins_;
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
        if (size == 0) {
            size = 1;
        }

        if (size <= SMALL_LARGE_THRESHOLD && alignment <= ALIGNMENT) {
            size_t class_index = SizeClassTable::class_index_for_size(size);
            size_t class_size = kClassSizes[class_index];
            auto* header = thread_cache().pop(class_index, node_id_);

            if (!header) {
                header = reinterpret_cast<BlockHeader*>(
                    small_.allocate_by_class_index(class_index)
                );
            }

            header->node_id = static_cast<uint32_t>(node_id_);
            header->size_class = static_cast<uint32_t>(class_size);
            header->size = 0;

            return header->to_user_ptr();
        }

        return large_.allocate(size, alignment);
    }

    void deallocate(void* ptr, size_t /*size*/, size_t /*alignment*/) {
        if (!ptr) return;

        auto* header = BlockHeader::from_user_ptr(ptr);

        if (static_cast<int>(header->node_id) != node_id_) {
            throw std::invalid_argument("deallocation routed to non-owner NUMA arena");
        }

        if (header->size_class != 0) {
            size_t class_index = SizeClassTable::class_index(header->size_class);
            if (thread_cache().push(header, class_index)) {
                return;
            }

            small_.deallocate(header);
        } else {
            large_.deallocate(ptr, header->size);
        }
    }

    int node_id() const noexcept {
        return node_id_;
    }

private:
    friend class ThreadLocalSmallCache;

    static ThreadLocalSmallCache& thread_cache() {
        static thread_local ThreadLocalSmallCache cache;
        return cache;
    }

    void deallocate_cached_small(BlockHeader* header) {
        if (static_cast<int>(header->node_id) != node_id_) {
            throw std::invalid_argument("cached block routed to non-owner NUMA arena");
        }

        small_.deallocate(header);
    }

    int node_id_;
    SmallObjectAllocator small_;
    LargeObjectAllocator large_;
};

struct NumaArenaDeleter {
    void operator()(NumaArena* arena) const noexcept;
};

using NumaArenaPtr = std::unique_ptr<NumaArena, NumaArenaDeleter>;


// ============================================================
// NUMA MANAGER
// ============================================================

class NumaManager {
public:
    static NumaManager& instance() {
        static NumaManager inst;
        return inst;
    }

    NumaArena& arena_for_current_thread() {
        int node = current_node();
        return *arenas_[node];
    }

    NumaArena& arena_for_node(int node_id) {
        if (node_id < 0 || node_id >= node_count_) {
            throw std::out_of_range("invalid NUMA node id");
        }

        return *arenas_[node_id];
    }

    int current_node() const {
        int cpu = sched_getcpu();

        if (cpu < 0 || cpu >= static_cast<int>(cpu_to_node_.size())) {
            return 0;
        }

        return cpu_to_node_[cpu];
    }

    int node_count() const noexcept {
        return node_count_;
    }

private:
    NumaManager();

    void init_topology();
    void init_single_node_topology();
    void init_arenas();
    static NumaArenaPtr create_arena_on_node(int node_id);

    int cpu_count_  = 0;
    int node_count_ = 0;

    std::vector<int> cpu_to_node_;

    std::vector<NumaArenaPtr> arenas_;
};

inline ThreadLocalSmallCache::~ThreadLocalSmallCache() noexcept {
    flush();
}

inline void ThreadLocalSmallCache::flush() noexcept {
    for (auto& bin : bins_) {
        while (bin.head) {
            BlockHeader* header = bin.head;
            auto** next = reinterpret_cast<BlockHeader**>(header->to_user_ptr());
            bin.head = *next;
            --bin.count;
            bin.bytes -= header->size_class;

            try {
                NumaManager::instance()
                    .arena_for_node(static_cast<int>(header->node_id))
                    .deallocate_cached_small(header);
            } catch (...) {
            }
        }
    }
}