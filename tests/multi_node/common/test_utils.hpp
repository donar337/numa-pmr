#pragma once

#include "virtual_memory/virtual_memory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <numa.h>
#include <numaif.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace numa_test {

// Touches each page in a range so NUMA placement is materialized by the kernel.
inline void touch_memory(void* ptr, std::size_t size, unsigned char value = 0xA5) {
    auto* bytes = static_cast<unsigned char*>(ptr);
    const std::size_t effective_size = size == 0 ? 1 : size;
    const std::size_t page_size = VirtualMemory::page_size();

    for (std::size_t offset = 0; offset < effective_size; offset += page_size) {
        bytes[offset] = value;
    }

    bytes[effective_size - 1] = value;
}

// Checks whether a pointer satisfies the requested alignment.
inline bool is_aligned(void* ptr, std::size_t alignment) {
    if (alignment == 0) {
        return true;
    }

    return reinterpret_cast<std::uintptr_t>(ptr) % alignment == 0;
}

// Sizes that exercise slab-backed small allocation paths and boundaries.
constexpr std::array<std::size_t, 12> small_sizes() {
    return {0, 1, 8, 16, 17, 63, 64, 65, 511, 512, 513, 4096};
}

// Sizes that cover zero, small, threshold, and large allocation paths.
constexpr std::array<std::size_t, 10> mixed_sizes() {
    return {0, 1, 32, 513, 1024, 2048, 4096, 4097, 8192, 65536};
}

// Large allocation sizes used to exercise mmap-backed paths and span caches.
constexpr std::array<std::size_t, 4> large_sizes() {
    return {4097, 8192, 65536, 1024 * 1024};
}

// Performs one full PMR allocate, write, and deallocate cycle.
inline void allocate_touch_free(std::pmr::memory_resource& resource,
                                std::size_t size,
                                std::size_t alignment = alignof(std::max_align_t)) {
    void* ptr = resource.allocate(size, alignment);
    touch_memory(ptr, size);
    resource.deallocate(ptr, size, alignment);
}

// Returns NUMA nodes that have at least one CPU allowed by the current affinity mask.
inline std::vector<int> nodes_with_allowed_cpus() {
    std::vector<int> nodes;

    if (numa_available() < 0 || numa_all_nodes_ptr == nullptr) {
        return nodes;
    }

    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    REQUIRE(sched_getaffinity(0, sizeof(allowed), &allowed) == 0);

    const int max_node = numa_max_node();
    for (int node = 0; node <= max_node; ++node) {
        if (numa_bitmask_isbitset(numa_all_nodes_ptr, static_cast<unsigned int>(node)) == 0) {
            continue;
        }

        struct bitmask* cpus = numa_allocate_cpumask();
        REQUIRE(cpus != nullptr);
        const int ret = numa_node_to_cpus(node, cpus);
        REQUIRE(ret == 0);

        bool has_allowed_cpu = false;
        for (unsigned int cpu = 0; cpu < cpus->size && cpu < CPU_SETSIZE; ++cpu) {
            if (numa_bitmask_isbitset(cpus, cpu) && CPU_ISSET(static_cast<int>(cpu), &allowed)) {
                has_allowed_cpu = true;
                break;
            }
        }

        numa_free_cpumask(cpus);

        if (has_allowed_cpu) {
            nodes.push_back(node);
        }
    }

    return nodes;
}

// Formats topology details that should be visible when multi-node tests run.
inline std::string topology_summary(const std::vector<int>& nodes) {
    std::ostringstream out;
    out << "numa_available=" << numa_available()
        << ", numa_max_node=" << numa_max_node()
        << ", allowed_nodes=[";

    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << nodes[i];
    }

    out << "], cpu_to_node=[";
    const long cpu_count = sysconf(_SC_NPROCESSORS_CONF);
    for (long cpu = 0; cpu < cpu_count; ++cpu) {
        if (cpu != 0) {
            out << ',';
        }
        out << cpu << ':' << numa_node_of_cpu(static_cast<int>(cpu));
    }
    out << ']';

    return out.str();
}

