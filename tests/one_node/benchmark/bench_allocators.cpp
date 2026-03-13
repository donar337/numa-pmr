#include "numa_aware_memory_resource.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <memory_resource>
#include <vector>

namespace {

// backend: 0 — NumaMemoryResource, 1 — std::pmr::new_delete_resource (см. регистрацию внизу файла).
constexpr int kNumaBackend = 0;
constexpr int kNewDeleteBackend = 1;

constexpr std::array<std::size_t, 8> kMixedSmallSizes{1, 16, 17, 64, 513, 1024, 2048, 4096};

// Один allocate + deallocate за итерацию state (измеряется внутри for (_ : state)).
void immediate_loop(benchmark::State& state, std::pmr::memory_resource* mr, std::size_t size) {
    for (auto _ : state) {
        void* ptr = mr->allocate(size, alignof(std::max_align_t));
        benchmark::DoNotOptimize(ptr);
        mr->deallocate(ptr, size, alignof(std::max_align_t));
    }
}

// Immediate allocate/deallocate на фиксированном размере; размеры включают мелкие, средние и крупные (до 1 MiB).
void BM_ImmediateAllocateFree(benchmark::State& state) {
    const auto size = static_cast<std::size_t>(state.range(0));
    const int backend = static_cast<int>(state.range(1));

    if (backend == kNumaBackend) {
        NumaMemoryResource resource;
        immediate_loop(state, &resource, size);
    } else {
        std::pmr::memory_resource* resource = std::pmr::new_delete_resource();
        immediate_loop(state, resource, size);
    }

    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(size));
}

// Полный цикл batch: все allocate, затем все deallocate.
void batch_loop(
    benchmark::State& state,
    std::pmr::memory_resource* mr,
    std::size_t size,
    std::size_t batch_size,
    std::vector<void*>& ptrs
) {
    for (auto _ : state) {
        for (void*& ptr : ptrs) {
            ptr = mr->allocate(size, alignof(std::max_align_t));
            benchmark::DoNotOptimize(ptr);
        }

        for (void* ptr : ptrs) {
            mr->deallocate(ptr, size, alignof(std::max_align_t));
        }
    }
}

// Сначала выделить batch блоков, затем освободить — чтобы увидеть эффект повторного использования slab/cache.
void BM_BatchAllocateFree(benchmark::State& state) {
    const auto size = static_cast<std::size_t>(state.range(0));
    const auto batch_size = static_cast<std::size_t>(state.range(1));
    const int backend = static_cast<int>(state.range(2));
    std::vector<void*> ptrs(batch_size);

    if (backend == kNumaBackend) {
        NumaMemoryResource resource;
        batch_loop(state, &resource, size, batch_size, ptrs);
    } else {
        std::pmr::memory_resource* resource = std::pmr::new_delete_resource();
        batch_loop(state, resource, size, batch_size, ptrs);
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(batch_size));
}

// Один размер из kMixedSmallSizes за итерацию state.
void mixed_small_loop(benchmark::State& state, std::pmr::memory_resource* mr) {
    std::size_t index = 0;

    for (auto _ : state) {
        const std::size_t size = kMixedSmallSizes[index++ % kMixedSmallSizes.size()];
        void* ptr = mr->allocate(size, alignof(std::max_align_t));
        benchmark::DoNotOptimize(ptr);
        mr->deallocate(ptr, size, alignof(std::max_align_t));
    }
}

// Чередование небольших размеров выделения (типичный «шумный» паттерн для small-object путей).
void BM_MixedSmallAllocateFree(benchmark::State& state) {
    const int backend = static_cast<int>(state.range(0));

    if (backend == kNumaBackend) {
        NumaMemoryResource resource;
        mixed_small_loop(state, &resource);
    } else {
        std::pmr::memory_resource* resource = std::pmr::new_delete_resource();
        mixed_small_loop(state, resource);
    }

    state.SetItemsProcessed(state.iterations());
}

// count раз push_back в pmr::vector на заданном memory_resource.
void pmr_vector_push_back_loop(benchmark::State& state, std::pmr::memory_resource* mr, int count) {
    for (auto _ : state) {
        std::pmr::vector<int> values(mr);
        for (int i = 0; i < count; ++i) {
            values.push_back(i);
        }
        benchmark::DoNotOptimize(values.data());
    }
}

// Рост std::pmr::vector<int> через push_back (реалистичная нагрузка на PMR + перевыделения буфера).
void BM_PmrVectorPushBack(benchmark::State& state) {
    const auto count = static_cast<int>(state.range(0));
    const int backend = static_cast<int>(state.range(1));

    if (backend == kNumaBackend) {
        NumaMemoryResource resource;
        pmr_vector_push_back_loop(state, &resource, count);
    } else {
        std::pmr::memory_resource* resource = std::pmr::new_delete_resource();
        pmr_vector_push_back_loop(state, resource, count);
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(count));
}

} // namespace

// Регистрация: для каждого сценария декартово произведение аргументов × {NUMA, new_delete}.
BENCHMARK(BM_ImmediateAllocateFree)
    ->ArgNames({"size", "backend"})
    ->ArgsProduct({
        {64, 256, 1024, 4096, 8192, 65536, 1048576},
        {kNumaBackend, kNewDeleteBackend},
    });

// batch — сколько выделений подряд до фазы освобождения (здесь 128).
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
