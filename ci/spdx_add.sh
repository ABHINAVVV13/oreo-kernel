#!/usr/bin/env bash
# spdx_add.sh — Backfill SPDX-License-Identifier headers.
#
# Walks every C/C++ source file under src/, include/, tests/, and
# fuzzers/. Files that already contain `SPDX-License-Identifier:` in
# the first 8 lines are skipped. Files lacking it get the line
# `// SPDX-License-Identifier: LGPL-2.1-or-later` prepended (with one
# blank line of separation from any existing comment block).
#
# Run once after a new vendored source is added; verify with
# `ci/spdx_check.sh --strict`.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SPDX_LINE='// SPDX-License-Identifier: LGPL-2.1-or-later'
SCAN_DIRS=(src include tests fuzzers)

added=0
skipped=0

for dir in "${SCAN_DIRS[@]}"; do
    [ -d "$dir" ] || continue
    while IFS= read -r -d '' file; do
        if head -n 8 "$file" | grep -q "SPDX-License-Identifier:"; then
            skipped=$((skipped + 1))
            continue
        fi
        # Build the new file: SPDX line, blank line, then original.
        tmp="${file}.spdxtmp"
        {
            printf '%s\n\n' "$SPDX_LINE"
            cat "$file"
        } > "$tmp"
        mv "$tmp" "$file"
        added=$((added + 1))
    done < <(find "$dir" -type f \( -name '*.h'   -o \
                                    -name '*.hpp' -o \
                                    -name '*.hxx' -o \
                                    -name '*.c'   -o \
                                    -name '*.cc'  -o \
                                    -name '*.cpp' -o \
                                    -name '*.cxx' \) -print0)
done

echo "SPDX backfill: ${added} added, ${skipped} already had SPDX."
