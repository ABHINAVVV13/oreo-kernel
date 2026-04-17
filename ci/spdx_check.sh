#!/usr/bin/env bash
# spdx_check.sh — SPDX-License-Identifier coverage gate.
#
# Walks every C/C++ source file under src/, include/, tests/, and
# fuzzers/ and reports those missing an `SPDX-License-Identifier:`
# comment in the first 8 lines. Exits non-zero only when invoked
# with --strict (default behaviour is informational so the gate can
# be added to CI before the historical files are backfilled).
#
# Usage:
#   ci/spdx_check.sh             # informational; prints stats + missing files
#   ci/spdx_check.sh --strict    # exits 1 if any source file lacks SPDX
#   ci/spdx_check.sh --quiet     # only prints stats, no per-file list

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

STRICT=0
QUIET=0
for arg in "$@"; do
    case "$arg" in
        --strict) STRICT=1 ;;
        --quiet)  QUIET=1  ;;
        *)        echo "Unknown flag: $arg" >&2; exit 2 ;;
    esac
done

SCAN_DIRS=(src include tests fuzzers)
TOTAL=0
MISSING_FILES=()

for dir in "${SCAN_DIRS[@]}"; do
    [ -d "$dir" ] || continue
    while IFS= read -r -d '' file; do
        TOTAL=$((TOTAL + 1))
        if ! head -n 8 "$file" | grep -q "SPDX-License-Identifier:"; then
            MISSING_FILES+=("$file")
        fi
    done < <(find "$dir" -type f \( -name '*.h'   -o \
                                    -name '*.hpp' -o \
                                    -name '*.hxx' -o \
                                    -name '*.c'   -o \
                                    -name '*.cc'  -o \
                                    -name '*.cpp' -o \
                                    -name '*.cxx' \) -print0)
done

MISSING=${#MISSING_FILES[@]}
COVERED=$((TOTAL - MISSING))
if [ "$TOTAL" -gt 0 ]; then
    PCT=$((100 * COVERED / TOTAL))
else
    PCT=100
fi

echo "SPDX coverage: ${COVERED}/${TOTAL} files (${PCT}%) — ${MISSING} missing"

if [ "$QUIET" -eq 0 ] && [ "$MISSING" -gt 0 ]; then
    echo
    echo "Files missing SPDX-License-Identifier:"
    for f in "${MISSING_FILES[@]}"; do
        echo "  $f"
    done
fi

if [ "$STRICT" -eq 1 ] && [ "$MISSING" -gt 0 ]; then
    exit 1
fi
exit 0
