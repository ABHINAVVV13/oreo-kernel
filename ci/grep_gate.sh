#!/usr/bin/env bash
# grep_gate.sh — CI gate preventing new callers of the v1 documentId
# squeeze outside the three sanctioned files.
#
# See docs/identity-model.md §3.3. The idiom we're forbidding is any
# form of `docId & 0xFFFFFFFF` or the shifted pack into the high 32
# bits of an int64 tag. Three patterns cover the three ways the idiom
# appears in real code:
#
#   1. `(docId|documentId|id) & 0xFFFFFFFF[uUlL]*`      — mask to low 32
#   2. `0xFFFFFFFF[uUlL]* \)? << 32`                    — shift into high 32
#   3. `static_cast<uint32_t>((docId|documentId|id) & …)` — explicit narrowing
#
# Allow-list (three files — same rationale as §3.3):
#   * src/core/shape_identity.{h,cpp}   — the ShapeIdentity type itself
#   * src/core/shape_identity_v1.{h,cpp} — the sanctioned v1 bridge
#   * src/core/tag_allocator.{h,cpp}    — v1-compat shim
#
# Usage:
#   ci/grep_gate.sh            # exits 0 on clean tree, 1 on new callers
#   ci/grep_gate.sh --self-test # flips the exit inversion for the
#                                 "deliberate break" validation PR
#                                 described in §3.3.
#
# Depends on ripgrep (`rg`). Falls back to GNU grep if not available.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# Patterns to search. Any hit outside the allow-list fails the gate.
PATTERNS=(
    '\b(docId|documentId|id)\s*&\s*0x[fF]{8}[uUlL]*\b'
    '\b0x[fF]{8}[uUlL]*\s*\)?\s*<<\s*32\b'
    'static_cast<\s*(uint32_t|std::uint32_t)\s*>\s*\(\s*(docId|documentId|id)\s*&'
)

# File globs excluded from the gate. Update deliberately, never casually.
EXCLUDES=(
    ':(exclude)src/core/shape_identity.h'
    ':(exclude)src/core/shape_identity.cpp'
    ':(exclude)src/core/shape_identity_v1.h'
    ':(exclude)src/core/shape_identity_v1.cpp'
    ':(exclude)src/core/tag_allocator.h'
    ':(exclude)src/core/tag_allocator.cpp'
)

# Choose the search command. Prefer rg for consistency with the CI
# config most teams use; fall back to grep -Er -P (PCRE) if rg is
# unavailable on a developer machine.
if command -v rg >/dev/null 2>&1; then
    SEARCH=(rg --no-heading --no-messages --color=never
            -g '!src/core/shape_identity*.{h,cpp}'
            -g '!src/core/tag_allocator*.{h,cpp}'
            -g 'src/**' -g 'include/**')
    RUNNER() {
        local pat="$1"
        "${SEARCH[@]}" -n -e "$pat"
    }
else
    echo "grep_gate: ripgrep not found; falling back to grep -Er -P"
    RUNNER() {
        local pat="$1"
        grep -Ern "$pat" src include 2>/dev/null |
            grep -v '^src/core/shape_identity' |
            grep -v '^src/core/tag_allocator'
    }
fi

HITS=0
for pat in "${PATTERNS[@]}"; do
    out=$(RUNNER "$pat" || true)
    if [ -n "$out" ]; then
        echo "---- v1 squeeze idiom detected outside the allow-list ----"
        echo "$out"
        echo ""
        HITS=1
    fi
done

if [ "$HITS" -eq 0 ]; then
    echo "grep_gate: OK — no v1 squeeze idioms outside sanctioned files."
fi

# Self-test mode flips the exit inversion so a "deliberately broken"
# PR can validate the gate. The idea: add a squeeze to some unrelated
# file, run `ci/grep_gate.sh --self-test`, confirm exit 0 (gate fired
# as expected). Then revert.
if [ "${1:-}" = "--self-test" ]; then
    if [ "$HITS" -eq 1 ]; then
        echo "grep_gate --self-test: gate fired as expected (deliberate break)."
        exit 0
    else
        echo "grep_gate --self-test: gate did NOT fire — the test setup is wrong."
        exit 1
    fi
fi

exit "$HITS"
