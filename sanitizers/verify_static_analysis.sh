#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/common.sh"

assert_allocator_root
resolve_common_tools
resolve_clang_tools
resolve_cppcheck
write_environment_snapshot

BUILD_DIR="${ALLOCATOR_ROOT}/build/verification-static-analysis"
STAGE_RC=0

append_summary '## Static Analysis'

set +e
run_logged static_configure \
    "${CMAKE_BIN}" -S "${ALLOCATOR_ROOT}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_COMPILER="${CLANGXX_BIN}" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DNUMA_ALLOCATOR_BUILD_TESTS=OFF \
    -DNUMA_ALLOCATOR_BUILD_BENCHMARKS=OFF
STATIC_CONFIGURE_RC=$?

if [[ "${STATIC_CONFIGURE_RC}" -eq 0 ]]; then
    run_logged static_build \
        "${CMAKE_BIN}" --build "${BUILD_DIR}" --target numa_allocator -j "${JOBS}"
    STATIC_BUILD_RC=$?
else
    STATIC_BUILD_RC=1
fi

mapfile -t ALLOCATOR_CPP_FILES < <(
    find "${ALLOCATOR_ROOT}/src" -type f -name '*.cpp' | sort
)

if [[ "${#ALLOCATOR_CPP_FILES[@]}" -eq 0 ]]; then
    printf 'No allocator source files found.\n' >&2
    exit 1
fi

CLANG_TIDY_CHECKS="${NUMA_VERIFY_CLANG_TIDY_CHECKS:-clang-analyzer-*,bugprone-*,performance-*,portability-*}"
HEADER_FILTER="${ALLOCATOR_ROOT}/src/.*"

if [[ "${STATIC_CONFIGURE_RC}" -eq 0 ]]; then
    run_logged clang_tidy \
        "${CLANG_TIDY_BIN}" \
        -p "${BUILD_DIR}" \
        "--checks=${CLANG_TIDY_CHECKS}" \
        "--header-filter=${HEADER_FILTER}" \
        "${ALLOCATOR_CPP_FILES[@]}"
    CLANG_TIDY_RC=$?
else
    CLANG_TIDY_RC=1
fi

run_logged cppcheck \
    "${CPPCHECK_BIN}" \
    --enable=warning,performance,portability \
    --std=c++20 \
    --language=c++ \
    --inline-suppr \
    --suppress=missingIncludeSystem \
    -I "${ALLOCATOR_ROOT}/src" \
    "${ALLOCATOR_ROOT}/src"
CPPCHECK_RC=$?
set -e

append_summary "- cmake configure: $( [[ "${STATIC_CONFIGURE_RC}" -eq 0 ]] && printf 'PASS' || printf 'FAIL' )"
append_summary "- cmake build: $( [[ "${STATIC_BUILD_RC}" -eq 0 ]] && printf 'PASS' || printf 'FAIL' )"
append_summary "- clang-tidy: $( [[ "${CLANG_TIDY_RC}" -eq 0 ]] && printf 'PASS' || printf 'FAIL' )"
append_summary "  - log: ${LOG_DIR}/clang_tidy.log"
append_summary "- cppcheck: $( [[ "${CPPCHECK_RC}" -eq 0 ]] && printf 'PASS' || printf 'FAIL' )"
append_summary "  - log: ${LOG_DIR}/cppcheck.log"

if [[ "${STATIC_CONFIGURE_RC}" -ne 0 || "${STATIC_BUILD_RC}" -ne 0 || "${CLANG_TIDY_RC}" -ne 0 || "${CPPCHECK_RC}" -ne 0 ]]; then
    STAGE_RC=1
fi

exit "${STAGE_RC}"
