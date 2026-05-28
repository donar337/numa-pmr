#!/usr/bin/env bash

set -Eeuo pipefail

if [[ "$(id -u)" -ne 0 ]]; then
    printf 'install_tools.sh must run as root on the verification server.\n' >&2
    exit 1
fi

LLVM_VERSION="${NUMA_VERIFY_LLVM_VERSION:-18}"

apt-get update

if apt-cache show "clang-${LLVM_VERSION}" >/dev/null 2>&1; then
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        "clang-${LLVM_VERSION}" \
        "clang-tidy-${LLVM_VERSION}" \
        "llvm-${LLVM_VERSION}" \
        "llvm-${LLVM_VERSION}-dev" \
        "llvm-${LLVM_VERSION}-tools" \
        "libclang-rt-${LLVM_VERSION}-dev" \
        cppcheck \
        ninja-build \
        cmake \
        python3 \
        numactl \
        libnuma-dev
else
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        clang \
        clang-tidy \
        llvm \
        llvm-dev \
        llvm-runtime \
        libclang-rt-dev \
        cppcheck \
        ninja-build \
        cmake \
        python3 \
        numactl \
        libnuma-dev
fi

printf 'Installed verification tools:\n'
command -v "clang-${LLVM_VERSION}" || command -v clang || true
command -v "clang++-${LLVM_VERSION}" || command -v clang++ || true
command -v "clang-tidy-${LLVM_VERSION}" || command -v clang-tidy || true
command -v "llvm-symbolizer-${LLVM_VERSION}" || command -v llvm-symbolizer || true
command -v cppcheck || true
command -v ninja || true
command -v cmake || true
command -v g++ || true
