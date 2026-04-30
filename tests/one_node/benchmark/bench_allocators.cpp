#include "numa_arena_memory_resource.hpp"
#include "numa_memory_resource.hpp"
#include "numa_simple_memory_resource.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// backend: 0 — numa_memory_resource, 1 — std::pmr::new_delete_resource (see registration at bottom of file).
constexpr int kNumaBackend = 0;
constexpr int kNewDeleteBackend = 1;

constexpr int kUnsyncPoolOverSimple = 10;
constexpr int kSyncPoolOverSimple = 11;
constexpr int kArenaUnsync = 12;
constexpr int kArenaSync = 13;

constexpr std::array<std::size_t, 8> kMixedSmallSizes{1, 16, 17, 64, 513, 1024, 2048, 4096};
constexpr std::array<std::size_t, 7> kWorkingSetSizes{24, 64, 256, 1024, 4096, 8192, 65536};

template <typename Fn>
void run_with_comparison_resource(benchmark::State& state, int backend, Fn&& fn) {
    switch (backend) {
        case kUnsyncPoolOverSimple: {
            numa_simple_memory_resource upstream;
            std::pmr::unsynchronized_pool_resource resource(&upstream);
            fn(&resource);
            break;
        }
        case kSyncPoolOverSimple: {
            numa_simple_memory_resource upstream;
            std::pmr::synchronized_pool_resource resource(&upstream);
            fn(&resource);
            break;
        }
        case kArenaUnsync: {
            numa_arena_memory_resource resource(false);
            fn(&resource);
            break;
        }
        case kArenaSync: {
            numa_arena_memory_resource resource(true);
            fn(&resource);
            break;
        }
        default:
            state.SkipWithError("unknown comparison backend");
    }
}

std::pmr::string make_payload(std::pmr::memory_resource* mr, std::size_t size, int seed) {
    std::pmr::string payload(mr);
    payload.append(size + static_cast<std::size_t>(seed % 17), static_cast<char>('a' + (seed % 26)));
    return payload;
}

// One allocate + deallocate per state iteration (measured inside for (_ : state)).
void immediate_loop(benchmark::State& state, std::pmr::memory_resource* mr, std::size_t size) {
    for (auto _ : state) {
        void* ptr = mr->allocate(size, alignof(std::max_align_t));
        benchmark::DoNotOptimize(ptr);
        mr->deallocate(ptr, size, alignof(std::max_align_t));
    }
}

// Immediate allocate/deallocate at a fixed size; sizes include small, medium, and large (up to 1 MiB).
void BM_ImmediateAllocateFree(benchmark::State& state) {
    const auto size = static_cast<std::size_t>(state.range(0));
    const int backend = static_cast<int>(state.range(1));

    if (backend == kNumaBackend) {
        numa_memory_resource resource;
        immediate_loop(state, &resource, size);
    } else {
        std::pmr::memory_resource* resource = std::pmr::new_delete_resource();
        immediate_loop(state, resource, size);
    }

    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(size));
}

// Full batch cycle: all allocates, then all deallocates.
void batch_loop(
    benchmark::State& state,
    std::pmr::memory_resource* mr,
    std::size_t size,
    std::size_t batch_size,
    std::vector<void*>& ptrs
) {
    for (auto _ : state) {
        for (void*& ptr : ptrs) {
            void* allocated = mr->allocate(size, alignof(std::max_align_t));
            benchmark::DoNotOptimize(allocated);
            ptr = allocated;
        }

        for (void* ptr : ptrs) {
            mr->deallocate(ptr, size, alignof(std::max_align_t));
        }
    }
}

// Allocate a batch of blocks first, then free them — to observe slab/cache reuse effects.
void BM_BatchAllocateFree(benchmark::State& state) {
    const auto size = static_cast<std::size_t>(state.range(0));
    const auto batch_size = static_cast<std::size_t>(state.range(1));
    const int backend = static_cast<int>(state.range(2));
    std::vector<void*> ptrs(batch_size);

    if (backend == kNumaBackend) {
        numa_memory_resource resource;
        batch_loop(state, &resource, size, batch_size, ptrs);
    } else {
        std::pmr::memory_resource* resource = std::pmr::new_delete_resource();
        batch_loop(state, resource, size, batch_size, ptrs);
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(batch_size));
}

