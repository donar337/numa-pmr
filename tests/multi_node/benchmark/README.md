# Multialloc Benchmark

Standalone CLI benchmark for mixed-size PMR allocation workloads on a NUMA
machine. It compares `std::pmr::memory_resource` backends by throughput and,
when requested, by virtual-memory allocation overhead traced at syscall level.

## Build

```bash
cmake -S . -B build-bench -G Ninja \
  -DNUMA_ALLOCATOR_BUILD_TESTS=ON \
  -DNUMA_ALLOCATOR_BUILD_BENCHMARKS=ON \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build-bench --target multi_node_bench_multialloc
```

## Run Modes

Performance mode keeps syscall tracing disabled. Use this mode for throughput:

```bash
./build-bench/tests/multi_node/multi_node_bench_multialloc \
  --threads 4 \
  --allocations-per-thread 10000 \
  --waves 1 \
  --backend all \
  --pin-threads true \
  --progress true \
  --size-profile mixed \
  --vm-trace false \
  --json -
```

Memory-overhead mode enables syscall tracing for `mmap`, `munmap`, `mremap`,
and `brk` during all allocation/free waves. Throughput from this mode is
diagnostic only because `ptrace` adds overhead:

```bash
./build-bench/tests/multi_node/multi_node_bench_multialloc \
  --threads 4 \
  --allocations-per-thread 10000 \
  --waves 3 \
  --backend all \
  --pin-threads true \
  --progress true \
  --size-profile mixed \
  --vm-trace true \
  --json data/vm_trace_overhead.json
```

## Options

- `--threads N`: number of worker threads.
- `--allocations-per-thread N`: number of allocations performed by each thread.
- `--waves N`: number of allocate/free waves per worker; default is `1`.
- `--backend all|numa|new_delete|sync_pool_new_delete`: backend to run.
- `--pin-threads true|false`: wrap each worker in `numa_thread_pin_guard`; NUMA nodes are selected round-robin.
- `--progress true|false`: print backend start/finish and per-wave completion to stderr; default is `true`.
- `--size-profile mixed|small|large|custom`: allocation size profile.
- `--sizes 64,256,4096`: custom allocation sizes; this also selects `custom`.
- `--vm-trace true|false`: trace virtual-memory syscalls during all allocation/free waves.
- `--json -|PATH`: write JSON to stdout or to a file. Parent directories are created automatically.

## Output JSON

Top-level output contains one result record per backend:

```json
{
  "results": [
    {
      "backend": "numa",
      "success": true,
      "parameters": {
        "threads": 4,
        "allocations_per_thread": 10000,
        "waves": 3,
        "pin_threads": true,
        "progress": true,
        "vm_trace": true,
        "size_profile": "mixed"
      },
      "throughput": {
        "duration_ns": 123456789,
        "allocation_count": 40000,
        "pmr_operation_count": 80000,
        "operations_per_second": 648000.0,
        "bytes_per_second": 2800000000.0
      },
      "memory": {
        "logical_current_bytes": 0,
        "logical_total_allocated_bytes": 104857600,
        "allocation_count": 40000,
        "memory_overhead_ratio": 1.7168,
        "vm_trace_required_for_overhead": true
      },
      "vm_trace": {
        "enabled": true,
        "complete": true,
        "trace_start_count": 1,
        "trace_stop_count": 1,
        "mmap_count": 512,
        "munmap_count": 0,
        "mremap_count": 0,
        "brk_count": 0,
        "total_mapped_bytes": 180020000,
        "total_unmapped_bytes": 0,
        "current_mapped_bytes": 180020000,
        "total_brk_growth_bytes": 0,
        "total_brk_shrink_bytes": 0,
        "current_brk_growth_bytes": 0,
        "total_virtual_allocated_bytes": 180020000,
        "current_virtual_bytes": 180020000
      }
    }
  ]
}
```

When `--vm-trace false`, the trace-derived ratios are `null` and the `vm_trace`
block is omitted.

## Field Meaning

- `backend`: backend name.
- `success`: whether the child process completed successfully.
- `error`: error message when `success` is `false`.
- `parameters`: effective benchmark parameters.

`throughput`:

- `duration_ns`: allocation phase plus free phase duration.
- `allocation_count`: number of allocation calls.
- `pmr_operation_count`: allocation plus deallocation calls.
- `operations_per_second`: PMR operations per second.
- `bytes_per_second`: logical allocated bytes per second.

`memory`:

- `logical_current_bytes`: live user-requested bytes after the benchmark; normally `0`.
- `logical_total_allocated_bytes`: total user-requested bytes over all waves.
- `allocation_count`: number of allocation calls observed by the PMR tracking wrapper.
- `memory_overhead_ratio`: `vm_trace.total_mapped_bytes / logical_total_allocated_bytes`; `null` when tracing is disabled or incomplete. With multiple waves, both numerator and denominator are cumulative across all waves, so lower values indicate more reuse.
- `vm_trace_required_for_overhead`: always `true`; memory-overhead ratios are intentionally trace-based.

`vm_trace`:

- `enabled`: whether syscall tracing was requested.
- `complete`: whether tracing finished without tracer errors.
- `trace_start_count`, `trace_stop_count`: traced workload-window marker counts.
- `mmap_count`, `munmap_count`, `mremap_count`, `brk_count`: traced syscall counts.
- `total_mapped_bytes`: bytes requested through successful `mmap` calls during traced waves.
- `total_unmapped_bytes`: bytes released through successful `munmap` calls during traced waves.
- `current_mapped_bytes`: `total_mapped_bytes - total_unmapped_bytes`, clamped at zero.
- `total_brk_growth_bytes`, `total_brk_shrink_bytes`: traced heap growth/shrink through `brk`.
- `total_virtual_allocated_bytes`: `total_mapped_bytes + total_brk_growth_bytes`.
- `current_virtual_bytes`: `current_mapped_bytes + current_brk_growth_bytes`.
