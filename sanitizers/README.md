# numa-pmr Verification

This directory contains scripts for allocator-only verification of `numa-pmr`.
They use local module paths only:

```text
src/
tests/
build/
verification_results/
```

## Run

Run from the allocator root:

```bash
cd <allocator-root>
```

Install required tools once:

```bash
scripts/verification/install_tools.sh
```

Run the recommended verification matrix:

```bash
scripts/verification/run_all.sh --with-tsan
```

Run in background:

```bash
mkdir -p verification_results
nohup scripts/verification/run_all.sh --with-tsan \
  > verification_results/latest_run.stdout 2>&1 &
tail -n 200 verification_results/latest_run.stdout
```

Run stages separately:

```bash
scripts/verification/verify_static_analysis.sh
scripts/verification/verify_allocator_sanitizers.sh
scripts/verification/verify_allocator_tsan.sh
```

Optional MSan run:

```bash
scripts/verification/run_all.sh --with-tsan --with-msan
```

## Results

Each run creates:

```text
verification_results/YYYYMMDD_HHMMSS/
  environment.txt
  summary.txt
  logs/
```

Use `summary.txt` first. It contains the stage verdicts:

```text
cat verification_results/<timestamp>/summary.txt
```

Use `environment.txt` to check the host, compiler versions, tool versions, and NUMA topology used for the run.

Use `logs/` for detailed evidence:

```text
logs/clang_tidy.log
logs/cppcheck.log
logs/allocator_asan_ubsan_ctest.log
logs/allocator_tsan_ctest.log
```

## How To Read The Result

The run is successful when:

- `summary.txt` contains `Overall status: PASS`;
- static analysis stages are `PASS`;
- `allocator_asan_ubsan_ctest.log` contains `100% tests passed`;
- if `--with-tsan` was used, `allocator_tsan_ctest.log` contains `100% tests passed`;
- logs do not contain infrastructure failures such as `No such file`, `command not found`, `syntax error`, or `CMake Error`.

Static analysis warnings do not automatically mean runtime failure. Treat them as findings to triage from
`clang_tidy.log` and `cppcheck.log`.

Sanitizer findings are stronger signals. `AddressSanitizer`, `UndefinedBehaviorSanitizer`, `LeakSanitizer`, or
`ThreadSanitizer` reports in the `ctest` logs should be treated as verification failures for the checked test surface.

## What Is Tested

Static analysis is run over `src`.

Sanitizers run the full test suite from `tests`, including:

- block metadata and layout;
- virtual memory and NUMA binding;
- small and large allocation paths;
- arena and arena manager behavior;
- thread-local cache behavior;
- cross-thread free and foreign-free draining;
- PMR resources.

These tests are synthetic and allocator-focused by design. They verify the allocator module in isolation and do not
claim that every full application runtime path has been checked.

## Useful Overrides

Override allocator root when running scripts from another directory:

```bash
NUMA_VERIFY_ALLOCATOR_ROOT=/path/to/numa-pmr scripts/verification/run_all.sh --with-tsan
```

Override result directory:

```bash
NUMA_VERIFY_RESULT_DIR=/tmp/numa-pmr-verify scripts/verification/run_all.sh --with-tsan
```

Override job count:

```bash
NUMA_VERIFY_JOBS=8 scripts/verification/run_all.sh --with-tsan
```