// One size from kMixedSmallSizes per state iteration.
void mixed_small_loop(benchmark::State& state, std::pmr::memory_resource* mr) {
    std::size_t index = 0;

    for (auto _ : state) {
        const std::size_t size = kMixedSmallSizes[index++ % kMixedSmallSizes.size()];
        void* ptr = mr->allocate(size, alignof(std::max_align_t));
        benchmark::DoNotOptimize(ptr);
        mr->deallocate(ptr, size, alignof(std::max_align_t));
    }
}

// Alternating small allocation sizes (typical noisy pattern for small-object paths).
void BM_MixedSmallAllocateFree(benchmark::State& state) {
    const int backend = static_cast<int>(state.range(0));

    if (backend == kNumaBackend) {
        numa_memory_resource resource;
        mixed_small_loop(state, &resource);
    } else {
        std::pmr::memory_resource* resource = std::pmr::new_delete_resource();
        mixed_small_loop(state, resource);
    }

    state.SetItemsProcessed(state.iterations());
}

// count push_back operations into pmr::vector on the given memory_resource.
void pmr_vector_push_back_loop(benchmark::State& state, std::pmr::memory_resource* mr, int count) {
    for (auto _ : state) {
        std::pmr::vector<int> values(mr);
        for (int i = 0; i < count; ++i) {
            values.push_back(i);
        }
        benchmark::DoNotOptimize(values.data());
    }
}

// Growing std::pmr::vector<int> via push_back (realistic PMR load + buffer reallocations).
void BM_PmrVectorPushBack(benchmark::State& state) {
    const auto count = static_cast<int>(state.range(0));
    const int backend = static_cast<int>(state.range(1));

    if (backend == kNumaBackend) {
        numa_memory_resource resource;
        pmr_vector_push_back_loop(state, &resource, count);
    } else {
        std::pmr::memory_resource* resource = std::pmr::new_delete_resource();
        pmr_vector_push_back_loop(state, resource, count);
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(count));
}

void query_scratch_loop(
    benchmark::State& state,
    std::pmr::memory_resource* mr,
    int rows,
    std::size_t payload_size
) {
    for (auto _ : state) {
        std::pmr::vector<std::pmr::string> keys(mr);
        std::pmr::unordered_map<int, std::pmr::string> values(mr);
        keys.reserve(static_cast<std::size_t>(rows));
        values.reserve(static_cast<std::size_t>(rows) * 2);

        for (int row = 0; row < rows; ++row) {
            keys.push_back(make_payload(mr, payload_size / 2 + 1, row));
            values.emplace(row, make_payload(mr, payload_size, row));
        }

        std::size_t checksum = 0;
        for (int row = 0; row < rows; row += 3) {
            auto it = values.find(row);
            if (it != values.end()) {
                it->second.push_back('!');
                checksum += it->second.size();
            }
        }

        benchmark::DoNotOptimize(keys.data());
        benchmark::DoNotOptimize(checksum);
    }
}

void BM_QueryScratchContainers(benchmark::State& state) {
    const auto rows = static_cast<int>(state.range(0));
    const auto payload_size = static_cast<std::size_t>(state.range(1));
    const int backend = static_cast<int>(state.range(2));

    run_with_comparison_resource(state, backend, [&](std::pmr::memory_resource* mr) {
        query_scratch_loop(state, mr, rows, payload_size);
    });

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(rows));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(rows) * static_cast<int64_t>(payload_size));
}

struct AllocationRecord {
    void* ptr;
    std::size_t size;
};

void mixed_lifetime_loop(
    benchmark::State& state,
    std::pmr::memory_resource* mr,
    std::size_t live_target,
    std::size_t wave_allocations
) {
    std::vector<AllocationRecord> live;
    live.reserve(live_target + wave_allocations);

    for (auto _ : state) {
        live.clear();

        for (std::size_t wave = 0; wave < 6; ++wave) {
            for (std::size_t i = 0; i < wave_allocations; ++i) {
                const std::size_t size = kWorkingSetSizes[(wave + i) % kWorkingSetSizes.size()];
                void* ptr = mr->allocate(size, alignof(std::max_align_t));
                std::memset(ptr, static_cast<int>((wave + i) & 0xFF), size);
                benchmark::DoNotOptimize(ptr);

                if ((wave + i) % 4 == 0) {
                    mr->deallocate(ptr, size, alignof(std::max_align_t));
                } else {
                    live.push_back({ptr, size});
                }
            }

            while (live.size() > live_target) {
                const AllocationRecord record = live.back();
                live.pop_back();
                mr->deallocate(record.ptr, record.size, alignof(std::max_align_t));
            }
        }

        for (const AllocationRecord record : live) {
            mr->deallocate(record.ptr, record.size, alignof(std::max_align_t));
        }
    }
}

