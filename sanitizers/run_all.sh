#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/common.sh"

INSTALL_TOOLS=0
WITH_TSAN=0
WITH_MSAN=0
for arg in "$@"; do
    if [[ "${arg}" == "--install-tools" ]]; then
        INSTALL_TOOLS=1
    elif [[ "${arg}" == "--with-tsan" ]]; then
        WITH_TSAN=1
    elif [[ "${arg}" == "--with-msan" ]]; then
        WITH_MSAN=1
    else
        printf 'Unknown option: %s\n' "${arg}" >&2
        printf 'Usage: %s [--install-tools] [--with-tsan] [--with-msan]\n' "$0" >&2
        exit 2
    fi
done

export NUMA_VERIFY_RESULT_DIR="${RESULT_DIR}"

: > "${SUMMARY_FILE}"
append_summary "# numa-pmr Verification Summary"
append_summary ""
append_summary "- result_dir: ${RESULT_DIR}"
append_summary "- allocator_root: ${ALLOCATOR_ROOT}"
append_summary ""

if [[ "${INSTALL_TOOLS}" -eq 1 ]]; then
    "${SCRIPT_DIR}/install_tools.sh"
fi

OVERALL_RC=0

run_stage() {
    local stage_name="$1"
    local script_path="$2"

    append_summary ""
    append_summary "### ${stage_name}"
    if "${script_path}"; then
        append_summary "- Stage status: PASS"
    else
        local rc=$?
        append_summary "- Stage status: FAIL (exit_code=${rc})"
        OVERALL_RC=1
    fi
}

run_stage "Static analysis" "${SCRIPT_DIR}/verify_static_analysis.sh"
if [[ "${WITH_MSAN}" -eq 1 ]]; then
    export NUMA_VERIFY_ENABLE_MSAN=1
    run_stage "Standalone allocator ASan/UBSan (+ optional MSan)" "${SCRIPT_DIR}/verify_allocator_sanitizers.sh"
else
    export NUMA_VERIFY_ENABLE_MSAN=0
    run_stage "Standalone allocator ASan/UBSan" "${SCRIPT_DIR}/verify_allocator_sanitizers.sh"
fi
if [[ "${WITH_TSAN}" -eq 1 ]]; then
    run_stage "Standalone allocator TSan (optional)" "${SCRIPT_DIR}/verify_allocator_tsan.sh"
else
    append_summary ""
    append_summary "### Standalone allocator TSan (optional)"
    append_summary "- Stage status: SKIPPED (run with --with-tsan)"
fi

append_summary ""
if [[ "${OVERALL_RC}" -eq 0 ]]; then
    append_summary "Overall status: PASS"
else
    append_summary "Overall status: FAIL"
fi

printf 'Verification complete: %s\n' "${RESULT_DIR}"
printf 'Summary: %s\n' "${SUMMARY_FILE}"

exit "${OVERALL_RC}"
