#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/common.sh"

assert_allocator_root
resolve_common_tools
resolve_clang_tools
write_environment_snapshot

append_summary '## Standalone Allocator TSan'
STAGE_RC=0

TSAN_BUILD_DIR="${ALLOCATOR_ROOT}/build/verification-tsan"
TSAN_FLAGS='-O1 -g -fno-omit-frame-pointer -fsanitize=thread'

set +e
run_logged allocator_tsan_configure \
    "${CMAKE_BIN}" -S "${ALLOCATOR_ROOT}" -B "${TSAN_BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_COMPILER="${CLANGXX_BIN}" \
    -DCMAKE_CXX_FLAGS="${TSAN_FLAGS}" \
    -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=thread' \
    -DNUMA_ALLOCATOR_BUILD_TESTS=ON \
    -DNUMA_ALLOCATOR_BUILD_BENCHMARKS=OFF
TSAN_CONFIGURE_RC=$?

if [[ "${TSAN_CONFIGURE_RC}" -eq 0 ]]; then
    run_logged allocator_tsan_build \
        "${CMAKE_BIN}" --build "${TSAN_BUILD_DIR}" -j "${JOBS}"
    TSAN_BUILD_RC=$?
else
    TSAN_BUILD_RC=1
fi

if [[ "${TSAN_CONFIGURE_RC}" -eq 0 && "${TSAN_BUILD_RC}" -eq 0 ]]; then
    run_shell_logged allocator_tsan_ctest \
        "TSAN_OPTIONS='halt_on_error=1:history_size=7:second_deadlock_stack=1' '${CTEST_BIN}' --test-dir '${TSAN_BUILD_DIR}' --output-on-failure"
    TSAN_CTEST_RC=$?
else
    TSAN_CTEST_RC=1
fi
set -e

append_summary "- Standalone TSan configure: $( [[ "${TSAN_CONFIGURE_RC}" -eq 0 ]] && printf 'PASS' || printf 'FAIL' )"
append_summary "- Standalone TSan build: $( [[ "${TSAN_BUILD_RC}" -eq 0 ]] && printf 'PASS' || printf 'FAIL' )"
append_summary "- Standalone TSan tests: $( [[ "${TSAN_CTEST_RC}" -eq 0 ]] && printf 'PASS' || printf 'FAIL' )"
append_summary "  - configure log: ${LOG_DIR}/allocator_tsan_configure.log"
append_summary "  - build log: ${LOG_DIR}/allocator_tsan_build.log"
append_summary "  - ctest log: ${LOG_DIR}/allocator_tsan_ctest.log"

if [[ "${TSAN_CONFIGURE_RC}" -ne 0 || "${TSAN_BUILD_RC}" -ne 0 || "${TSAN_CTEST_RC}" -ne 0 ]]; then
    STAGE_RC=1
fi

exit "${STAGE_RC}"
