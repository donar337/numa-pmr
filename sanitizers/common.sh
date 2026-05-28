#!/usr/bin/env bash

set -Eeuo pipefail

default_jobs() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    elif command -v sysctl >/dev/null 2>&1; then
        sysctl -n hw.ncpu
    else
        printf '4\n'
    fi
}

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ALLOCATOR_ROOT="${NUMA_VERIFY_ALLOCATOR_ROOT:-$(cd -- "${SCRIPT_DIR}/../.." && pwd)}"
RESULTS_ROOT="${NUMA_VERIFY_RESULTS_ROOT:-${ALLOCATOR_ROOT}/verification_results}"
TIMESTAMP="${NUMA_VERIFY_TIMESTAMP:-$(date +%Y%m%d_%H%M%S)}"
RESULT_DIR="${NUMA_VERIFY_RESULT_DIR:-${RESULTS_ROOT}/${TIMESTAMP}}"
LOG_DIR="${RESULT_DIR}/logs"
SUMMARY_FILE="${RESULT_DIR}/summary.txt"
JOBS="${NUMA_VERIFY_JOBS:-$(default_jobs)}"
LLVM_VERSION="${NUMA_VERIFY_LLVM_VERSION:-18}"

mkdir -p "${LOG_DIR}"

log_info() {
    printf '[numa-verify] %s\n' "$*"
}

append_summary() {
    mkdir -p "$(dirname -- "${SUMMARY_FILE}")"
    printf '%s\n' "$*" >> "${SUMMARY_FILE}"
}

find_first_tool() {
    local candidate
    for candidate in "$@"; do
        if command -v "${candidate}" >/dev/null 2>&1; then
            command -v "${candidate}"
            return 0
        fi
    done
    return 1
}

require_resolved_tool() {
    local variable_name="$1"
    local description="$2"
    if [[ -z "${!variable_name:-}" ]]; then
        printf 'Required tool not found: %s\n' "${description}" >&2
        printf 'Run scripts/verification/install_tools.sh on the server first.\n' >&2
        return 1
    fi
}

resolve_common_tools() {
    CMAKE_BIN="$(find_first_tool cmake)" || CMAKE_BIN=""
    NINJA_BIN="$(find_first_tool ninja ninja-build)" || NINJA_BIN=""
    CTEST_BIN="$(find_first_tool ctest)" || CTEST_BIN=""
    GXX_BIN="$(find_first_tool g++)" || GXX_BIN=""

    require_resolved_tool CMAKE_BIN cmake
    require_resolved_tool NINJA_BIN ninja
    require_resolved_tool CTEST_BIN ctest
    require_resolved_tool GXX_BIN g++

    export CMAKE_BIN NINJA_BIN CTEST_BIN GXX_BIN
}

resolve_clang_tools() {
    CLANG_BIN="$(find_first_tool "clang-${LLVM_VERSION}" clang)" || CLANG_BIN=""
    CLANGXX_BIN="$(find_first_tool "clang++-${LLVM_VERSION}" clang++)" || CLANGXX_BIN=""
    CLANG_TIDY_BIN="$(find_first_tool "clang-tidy-${LLVM_VERSION}" clang-tidy)" || CLANG_TIDY_BIN=""
    LLVM_SYMBOLIZER_BIN="$(find_first_tool "llvm-symbolizer-${LLVM_VERSION}" llvm-symbolizer)" || LLVM_SYMBOLIZER_BIN=""

    require_resolved_tool CLANG_BIN clang
    require_resolved_tool CLANGXX_BIN clang++
    require_resolved_tool CLANG_TIDY_BIN clang-tidy

    export CLANG_BIN CLANGXX_BIN CLANG_TIDY_BIN LLVM_SYMBOLIZER_BIN
}

resolve_cppcheck() {
    CPPCHECK_BIN="$(find_first_tool cppcheck)" || CPPCHECK_BIN=""
    require_resolved_tool CPPCHECK_BIN cppcheck
    export CPPCHECK_BIN
}

assert_allocator_root() {
    if [[ ! -f "${ALLOCATOR_ROOT}/CMakeLists.txt" || ! -d "${ALLOCATOR_ROOT}/src" || ! -d "${ALLOCATOR_ROOT}/tests" ]]; then
        printf 'ALLOCATOR_ROOT does not look like numa-pmr: %s\n' "${ALLOCATOR_ROOT}" >&2
        return 1
    fi
}

run_logged() {
    local log_name="$1"
    shift
    local log_path="${LOG_DIR}/${log_name}.log"

    log_info "running ${log_name}; log=${log_path}"
    {
        printf '$'
        printf ' %q' "$@"
        printf '\n'
        "$@"
    } > "${log_path}" 2>&1
}

run_shell_logged() {
    local log_name="$1"
    local command_text="$2"
    local log_path="${LOG_DIR}/${log_name}.log"

    log_info "running ${log_name}; log=${log_path}"
    {
        printf '$ %s\n' "${command_text}"
        bash -lc "${command_text}"
    } > "${log_path}" 2>&1
}

sanitizer_symbolizer_option() {
    if [[ -n "${LLVM_SYMBOLIZER_BIN:-}" ]]; then
        printf ':external_symbolizer_path=%s' "${LLVM_SYMBOLIZER_BIN}"
    fi
}

write_environment_snapshot() {
    local output_path="${RESULT_DIR}/environment.txt"

    {
        printf 'timestamp=%s\n' "${TIMESTAMP}"
        printf 'allocator_root=%s\n' "${ALLOCATOR_ROOT}"
        printf 'result_dir=%s\n' "${RESULT_DIR}"
        printf 'hostname='
        hostname || true
        printf 'kernel='
        uname -a || true
        printf '\nnuma_hardware=\n'
        numactl --hardware || true
        printf '\ncpu_topology=\n'
        lscpu || true
        printf '\ntools=\n'
        "${CMAKE_BIN:-cmake}" --version | head -n 1 || true
        "${NINJA_BIN:-ninja}" --version || true
        "${CTEST_BIN:-ctest}" --version | head -n 1 || true
        "${GXX_BIN:-g++}" --version | head -n 1 || true
        "${CLANG_BIN:-clang}" --version | head -n 1 || true
        "${CLANGXX_BIN:-clang++}" --version | head -n 1 || true
        "${CLANG_TIDY_BIN:-clang-tidy}" --version | head -n 1 || true
        "${CPPCHECK_BIN:-cppcheck}" --version || true
    } > "${output_path}" 2>&1
}
