#include "numa_aware_memory_resource.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <memory_resource>
#include <vector>

namespace {

constexpr std::array<std::size_t, 8> kMixedSmallSizes{1, 16, 17, 64, 513, 1024, 2048, 4096};

// Измеряет immediate allocate/free для NUMA resource на фиксированных размерах.
void BM_NumaImmediateAllocateFree(benchmark::State& state) {
    NumaMemoryResource resource;
    const auto size = static_cast<std::size_t>(state.range(0));

    for (auto _ : state) {
        void* ptr = resource.allocate(size, alignof(std::max_align_t));
        benchmark::DoNotOptimize(ptr);
        resource.deallocate(ptr, size, alignof(std::max_align_t));
    }

    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(size));
}

// Измеряет baseline immediate allocate/free через std::pmr::new_delete_resource на тех же размерах.
void _BM_NewDeleteImmediateAllocateFree(benchmark::State& state) {
    std::pmr::memory_resource* resource = std::pmr::new_delete_resource();
    const auto size = static_cast<std::size_t>(state.range(0));

    for (auto _ : state) {
        void* ptr = resource->allocate(size, alignof(std::max_align_t));
        benchmark::DoNotOptimize(ptr);
        resource->deallocate(ptr, size, alignof(std::max_align_t));
    }

    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(size));
}

// Измеряет batch allocate-then-free для NUMA resource, чтобы увидеть эффект slab reuse.
void BM_NumaBatchAllocateFree(benchmark::State& state) {
    NumaMemoryResource resource;
    const auto size = static_cast<std::size_t>(state.range(0));
    const auto batch_size = static_cast<std::size_t>(state.range(1));
    std::vector<void*> ptrs(batch_size);

    for (auto _ : state) {
        for (void*& ptr : ptrs) {
            ptr = resource.allocate(size, alignof(std::max_align_t));
            benchmark::DoNotOptimize(ptr);
        }

        for (void* ptr : ptrs) {
            resource.deallocate(ptr, size, alignof(std::max_align_t));
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(batch_size));
}

// Измеряет baseline batch allocate-then-free через std::pmr::new_delete_resource.
void _BM_NewDeleteBatchAllocateFree(benchmark::State& state) {
    std::pmr::memory_resource* resource = std::pmr::new_delete_resource();
    const auto size = static_cast<std::size_t>(state.range(0));
    const auto batch_size = static_cast<std::size_t>(state.range(1));
    std::vector<void*> ptrs(batch_size);

    for (auto _ : state) {
        for (void*& ptr : ptrs) {
            ptr = resource->allocate(size, alignof(std::max_align_t));
            benchmark::DoNotOptimize(ptr);
        }

        for (void* ptr : ptrs) {
            resource->deallocate(ptr, size, alignof(std::max_align_t));
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(batch_size));
}

// Измеряет смешанные small allocation sizes для NUMA resource.
void BM_NumaMixedSmallAllocateFree(benchmark::State& state) {
    NumaMemoryResource resource;
    std::size_t index = 0;

    for (auto _ : state) {
        const std::size_t size = kMixedSmallSizes[index++ % kMixedSmallSizes.size()];
        void* ptr = resource.allocate(size, alignof(std::max_align_t));
        benchmark::DoNotOptimize(ptr);
        resource.deallocate(ptr, size, alignof(std::max_align_t));
    }

    state.SetItemsProcessed(state.iterations());
}

// Измеряет baseline смешанных small allocation sizes через std::pmr::new_delete_resource.
void _BM_NewDeleteMixedSmallAllocateFree(benchmark::State& state) {
    std::pmr::memory_resource* resource = std::pmr::new_delete_resource();
    std::size_t index = 0;

    for (auto _ : state) {
        const std::size_t size = kMixedSmallSizes[index++ % kMixedSmallSizes.size()];
        void* ptr = resource->allocate(size, alignof(std::max_align_t));
        benchmark::DoNotOptimize(ptr);
        resource->deallocate(ptr, size, alignof(std::max_align_t));
    }

    state.SetItemsProcessed(state.iterations());
}

// Измеряет рост std::pmr::vector<int> поверх NUMA resource.
void BM_NumaPmrVectorPushBack(benchmark::State& state) {
    NumaMemoryResource resource;
    const auto count = static_cast<int>(state.range(0));

    for (auto _ : state) {
        std::pmr::vector<int> values(&resource);
        for (int i = 0; i < count; ++i) {
            values.push_back(i);
        }
        benchmark::DoNotOptimize(values.data());
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(count));
}

// Измеряет baseline роста std::pmr::vector<int> через std::pmr::new_delete_resource.
void _BM_NewDeletePmrVectorPushBack(benchmark::State& state) {
    std::pmr::memory_resource* resource = std::pmr::new_delete_resource();
    const auto count = static_cast<int>(state.range(0));

    for (auto _ : state) {
        std::pmr::vector<int> values(resource);
        for (int i = 0; i < count; ++i) {
            values.push_back(i);
        }
        benchmark::DoNotOptimize(values.data());
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(count));
}

} // namespace

BENCHMARK(BM_NumaImmediateAllocateFree)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096)->Arg(8192)->Arg(65536);
BENCHMARK(_BM_NewDeleteImmediateAllocateFree)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096)->Arg(8192)->Arg(65536);

BENCHMARK(BM_NumaBatchAllocateFree)->Args({64, 128})->Args({1024, 128})->Args({4096, 128});
BENCHMARK(_BM_NewDeleteBatchAllocateFree)->Args({64, 128})->Args({1024, 128})->Args({4096, 128});

BENCHMARK(BM_NumaMixedSmallAllocateFree);
BENCHMARK(_BM_NewDeleteMixedSmallAllocateFree);

BENCHMARK(BM_NumaImmediateAllocateFree)->Name("BM_NumaLargeAllocateFree")->Arg(8192)->Arg(65536)->Arg(1024 * 1024);
BENCHMARK(_BM_NewDeleteImmediateAllocateFree)->Name("_BM_NewDeleteLargeAllocateFree")->Arg(8192)->Arg(65536)->Arg(1024 * 1024);

BENCHMARK(BM_NumaPmrVectorPushBack)->Arg(1024)->Arg(16384);
BENCHMARK(_BM_NewDeletePmrVectorPushBack)->Arg(1024)->Arg(16384);

BENCHMARK_MAIN();
