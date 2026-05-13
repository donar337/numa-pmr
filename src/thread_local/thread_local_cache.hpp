#pragma once

#include "numa_arena/numa_arena.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

class ThreadNumaContextOwner;

class ThreadLocalCache {
public:
    static ThreadLocalCache& current();
    static ThreadLocalCache& current(bool use_thread_cache);

    ~ThreadLocalCache() noexcept;

    ThreadLocalCache(const ThreadLocalCache&) = delete;
    ThreadLocalCache& operator=(const ThreadLocalCache&) = delete;

    void* allocate(size_t size, size_t alignment) {
        if (!use_thread_cache_) {
            return arena_->allocate(size, alignment);
        }

        if (size == 0) {
            size = 1;
        }

        if (size <= SMALL_LARGE_THRESHOLD && alignment <= ALIGNMENT) {
            const size_t class_index = SizeClassTable::class_index_for_size(size);
            const size_t class_size = kClassSizes[class_index];

            if (auto* header = take_cached(class_index)) {
                return header->to_user_ptr();
            }

            void* ptr = arena_->allocate(size, alignment);
            auto* header = BlockHeader::from_user_ptr(ptr);
            header->node_id = static_cast<std::uint32_t>(node_id_);
            header->size_class = static_cast<std::uint32_t>(class_size);
            header->size = 0;

            return ptr;
        }

        return arena_->allocate(size, alignment);
    }

    void deallocate(void* ptr) {
        if (!ptr) {
            return;
        }

        auto* header = BlockHeader::from_user_ptr(ptr);

        if (!use_thread_cache_ ||
            header->size_class == 0 ||
            static_cast<int>(header->node_id) != node_id_) {
            arena_->deallocate(ptr);
            return;
        }

        const size_t class_index = SizeClassTable::class_index_for_size(header->size_class);
        if (cache_block(header, class_index)) {
            return;
        }

        arena_->deallocate(ptr);
    }

    int node_id() const noexcept {
        return node_id_;
    }

private:
    friend class ThreadNumaContextOwner;

    struct CacheBin {
        BlockHeader* head = nullptr;
        size_t count = 0;
        size_t bytes = 0;
    };

    ThreadLocalCache(NumaArena& arena, int node_id, bool use_thread_cache) noexcept;

    static ThreadLocalCache* create_on_current_node(bool use_thread_cache);
    static void destroy(ThreadLocalCache* cache) noexcept;

    BlockHeader* take_cached(size_t class_index) noexcept {
        auto& bin = bins_[class_index];
        auto* header = bin.head;

        if (!header) {
            return nullptr;
        }

        auto** next = reinterpret_cast<BlockHeader**>(header->to_user_ptr());
        bin.head = *next;
        --bin.count;
        bin.bytes -= header->size_class;
        return header;
    }

    bool cache_block(BlockHeader* header, size_t class_index) noexcept {
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

    int node_id_;
    bool use_thread_cache_;
    NumaArena* arena_;
    std::array<CacheBin, kNumSizeClasses> bins_;
};

class ThreadNumaContextOwner {
public:
    explicit ThreadNumaContextOwner(bool use_thread_cache);
    ~ThreadNumaContextOwner() noexcept;

    ThreadLocalCache& get() noexcept {
        return *cache_;
    }

private:
    ThreadLocalCache* cache_;
};