// Requires a real multi-node NUMA machine and logs the detected topology once.
inline const std::vector<int>& require_real_numa_system() {
    static const std::vector<int> nodes = nodes_with_allowed_cpus();
    static bool logged = false;

    if (!logged) {
        WARN(topology_summary(nodes));
        logged = true;
    }

    REQUIRE(numa_available() >= 0);
    REQUIRE(numa_all_nodes_ptr != nullptr);
    REQUIRE(nodes.size() >= 2);
    return nodes;
}

// Selects the first two usable NUMA nodes for cross-node test scenarios.
inline std::array<int, 2> two_test_nodes() {
    const auto& nodes = require_real_numa_system();
    return {nodes[0], nodes[1]};
}

// Restores the current thread affinity after code that calls sched_setaffinity directly.
class ScopedAffinityRestore {
public:
    ScopedAffinityRestore()
        : valid_(sched_getaffinity(0, sizeof(previous_), &previous_) == 0) {
        REQUIRE(valid_);
    }

    ~ScopedAffinityRestore() noexcept {
        if (valid_) {
            sched_setaffinity(0, sizeof(previous_), &previous_);
        }
    }

    ScopedAffinityRestore(const ScopedAffinityRestore&) = delete;
    ScopedAffinityRestore& operator=(const ScopedAffinityRestore&) = delete;

private:
    cpu_set_t previous_{};
    bool valid_ = false;
};

// Temporarily pins the current thread to CPUs from one NUMA node, then restores affinity.
class ScopedThreadPin {
public:
    explicit ScopedThreadPin(int node_id)
        : valid_(sched_getaffinity(0, sizeof(previous_), &previous_) == 0) {
        REQUIRE(valid_);

        cpu_set_t next;
        CPU_ZERO(&next);

        struct bitmask* cpus = numa_allocate_cpumask();
        REQUIRE(cpus != nullptr);
        const int ret = numa_node_to_cpus(node_id, cpus);
        REQUIRE(ret == 0);

        bool has_cpu = false;
        for (unsigned int cpu = 0; cpu < cpus->size && cpu < CPU_SETSIZE; ++cpu) {
            if (numa_bitmask_isbitset(cpus, cpu) && CPU_ISSET(static_cast<int>(cpu), &previous_)) {
                CPU_SET(static_cast<int>(cpu), &next);
                has_cpu = true;
            }
        }

        numa_free_cpumask(cpus);

        REQUIRE(has_cpu);
        REQUIRE(sched_setaffinity(0, sizeof(next), &next) == 0);
    }

    ~ScopedThreadPin() noexcept {
        if (valid_) {
            sched_setaffinity(0, sizeof(previous_), &previous_);
        }
    }

    ScopedThreadPin(const ScopedThreadPin&) = delete;
    ScopedThreadPin& operator=(const ScopedThreadPin&) = delete;

private:
    cpu_set_t previous_{};
    bool valid_ = false;
};

// Asserts that the current CPU belongs to the expected NUMA node.
inline void require_current_thread_on_node(int expected_node) {
    const int cpu = sched_getcpu();
    REQUIRE(cpu >= 0);
    REQUIRE(numa_node_of_cpu(cpu) == expected_node);
}

// Verifies physical page placement using move_pages; query failures are test failures.
inline void require_pages_on_node(void* ptr, std::size_t size, int expected_node) {
    REQUIRE(ptr != nullptr);

    const std::size_t page_size = VirtualMemory::page_size();
    const std::size_t effective_size = size == 0 ? page_size : VirtualMemory::align_up(size, page_size);
    const std::size_t page_count = effective_size / page_size;

    touch_memory(ptr, effective_size, static_cast<unsigned char>(expected_node + 1));

    std::vector<void*> pages;
    pages.reserve(page_count);
    auto* base = static_cast<unsigned char*>(ptr);
    for (std::size_t page = 0; page < page_count; ++page) {
        pages.push_back(base + page * page_size);
    }

    std::vector<int> status(page_count, -1);
    const long ret = move_pages(
        0,
        static_cast<unsigned long>(pages.size()),
        pages.data(),
        nullptr,
        status.data(),
        0
    );

    INFO("move_pages errno=" << errno << " (" << std::strerror(errno) << ")");
    REQUIRE(ret == 0);

    for (int actual_node : status) {
        REQUIRE(actual_node == expected_node);
    }
}

} // namespace numa_test
