#include <catch2/catch_test_macros.hpp>

#include "common/test_utils.hpp"
#include "thread_local/thread_local_cache.hpp"

#include <atomic>
#include <cstddef>
#include <thread>

// Verifies that a pinned thread initializes its cache for the CPU's NUMA node.
TEST_CASE("multi-node unit: thread cache initializes on pinned NUMA nodes", "[multi_node][unit][thread_cache]") {
    const auto nodes = numa_test::two_test_nodes();
    std::atomic<bool> failed{false};

    for (int node : nodes) {
        std::thread worker([node, &failed] {
            try {
                numa_test::ScopedThreadPin pin(node);
                ThreadLocalCache::configure_current(true);
                auto& cache = ThreadLocalCache::current();
                if (cache.node_id() != node) {
                    failed.store(true);
                }
            } catch (...) {
                failed.store(true);
            }
        });
        worker.join();
    }

    REQUIRE_FALSE(failed.load());
}

// Verifies that cached small blocks are reused by the owning thread.
TEST_CASE("multi-node unit: thread cache reuses same-thread small blocks", "[multi_node][unit][thread_cache][reuse]") {
    const auto nodes = numa_test::two_test_nodes();
    std::atomic<bool> failed{false};

    std::thread worker([node = nodes[0], &failed] {
        try {
            numa_test::ScopedThreadPin pin(node);
            ThreadLocalCache::configure_current(true);
            auto& cache = ThreadLocalCache::current();
            constexpr std::size_t size = 256;

            void* first = cache.allocate(size, alignof(std::max_align_t));
            cache.deallocate(first);
            void* second = cache.allocate(size, alignof(std::max_align_t));

            if (second != first) {
                failed.store(true);
            }

            cache.deallocate(second);
        } catch (...) {
            failed.store(true);
        }
    });
    worker.join();

    REQUIRE_FALSE(failed.load());
}

// Verifies no-cache mode still routes allocations through the current node arena.
TEST_CASE("multi-node unit: thread cache supports disabled cache mode", "[multi_node][unit][thread_cache][no_cache]") {
    const auto nodes = numa_test::two_test_nodes();
    std::atomic<bool> failed{false};

    std::thread worker([node = nodes[1], &failed] {
        try {
            numa_test::ScopedThreadPin pin(node);
            ThreadLocalCache::configure_current(false);
            auto& cache = ThreadLocalCache::current();

            for (std::size_t size : numa_test::mixed_sizes()) {
                void* ptr = cache.allocate(size, alignof(std::max_align_t));
                if (!ptr || cache.node_id() != node) {
                    failed.store(true);
                    return;
                }

                numa_test::touch_memory(ptr, size);
                cache.deallocate(ptr);
            }
        } catch (...) {
            failed.store(true);
        }
    });
    worker.join();

    REQUIRE_FALSE(failed.load());
}

// Verifies explicit configuration rebuilds the cache when the thread moves nodes.
TEST_CASE("multi-node unit: thread cache reconfigures on node change", "[multi_node][unit][thread_cache][reconfigure]") {
    const auto nodes = numa_test::two_test_nodes();

    {
        numa_test::ScopedThreadPin pin(nodes[0]);
        ThreadLocalCache::configure_current(true);
        REQUIRE(ThreadLocalCache::current().node_id() == nodes[0]);
    }

    {
        numa_test::ScopedThreadPin pin(nodes[1]);
        ThreadLocalCache::configure_current(false);
        REQUIRE(ThreadLocalCache::current().node_id() == nodes[1]);
    }
}

// Verifies that a block allocated on one node can be freed by a cache on another node.
TEST_CASE("multi-node unit: thread cache handles cross-node frees", "[multi_node][unit][thread_cache][foreign]") {
    const auto nodes = numa_test::two_test_nodes();
    std::atomic<bool> failed{false};
    void* ptr = nullptr;

    std::thread allocator([node = nodes[0], &failed, &ptr] {
        try {
            numa_test::ScopedThreadPin pin(node);
            ThreadLocalCache::configure_current(true);
            auto& cache = ThreadLocalCache::current();
            ptr = cache.allocate(512, alignof(std::max_align_t));
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
            ThreadLocalCache::configure_current(true);
            auto& cache = ThreadLocalCache::current();
            cache.deallocate(ptr);
        } catch (...) {
            failed.store(true);
        }
    });
    freer.join();

    REQUIRE_FALSE(failed.load());
}
