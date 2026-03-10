#include <catch2/catch_test_macros.hpp>

#include "common/test_utils.hpp"
#include "numa_aware_memory_resource.hpp"

#include <atomic>
#include <cstddef>
#include <memory_resource>
#include <string>
#include <thread>
#include <vector>

// Проверяет, что PMR resource обслуживает small, large и zero-size allocation cycles.
TEST_CASE("one-node integration: PMR allocation cycles are writable and reusable", "[one_node][integration][pmr]") {
    NumaMemoryResource resource;

    for (std::size_t round = 0; round < 4; ++round) {
        for (std::size_t size : numa_test::mixed_sizes()) {
            numa_test::allocate_touch_free(resource, size);
        }
    }
}

// Проверяет, что стандартные PMR-контейнеры корректно используют NUMA resource.
TEST_CASE("one-node integration: PMR containers work with NUMA resource", "[one_node][integration][pmr]") {
    NumaMemoryResource resource;

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

// Проверяет, что блок можно освободить из другого потока через header-based arena routing.
TEST_CASE("one-node integration: cross-thread free returns memory to owner arena", "[one_node][integration][thread]") {
    NumaMemoryResource resource;

    void* ptr = resource.allocate(256, alignof(std::max_align_t));
    REQUIRE(ptr != nullptr);
    numa_test::touch_memory(ptr, 256);

    std::thread freer([&resource, ptr] {
        resource.deallocate(ptr, 256, alignof(std::max_align_t));
    });

    freer.join();

    numa_test::allocate_touch_free(resource, 256);
}

// Проверяет базовую потокобезопасность при конкурентных allocation/write/free циклах.
TEST_CASE("one-node integration: concurrent allocation cycles stay valid", "[one_node][integration][thread]") {
    NumaMemoryResource resource;
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

// Проверяет публичные manager invariants для one-node режима и ошибку на invalid node id.
TEST_CASE("one-node integration: manager exposes valid arena routing", "[one_node][integration][manager]") {
    auto& manager = NumaManager::instance();

    REQUIRE(manager.node_count() >= 1);
    REQUIRE(manager.current_node() >= 0);
    REQUIRE(manager.current_node() < manager.node_count());

    auto& arena = manager.arena_for_current_thread();
    REQUIRE(arena.node_id() == manager.current_node());

    REQUIRE_THROWS_AS(manager.arena_for_node(manager.node_count()), std::out_of_range);
}
