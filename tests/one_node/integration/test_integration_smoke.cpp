#include <catch2/catch_test_macros.hpp>

#include "numa_arena_memory_resource.hpp"
#include "common/test_utils.hpp"
#include "numa_memory_resource.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory_resource>
#include <string>
#include <thread>
#include <vector>

// Verifies that the PMR resource handles small, large, and zero-size allocation cycles.
TEST_CASE("one-node integration: PMR allocation cycles are writable and reusable", "[one_node][integration][pmr]") {
    auto* resource = get_numa_memory_resource();

    for (std::size_t round = 0; round < 4; ++round) {
        for (std::size_t size : numa_test::mixed_sizes()) {
            numa_test::allocate_touch_free(*resource, size);
        }
    }
}

TEST_CASE("one-node integration: PMR works with thread cache disabled", "[one_node][integration][pmr]") {
    numa_memory_resource resource(false, false);

    for (std::size_t size : numa_test::mixed_sizes()) {
        numa_test::allocate_touch_free(resource, size);
    }
}

TEST_CASE("one-node integration: PMR batch reuses thread cache", "[one_node][integration][pmr]") {
    numa_memory_resource resource;
    constexpr std::size_t size = 1024;
    constexpr std::size_t batch_size = 128;
    std::vector<void*> first(batch_size);
    std::vector<void*> second(batch_size);

    for (void*& ptr : first) {
        ptr = resource.allocate(size, alignof(std::max_align_t));
    }

    for (void* ptr : first) {
        resource.deallocate(ptr, size, alignof(std::max_align_t));
    }

    for (void*& ptr : second) {
        ptr = resource.allocate(size, alignof(std::max_align_t));
    }

    for (void* ptr : second) {
        REQUIRE(std::find(first.begin(), first.end(), ptr) != first.end());
    }

    for (void* ptr : second) {
        resource.deallocate(ptr, size, alignof(std::max_align_t));
    }
}

// Verifies that standard PMR containers use the NUMA resource correctly.
TEST_CASE("one-node integration: PMR containers work with NUMA resource", "[one_node][integration][pmr]") {
    auto* resource = get_numa_memory_resource();

    std::pmr::vector<int> values(resource);
    for (int i = 0; i < 1024; ++i) {
        values.push_back(i);
    }

    REQUIRE(values.size() == 1024);
    REQUIRE(values.front() == 0);
    REQUIRE(values.back() == 1023);

    std::pmr::string text(resource);
    text.append(256, 'x');
    REQUIRE(text.size() == 256);
    REQUIRE(text[0] == 'x');
}

TEST_CASE("one-node integration: numa_arena_memory_resource handles allocation cycles", "[one_node][integration][arena_pmr]") {
    numa_arena_memory_resource resource;

    for (std::size_t size : numa_test::mixed_sizes()) {
        numa_test::allocate_touch_free(resource, size);
    }

    void* over_aligned = resource.allocate(128, 64);
    REQUIRE(numa_test::is_aligned(over_aligned, 64));
    numa_test::touch_memory(over_aligned, 128);
    resource.deallocate(over_aligned, 128, 64);
}

TEST_CASE("one-node integration: numa_arena_memory_resource works with PMR containers", "[one_node][integration][arena_pmr]") {
    numa_arena_memory_resource resource;

    std::pmr::vector<int> values(&resource);
    for (int i = 0; i < 1024; ++i) {
        values.push_back(i);
    }

    REQUIRE(values.size() == 1024);
    REQUIRE(values.front() == 0);
    REQUIRE(values.back() == 1023);

    std::pmr::string text(&resource);
    text.append(256, 'x');
    REQUIRE(text.size() == 256);
    REQUIRE(text[0] == 'x');
}

TEST_CASE("one-node integration: numa_arena_memory_resource no-sync works in one thread", "[one_node][integration][arena_pmr]") {
    numa_arena_memory_resource resource(false);

    for (std::size_t round = 0; round < 4; ++round) {
        for (std::size_t size : numa_test::mixed_sizes()) {
            numa_test::allocate_touch_free(resource, size);
        }
    }
}

// Verifies that all NUMA PMR resource objects are interchangeable for PMR equality.
TEST_CASE("one-node integration: NUMA resources compare equal", "[one_node][integration][pmr]") {
    numa_memory_resource default_resource;
    numa_memory_resource pinned_no_cache_resource(true, false);

    REQUIRE(default_resource.is_equal(pinned_no_cache_resource));
    REQUIRE(pinned_no_cache_resource.is_equal(*get_numa_memory_resource()));
}

TEST_CASE("one-node integration: numa_arena_memory_resource compares by identity", "[one_node][integration][arena_pmr]") {
    numa_arena_memory_resource first;
    numa_arena_memory_resource second;
    numa_memory_resource numa_resource;

    REQUIRE(first.is_equal(first));
    REQUIRE_FALSE(first.is_equal(second));
    REQUIRE_FALSE(first.is_equal(numa_resource));
    REQUIRE_FALSE(numa_resource.is_equal(first));
}

// Verifies that a block can be freed from another thread via header-based arena routing.
TEST_CASE("one-node integration: cross-thread free returns memory to owner arena", "[one_node][integration][thread]") {
    auto* resource = get_numa_memory_resource();

    void* ptr = resource->allocate(256, alignof(std::max_align_t));
    REQUIRE(ptr != nullptr);
    numa_test::touch_memory(ptr, 256);

    std::thread freer([resource, ptr] {
        resource->deallocate(ptr, 256, alignof(std::max_align_t));
    });

    freer.join();

    numa_test::allocate_touch_free(*resource, 256);
}

// Verifies basic thread safety under concurrent allocation/write/free cycles.
TEST_CASE("one-node integration: concurrent allocation cycles stay valid", "[one_node][integration][thread]") {
    auto* resource = get_numa_memory_resource();
    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;

    for (int thread_id = 0; thread_id < 8; ++thread_id) {
        threads.emplace_back([resource, &failed, thread_id] {
            for (std::size_t i = 0; i < 1000; ++i) {
                const auto sizes = numa_test::mixed_sizes();
                const std::size_t size = sizes[(i + static_cast<std::size_t>(thread_id)) % sizes.size()];
                void* ptr = resource->allocate(size, alignof(std::max_align_t));
                if (!ptr) {
                    failed.store(true);
                    return;
                }

                numa_test::touch_memory(ptr, size, static_cast<unsigned char>(thread_id + 1));
                resource->deallocate(ptr, size, alignof(std::max_align_t));
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    REQUIRE_FALSE(failed.load());
}

TEST_CASE("one-node integration: numa_arena_memory_resource sync mode supports concurrent cycles", "[one_node][integration][arena_pmr][thread]") {
    numa_arena_memory_resource resource(true);
    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;

    for (int thread_id = 0; thread_id < 8; ++thread_id) {
        threads.emplace_back([&resource, &failed, thread_id] {
            for (std::size_t i = 0; i < 1000; ++i) {
                const auto sizes = numa_test::mixed_sizes();
                const std::size_t size = sizes[(i + static_cast<std::size_t>(thread_id)) % sizes.size()];
                void* ptr = resource.allocate(size, alignof(std::max_align_t));
                if (!ptr) {
                    failed.store(true);
                    return;
                }

                numa_test::touch_memory(ptr, size, static_cast<unsigned char>(thread_id + 1));
                resource.deallocate(ptr, size, alignof(std::max_align_t));
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    REQUIRE_FALSE(failed.load());
}
