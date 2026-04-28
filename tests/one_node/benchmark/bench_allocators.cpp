#include "numa_memory_resource.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <memory_resource>
#include <vector>

namespace {

// backend: 0 — numa_memory_resource, 1 — std::pmr::new_delete_resource (see registration at bottom of file).
constexpr int kNumaBackend = 0;
constexpr int kNewDeleteBackend = 1;

constexpr std::array<std::size_t, 8> kMixedSmallSizes{1, 16, 17, 64, 513, 1024, 2048, 4096};

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

BENCHMARK_MAIN();
