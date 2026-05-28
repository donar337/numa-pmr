#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/common.sh"

assert_allocator_root
resolve_common_tools
write_environment_snapshot

append_summary '## Standalone Allocator Sanitizers'
STAGE_RC=0

ASAN_UBSAN_BUILD_DIR="${ALLOCATOR_ROOT}/build/verification-asan-ubsan"
ASAN_UBSAN_FLAGS='-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined'

set +e
run_logged allocator_asan_ubsan_configure \
    "${CMAKE_BIN}" -S "${ALLOCATOR_ROOT}" -B "${ASAN_UBSAN_BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_COMPILER="${GXX_BIN}" \
    -DCMAKE_CXX_FLAGS="${ASAN_UBSAN_FLAGS}" \
    -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined' \
    -DNUMA_ALLOCATOR_BUILD_TESTS=ON \
    -DNUMA_ALLOCATOR_BUILD_BENCHMARKS=OFF
ASAN_UBSAN_CONFIGURE_RC=$?

if [[ "${ASAN_UBSAN_CONFIGURE_RC}" -eq 0 ]]; then
    run_logged allocator_asan_ubsan_build \
        "${CMAKE_BIN}" --build "${ASAN_UBSAN_BUILD_DIR}" -j "${JOBS}"
    ASAN_UBSAN_BUILD_RC=$?
else
    ASAN_UBSAN_BUILD_RC=1
fi

if [[ "${ASAN_UBSAN_CONFIGURE_RC}" -eq 0 && "${ASAN_UBSAN_BUILD_RC}" -eq 0 ]]; then
    run_shell_logged allocator_asan_ubsan_ctest \
        "ASAN_OPTIONS='detect_leaks=1:halt_on_error=1:strict_string_checks=1' UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1' '${CTEST_BIN}' --test-dir '${ASAN_UBSAN_BUILD_DIR}' --output-on-failure"
    ASAN_UBSAN_CTEST_RC=$?
else
    ASAN_UBSAN_CTEST_RC=1
fi

append_summary "- Standalone ASan/UBSan configure: $( [[ "${ASAN_UBSAN_CONFIGURE_RC}" -eq 0 ]] && printf 'PASS' || printf 'FAIL' )"
append_summary "- Standalone ASan/UBSan build: $( [[ "${ASAN_UBSAN_BUILD_RC}" -eq 0 ]] && printf 'PASS' || printf 'FAIL' )"
append_summary "- Standalone ASan/UBSan tests: $( [[ "${ASAN_UBSAN_CTEST_RC}" -eq 0 ]] && printf 'PASS' || printf 'FAIL' )"
append_summary "  - configure log: ${LOG_DIR}/allocator_asan_ubsan_configure.log"
append_summary "  - build log: ${LOG_DIR}/allocator_asan_ubsan_build.log"
append_summary "  - ctest log: ${LOG_DIR}/allocator_asan_ubsan_ctest.log"

if [[ "${ASAN_UBSAN_CONFIGURE_RC}" -ne 0 || "${ASAN_UBSAN_BUILD_RC}" -ne 0 || "${ASAN_UBSAN_CTEST_RC}" -ne 0 ]]; then
    STAGE_RC=1
fi

if [[ "${NUMA_VERIFY_ENABLE_MSAN:-0}" != "1" ]]; then
    append_summary '- Standalone MSan tests: SKIPPED by NUMA_VERIFY_ENABLE_MSAN'
    set -e
    exit "${STAGE_RC}"
fi

resolve_clang_tools
CLANG_RESOLVE_RC=$?

if [[ "${CLANG_RESOLVE_RC}" -ne 0 ]]; then
    append_summary '- Standalone MSan tests: NOT APPLICABLE'
    append_summary '  - clang/clang++ was not available.'
    set -e
    exit "${STAGE_RC}"
fi

MSAN_BUILD_DIR="${ALLOCATOR_ROOT}/build/verification-msan"
MSAN_FLAGS='-O1 -g -fno-omit-frame-pointer -fsanitize=memory -fsanitize-memory-track-origins=2'

set +e
run_logged allocator_msan_configure \
    "${CMAKE_BIN}" -S "${ALLOCATOR_ROOT}" -B "${MSAN_BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_COMPILER="${CLANGXX_BIN}" \
    -DCMAKE_CXX_FLAGS="${MSAN_FLAGS}" \
    -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=memory' \
    -DNUMA_ALLOCATOR_BUILD_TESTS=ON \
    -DNUMA_ALLOCATOR_BUILD_BENCHMARKS=OFF
MSAN_CONFIGURE_RC=$?

if [[ "${MSAN_CONFIGURE_RC}" -eq 0 ]]; then
    run_logged allocator_msan_build \
        "${CMAKE_BIN}" --build "${MSAN_BUILD_DIR}" -j "${JOBS}"
    MSAN_BUILD_RC=$?
else
    MSAN_BUILD_RC=1
fi

if [[ "${MSAN_CONFIGURE_RC}" -eq 0 && "${MSAN_BUILD_RC}" -eq 0 ]]; then
    run_shell_logged allocator_msan_ctest \
        "MSAN_OPTIONS='halt_on_error=1:print_stats=1$(sanitizer_symbolizer_option)' '${CTEST_BIN}' --test-dir '${MSAN_BUILD_DIR}' --output-on-failure"
    MSAN_CTEST_RC=$?
else
    MSAN_CTEST_RC=1
fi
set -e

if [[ "${MSAN_CONFIGURE_RC}" -eq 0 && "${MSAN_BUILD_RC}" -eq 0 && "${MSAN_CTEST_RC}" -eq 0 ]]; then
    append_summary '- Standalone MSan tests: PASS'
    append_summary "  - ctest log: ${LOG_DIR}/allocator_msan_ctest.log"
else
    STAGE_RC=1
    append_summary '- Standalone MSan tests: NOT APPLICABLE or FAILED NON-CRITICALLY'
    append_summary '  - MSan often requires fully instrumented dependencies; inspect allocator_msan_*.log.'
fi

set -e
exit "${STAGE_RC}"
