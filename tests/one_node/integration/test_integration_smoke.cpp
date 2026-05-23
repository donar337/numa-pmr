#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory_resource>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/test_utils.hpp"
#include "numa_arena_memory_resource.hpp"
#include "numa_memory_resource.hpp"
#include "numa_simple_memory_resource.hpp"

namespace {

void exercise_direct_allocations(std::pmr::memory_resource& resource) {
    for (std::size_t size : numa_test::mixed_sizes()) {
        numa_test::allocate_touch_free(resource, size);
    }
}

void exercise_alignment(std::pmr::memory_resource& resource) {
    constexpr std::size_t size = 128;
    constexpr std::size_t alignment = 64;

    void* ptr = resource.allocate(size, alignment);
    REQUIRE(numa_test::is_aligned(ptr, alignment));
    numa_test::touch_memory(ptr, size);
    resource.deallocate(ptr, size, alignment);
}

void exercise_pmr_containers(std::pmr::memory_resource* resource) {
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

    std::pmr::unordered_map<int, std::pmr::string> rows(resource);
    rows.reserve(128);
    for (int i = 0; i < 128; ++i) {
        std::pmr::string payload(resource);
        payload.append(static_cast<std::size_t>((i % 31) + 1), static_cast<char>('a' + (i % 26)));
        rows.emplace(i, std::move(payload));
    }

    REQUIRE(rows.size() == 128);
    REQUIRE(rows.at(0).size() == 1);
    REQUIRE(rows.at(30).size() == 31);
}

void exercise_pool_reuse(std::pmr::memory_resource& resource) {
    constexpr std::size_t batch_size = 128;
    constexpr std::size_t size = 96;
    std::vector<void*> ptrs(batch_size);

    for (void*& ptr : ptrs) {
        ptr = resource.allocate(size, alignof(std::max_align_t));
        numa_test::touch_memory(ptr, size);
    }

    for (void* ptr : ptrs) {
        resource.deallocate(ptr, size, alignof(std::max_align_t));
    }

    for (void*& ptr : ptrs) {
        ptr = resource.allocate(size, alignof(std::max_align_t));
        numa_test::touch_memory(ptr, size, 0x5A);
    }

    for (void* ptr : ptrs) {
        resource.deallocate(ptr, size, alignof(std::max_align_t));
    }
}

} // namespace

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
    numa_memory_resource resource(false);

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

    exercise_pmr_containers(resource);
}

TEST_CASE("one-node integration: numa_arena_memory_resource handles allocation cycles", "[one_node][integration][arena_pmr]") {
    numa_arena_memory_resource resource;

    exercise_direct_allocations(resource);
    exercise_alignment(resource);
}

TEST_CASE("one-node integration: numa_arena_memory_resource works with PMR containers", "[one_node][integration][arena_pmr]") {
    numa_arena_memory_resource resource;

    exercise_pmr_containers(&resource);
}

TEST_CASE("one-node integration: numa_arena_memory_resource no-sync works in one thread", "[one_node][integration][arena_pmr]") {
    numa_arena_memory_resource resource(false);

    for (std::size_t round = 0; round < 4; ++round) {
        for (std::size_t size : numa_test::mixed_sizes()) {
            numa_test::allocate_touch_free(resource, size);
        }
    }
}

TEST_CASE("one-node integration: numa_simple_memory_resource handles direct and container allocations", "[one_node][integration][pmr_direct][simple_pmr]") {
    numa_simple_memory_resource resource;

    exercise_direct_allocations(resource);
    exercise_alignment(resource);
    exercise_pmr_containers(&resource);
}

TEST_CASE("one-node integration: monotonic_buffer_resource works over NUMA resources", "[one_node][integration][pmr_composition][monotonic]") {
    SECTION("over numa_memory_resource") {
        numa_memory_resource upstream;
        std::pmr::monotonic_buffer_resource resource(&upstream);

        exercise_direct_allocations(resource);
        exercise_pmr_containers(&resource);
        resource.release();
        exercise_direct_allocations(resource);
    }

    SECTION("over numa_arena_memory_resource no-sync") {
        numa_arena_memory_resource upstream(false);
        std::pmr::monotonic_buffer_resource resource(&upstream);

        exercise_direct_allocations(resource);
        exercise_pmr_containers(&resource);
        resource.release();
        exercise_pmr_containers(&resource);
    }

    SECTION("over numa_simple_memory_resource") {
        numa_simple_memory_resource upstream;
        std::pmr::monotonic_buffer_resource resource(&upstream);

        exercise_direct_allocations(resource);
        exercise_pmr_containers(&resource);
        resource.release();
        exercise_direct_allocations(resource);
    }
}

TEST_CASE("one-node integration: unsynchronized_pool_resource works over numa_simple_memory_resource", "[one_node][integration][pmr_composition][pool]") {
    numa_simple_memory_resource upstream;
    std::pmr::unsynchronized_pool_resource resource(&upstream);

    exercise_direct_allocations(resource);
    exercise_pmr_containers(&resource);
    exercise_pool_reuse(resource);
}

TEST_CASE("one-node integration: synchronized_pool_resource works over numa_simple_memory_resource", "[one_node][integration][pmr_composition][pool][thread]") {
    numa_simple_memory_resource upstream;
    std::pmr::synchronized_pool_resource resource(&upstream);

    exercise_direct_allocations(resource);
    exercise_pmr_containers(&resource);
    exercise_pool_reuse(resource);

    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;

    for (int thread_id = 0; thread_id < 4; ++thread_id) {
        threads.emplace_back([&resource, &failed, thread_id] {
            try {
                std::pmr::vector<int> values(&resource);
                std::pmr::unordered_map<int, int> index(&resource);

                for (int i = 0; i < 256; ++i) {
                    values.push_back(thread_id * 256 + i);
                    index.emplace(i, values.back());
                }

                if (values.size() != 256 || index.at(255) != thread_id * 256 + 255) {
                    failed.store(true);
                }
            } catch (...) {
                failed.store(true);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    REQUIRE_FALSE(failed.load());
}

// Verifies that all NUMA PMR resource objects are interchangeable for PMR equality.
TEST_CASE("one-node integration: NUMA resources compare equal", "[one_node][integration][pmr]") {
    numa_memory_resource default_resource;
    numa_memory_resource no_cache_resource(false);

    REQUIRE(default_resource.is_equal(no_cache_resource));
    REQUIRE(no_cache_resource.is_equal(*get_numa_memory_resource()));
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
