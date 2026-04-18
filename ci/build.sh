#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# build.sh — Single source of truth for "configure + build + test".
#
# Driven entirely by environment variables so the CI matrix (and any
# downstream contributor running it locally) can pick a configuration
# without editing this file or the workflow YAML.
#
# Required: none. All vars have defaults that match a vanilla GCC
# release build.
#
# Recognised env vars:
#   BUILD_DIR        — out-of-source dir to use (default: build)
#   CC, CXX          — compilers (default: cc, c++)
#   BUILD_TYPE       — CMAKE_BUILD_TYPE (default: Release)
#   LEGACY_API       — ON|OFF (default: ON)
#   SANITIZER        — none|address|undefined|address+undefined|thread|memory
#                      (default: none)
#   BUILD_FUZZERS    — ON|OFF (default: OFF). When ON, also builds and
#                      smoke-runs the libFuzzer harnesses for 60s each.
#   RUN_GATES        — 1|0 (default: 0). When 1, additionally runs the
#                      grep_gate.sh + spdx_check.sh policy gates.
#
# This script is invoked from .github/workflows/ci.yml and is also the
# recommended local entrypoint:
#   docker run --rm -v "$PWD":/w -w /w \
#       ghcr.io/abhinavvv13/oreo-kernel-dev:latest ci/build.sh

set -euo pipefail

BUILD_DIR=${BUILD_DIR:-build}
CC=${CC:-cc}
CXX=${CXX:-c++}
BUILD_TYPE=${BUILD_TYPE:-Release}
LEGACY_API=${LEGACY_API:-ON}
SANITIZER=${SANITIZER:-none}
BUILD_FUZZERS=${BUILD_FUZZERS:-OFF}
RUN_GATES=${RUN_GATES:-0}

echo "── build.sh ────────────────────────────────────────────"
echo "  BUILD_DIR     = $BUILD_DIR"
echo "  CC / CXX      = $CC / $CXX"
echo "  BUILD_TYPE    = $BUILD_TYPE"
echo "  LEGACY_API    = $LEGACY_API"
echo "  SANITIZER     = $SANITIZER"
echo "  BUILD_FUZZERS = $BUILD_FUZZERS"
echo "  RUN_GATES     = $RUN_GATES"
echo "────────────────────────────────────────────────────────"

# ── Configure ──────────────────────────────────────────────────
CMAKE_ARGS=(
    -S .
    -B "$BUILD_DIR"
    -G Ninja
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DCMAKE_C_COMPILER="$CC"
    -DCMAKE_CXX_COMPILER="$CXX"
    -DOREO_ENABLE_LEGACY_API="$LEGACY_API"
    -DOREO_ENABLE_SANITIZER="$SANITIZER"
    -DOREO_BUILD_FUZZERS="$BUILD_FUZZERS"
)
cmake "${CMAKE_ARGS[@]}"

# ── Build ──────────────────────────────────────────────────────
cmake --build "$BUILD_DIR" --parallel

# ── Test ───────────────────────────────────────────────────────
# Sanitizer environment hardens detection without changing behaviour
# of non-sanitized builds.
#
# Suppression files live next to this script and are scoped to the
# third-party libraries we link against (OCCT in particular). We
# resolve their absolute path here because ctest launches each test
# binary with cwd=<build dir>, so a relative suppressions= path
# wouldn't find them.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=1:halt_on_error=1}
export LSAN_OPTIONS=${LSAN_OPTIONS:-suppressions=${SCRIPT_DIR}/lsan.supp:print_suppressions=0}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-suppressions=${SCRIPT_DIR}/ubsan.supp:print_stacktrace=1:halt_on_error=1}
ctest --test-dir "$BUILD_DIR" --output-on-failure

# ── Fuzzers (optional) ─────────────────────────────────────────
# When BUILD_FUZZERS=ON, exercise each harness for 60s against the
# checked-in seed corpora. Crashes leave artefacts at the working
# dir for the workflow to upload.
if [[ "$BUILD_FUZZERS" == "ON" ]]; then
    for h in fuzz_deserialize fuzz_step_import fuzz_feature_tree_json; do
        echo "── fuzzer: $h ──"
        "./$BUILD_DIR/fuzzers/$h" \
            -max_total_time=60 \
            -timeout=10 \
            -rss_limit_mb=2048 \
            "corpora/$h/"
    done
fi

# ── Policy gates (optional) ────────────────────────────────────
if [[ "$RUN_GATES" == "1" ]]; then
    echo "── grep gate (identity v1 squeeze) ──"
    ci/grep_gate.sh
    echo "── SPDX gate ──"
    ci/spdx_check.sh --strict
fi

echo "── build.sh complete ──"
