Сборка и запуск тестов:

```bash
cmake -S . -B build -G Ninja \
  -DNUMA_ALLOCATOR_BUILD_TESTS=ON \
  -DNUMA_ALLOCATOR_BUILD_BENCHMARKS=OFF

cmake --build build
ctest --test-dir build --output-on-failure
```

Сборка и запуск coverage:

```bash
cmake -S . -B build-coverage -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DNUMA_ALLOCATOR_BUILD_TESTS=ON \
  -DNUMA_ALLOCATOR_ENABLE_COVERAGE=ON

cmake --build build-coverage
cmake --build build-coverage --target coverage_one_node_unit
cmake --build build-coverage --target coverage_multi_node_unit

# отчёты:
# - build-coverage/coverage_one_node_unit.html
# - build-coverage/coverage_multi_node_unit.html
```

Для coverage нужен `gcovr`. Targets перед запуском удаляют старые counters и
игнорируют известный баг `gcov` с отрицательными branch counters через
`--gcov-ignore-parse-errors negative_hits.warn_once_per_file`. Multi-node unit
coverage также требует машину с двумя доступными NUMA nodes.

Сборка и запуск бенчей:

```bash
cmake -S . -B build-bench -G Ninja \
  -DNUMA_ALLOCATOR_BUILD_TESTS=ON \
  -DNUMA_ALLOCATOR_BUILD_BENCHMARKS=ON \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build-bench
./build-bench/tests/one_node/one_node_bench_allocators

cmake --build build-bench-release --target one_node_bench_allocators
./build-bench-release/tests/one_node/one_node_bench_allocators --benchmark_format=json
```