void BM_MixedLifetimeWorkingSet(benchmark::State& state) {
    const auto live_target = static_cast<std::size_t>(state.range(0));
    const auto wave_allocations = static_cast<std::size_t>(state.range(1));
    const int backend = static_cast<int>(state.range(2));

    run_with_comparison_resource(state, backend, [&](std::pmr::memory_resource* mr) {
        mixed_lifetime_loop(state, mr, live_target, wave_allocations);
    });

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(wave_allocations) * 6);
}

void pipeline_batch_loop(
    benchmark::State& state,
    std::pmr::memory_resource* mr,
    int rows,
    std::size_t payload_size
) {
    for (auto _ : state) {
        std::pmr::vector<int> ids(mr);
        std::pmr::vector<std::uint64_t> amounts(mr);
        std::pmr::vector<std::pmr::string> payloads(mr);
        std::pmr::unordered_map<int, std::size_t> hash_index(mr);

        ids.reserve(static_cast<std::size_t>(rows));
        amounts.reserve(static_cast<std::size_t>(rows));
        payloads.reserve(static_cast<std::size_t>(rows));
        hash_index.reserve(static_cast<std::size_t>(rows) * 2);

        for (int row = 0; row < rows; ++row) {
            ids.push_back(row);
            amounts.push_back(static_cast<std::uint64_t>((row * 17) % 997));
            payloads.push_back(make_payload(mr, payload_size, row));
            hash_index.emplace(row, static_cast<std::size_t>(row));
        }

        std::uint64_t checksum = 0;
        for (int row = 0; row < rows; row += 5) {
            const auto found = hash_index.find(row);
            if (found != hash_index.end()) {
                checksum += amounts[found->second];
                checksum += static_cast<unsigned char>(payloads[found->second][0]);
            }
        }

        benchmark::DoNotOptimize(payloads.data());
        benchmark::DoNotOptimize(checksum);
    }
}

void BM_PipelineBatchMaterialization(benchmark::State& state) {
    const auto rows = static_cast<int>(state.range(0));
    const auto payload_size = static_cast<std::size_t>(state.range(1));
    const int backend = static_cast<int>(state.range(2));

    run_with_comparison_resource(state, backend, [&](std::pmr::memory_resource* mr) {
        pipeline_batch_loop(state, mr, rows, payload_size);
    });

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(rows));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(rows) * static_cast<int64_t>(payload_size));
}

} // namespace

// Registration: for each scenario, Cartesian product of arguments × {NUMA, new_delete}.
BENCHMARK(BM_ImmediateAllocateFree)
    ->ArgNames({"size", "backend"})
    ->ArgsProduct({
        {64, 256, 1024, 4096, 8192, 65536, 1048576},
        {kNumaBackend, kNewDeleteBackend},
    });

// batch — how many consecutive allocations before the free phase (128 here).
BENCHMARK(BM_BatchAllocateFree)
    ->ArgNames({"size", "batch", "backend"})
    ->ArgsProduct({
        {64, 1024, 4096},
        {128},
        {kNumaBackend, kNewDeleteBackend},
    });

BENCHMARK(BM_MixedSmallAllocateFree)->ArgNames({"backend"})->Arg(kNumaBackend)->Arg(kNewDeleteBackend);

BENCHMARK(BM_PmrVectorPushBack)
    ->ArgNames({"count", "backend"})
    ->ArgsProduct({
        {1024, 16384},
        {kNumaBackend, kNewDeleteBackend},
    });

BENCHMARK(BM_QueryScratchContainers)
    ->ArgNames({"rows", "payload", "backend"})
    ->ArgsProduct({
        {128, 1024},
        {24, 96},
        {kUnsyncPoolOverSimple, kSyncPoolOverSimple, kArenaUnsync, kArenaSync},
    });

BENCHMARK(BM_MixedLifetimeWorkingSet)
    ->ArgNames({"live_set", "wave_allocations", "backend"})
    ->ArgsProduct({
        {64, 256},
        {64, 256},
        {kUnsyncPoolOverSimple, kSyncPoolOverSimple, kArenaUnsync, kArenaSync},
    });

BENCHMARK(BM_PipelineBatchMaterialization)
    ->ArgNames({"rows", "payload", "backend"})
    ->ArgsProduct({
        {128, 1024},
        {32, 256},
        {kUnsyncPoolOverSimple, kSyncPoolOverSimple, kArenaUnsync, kArenaSync},
    });

BENCHMARK_MAIN();
