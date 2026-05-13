#include <catch2/catch_test_macros.hpp>

#include "common/test_utils.hpp"
#include "numa_arena_memory_resource.hpp"
#include "numa_memory_resource.hpp"
#include "numa_topology/numa_thread_pin_guard.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory_resource>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

void exercise_direct_allocations(std::pmr::memory_resource& resource) {
    for (std::size_t size : numa_test::mixed_sizes()) {
        numa_test::allocate_touch_free(resource, size);
    }
}

void exercise_pmr_containers(std::pmr::memory_resource* resource) {
    std::pmr::vector<int> values(resource);
    std::pmr::unordered_map<int, std::pmr::string> rows(resource);

    for (int i = 0; i < 1024; ++i) {
        values.push_back(i);
    }
    rows.reserve(128);
    for (int i = 0; i < 128; ++i) {
        std::pmr::string payload(resource);
        payload.append(static_cast<std::size_t>((i % 31) + 1), static_cast<char>('a' + (i % 26)));
        rows.emplace(i, std::move(payload));
    }

    REQUIRE(values.size() == 1024);
    REQUIRE(values.back() == 1023);
    REQUIRE(rows.size() == 128);
    REQUIRE(rows.at(30).size() == 31);
}

} // namespace

// Verifies the main NUMA resource works on pinned threads for multiple nodes.
TEST_CASE("multi-node integration: numa_memory_resource handles allocation cycles on pinned nodes", "[multi_node][integration][pmr]") {
    const auto nodes = numa_test::two_test_nodes();
    std::atomic<bool> failed{false};

    for (int node : nodes) {
        std::thread worker([node, &failed] {
            try {
                numa_test::ScopedThreadPin pin(node);
                numa_memory_resource resource;
                exercise_direct_allocations(resource);
            } catch (...) {
                failed.store(true);
            }
        });
        worker.join();
    }

    REQUIRE_FALSE(failed.load());
}

// Verifies the main NUMA resource remains functional with thread cache disabled.
TEST_CASE("multi-node integration: numa_memory_resource works with thread cache disabled", "[multi_node][integration][pmr][no_cache]") {
    const auto nodes = numa_test::two_test_nodes();
    std::atomic<bool> failed{false};

    for (int node : nodes) {
        std::thread worker([node, &failed] {
            try {
                numa_test::ScopedThreadPin pin(node);
                numa_memory_resource resource(false);
                exercise_direct_allocations(resource);
                exercise_pmr_containers(&resource);
            } catch (...) {
                failed.store(true);
            }
        });
        worker.join();
    }

    REQUIRE_FALSE(failed.load());
}

// Verifies cached small blocks are reused through the public memory_resource API.
TEST_CASE("multi-node integration: numa_memory_resource batch reuses thread cache", "[multi_node][integration][pmr][cache]") {
    const auto nodes = numa_test::two_test_nodes();
    std::atomic<bool> failed{false};

    std::thread worker([node = nodes[0], &failed] {
        try {
            numa_test::ScopedThreadPin pin(node);
            numa_memory_resource resource(true);
            constexpr std::size_t size = 1024;
            constexpr std::size_t batch_size = 64;
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
                if (std::find(first.begin(), first.end(), ptr) == first.end()) {
                    failed.store(true);
                }
            }
            for (void* ptr : second) {
                resource.deallocate(ptr, size, alignof(std::max_align_t));
            }
        } catch (...) {
            failed.store(true);
        }
    });
    worker.join();

    REQUIRE_FALSE(failed.load());
}

// Verifies PMR containers work over static main NUMA resources in pinned workers.
TEST_CASE("multi-node integration: get_numa_memory_resource works with PMR containers", "[multi_node][integration][pmr][containers]") {
    const auto nodes = numa_test::two_test_nodes();
    std::atomic<bool> failed{false};

    for (int node : nodes) {
        std::thread worker([node, &failed] {
            try {
                numa_test::ScopedThreadPin pin(node);
                auto* resource = get_numa_memory_resource();
                exercise_pmr_containers(resource);
            } catch (...) {
                failed.store(true);
            }
        });
        worker.join();
    }

    REQUIRE_FALSE(failed.load());
}

// Verifies explicit pin guards keep allocation context on the selected current node.
TEST_CASE("multi-node integration: numa_memory_resource works under thread pin guard", "[multi_node][integration][pmr][pinning]") {
    const auto nodes = numa_test::two_test_nodes();
    std::atomic<bool> failed{false};

    for (int node : nodes) {
        std::thread worker([node, &failed] {
            try {
                numa_thread_pin_guard pin(node);
                REQUIRE(pin.pinned());
                numa_memory_resource resource;
                exercise_direct_allocations(resource);
                numa_test::require_current_thread_on_node(node);
            } catch (...) {
                failed.store(true);
            }
        });
        worker.join();
    }

    REQUIRE_FALSE(failed.load());
}

// Verifies a block allocated on one NUMA node can be freed by a worker on another node.
TEST_CASE("multi-node integration: numa_memory_resource supports cross-node frees", "[multi_node][integration][pmr][foreign]") {
    const auto nodes = numa_test::two_test_nodes();
    std::atomic<bool> failed{false};
    void* ptr = nullptr;

    std::thread allocator([node = nodes[0], &failed, &ptr] {
        try {
            numa_test::ScopedThreadPin pin(node);
            numa_memory_resource resource(true);
            ptr = resource.allocate(512, alignof(std::max_align_t));
            numa_test::touch_memory(ptr, 512);
        } catch (...) {
            failed.store(true);
        }
    });
    allocator.join();

    REQUIRE_FALSE(failed.load());
    REQUIRE(ptr != nullptr);

    std::thread freer([node = nodes[1], &failed, ptr] {
        try {
            numa_test::ScopedThreadPin pin(node);
            numa_memory_resource resource(true);
            resource.deallocate(ptr, 512, alignof(std::max_align_t));
        } catch (...) {
            failed.store(true);
        }
    });
    freer.join();

    REQUIRE_FALSE(failed.load());
}

// Verifies all main NUMA resource instances remain interchangeable for PMR equality.
TEST_CASE("multi-node integration: numa_memory_resource instances compare equal", "[multi_node][integration][pmr][equality]") {
    numa_test::require_real_numa_system();
    numa_memory_resource default_resource;
    numa_memory_resource no_cache_resource(false);
    numa_arena_memory_resource arena_resource;

    REQUIRE(default_resource.is_equal(no_cache_resource));
    REQUIRE(no_cache_resource.is_equal(*get_numa_memory_resource()));
    REQUIRE_FALSE(default_resource.is_equal(arena_resource));
}
