#!/usr/bin/env bash
# run_sanitizers.sh
#
# Builds the project with AddressSanitizer + UndefinedBehaviorSanitizer,
# runs the unit test suite and the demo driver under it, and (if valgrind
# is installed) also runs the unit tests under Valgrind's memcheck as a
# second, independent check.
#
# Usage:
#   scripts/run_sanitizers.sh
#
# Exits non-zero if any step fails, so it's suitable for CI.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-sanitize"

echo "==> Configuring (ASan + UBSan, Debug)"
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DMEMPOOL_ENABLE_SANITIZERS=ON \
    -DMEMPOOL_BUILD_BENCHMARKS=OFF

echo "==> Building"
cmake --build "${BUILD_DIR}" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

echo "==> Running unit tests under ASan/UBSan"
ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" \
UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
"${BUILD_DIR}/unit_tests"

echo "==> Running demo driver under ASan/UBSan"
ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" \
UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
"${BUILD_DIR}/memory_pool_demo" >/dev/null

if command -v valgrind >/dev/null 2>&1; then
    echo "==> Configuring a separate non-sanitized Debug build for Valgrind"
    VG_BUILD_DIR="${ROOT_DIR}/build-valgrind"
    cmake -S "${ROOT_DIR}" -B "${VG_BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DMEMPOOL_ENABLE_SANITIZERS=OFF \
        -DMEMPOOL_BUILD_BENCHMARKS=OFF
    cmake --build "${VG_BUILD_DIR}" -j"$(nproc 2>/dev/null || echo 4)"

    echo "==> Running unit tests under Valgrind memcheck"
    valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 \
        "${VG_BUILD_DIR}/unit_tests"
else
    echo "==> Valgrind not found on PATH, skipping memcheck pass (ASan/UBSan already ran above)"
fi

echo "==> All sanitizer checks passed."
