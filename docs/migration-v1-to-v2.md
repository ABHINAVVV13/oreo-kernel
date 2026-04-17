# Migrating from Identity v1 to v2

**Status:** ships with the v2 hardening (Phases 1–8, 2026-04-17).
**Audience:** engineers updating callers of the oreo-kernel API after the
identity-model v2 landing.
**Read first:** [docs/identity-model.md](identity-model.md) for the full
design rationale. This doc focuses on the concrete migration steps.

---

## 1. TL;DR

- Old: `int64_t tag = ctx.tags().nextTag()` — lossy in multi-doc mode.
- New: `oreo::ShapeIdentity id = ctx.tags().nextShapeIdentity()` —
  full 64+64 identity, no squeeze.

- Old: `NamedShape(shape, tag)` / `elmap->addChild({m, tag, pfx})`.
- New: `NamedShape(shape, tag)` still compiles (deprecated); for
  `ChildElementMap` the field was **renamed** `tag → id` and retyped,
  so the struct initializer must change.

- Old: buffer format version `1`.
- New: buffer format version `3`. v1 reads still work; v1 writes have
  been removed.

- Old: `oreo_face_name` returns a thread-local pointer (legacy-gated).
- New: `oreo_ctx_face_name(ctx, solid, index, buf, buflen, &needed)` —
  caller-owned buffer, size-probe protocol.

- New: `oreo_face_shape_id(solid, index, &out_shape_id)` — full 64-bit
  identity across the C ABI.

Everything v1 still compiles. Migrate at your own pace; grep-gate prevents
new callers from adding fresh v1 squeeze idioms.

---

## 2. Deprecated methods & their replacements

| v1 call                                   | v2 replacement                          |
|-------------------------------------------|-----------------------------------------|
| `TagAllocator::nextTag() -> int64_t`      | `nextShapeIdentity() -> ShapeIdentity`  |
| `TagAllocator::peek()`                    | `peekShapeIdentity()`                   |
| `TagAllocator::allocateRange(n)`          | `allocateRangeV2(n)`                    |
| `TagAllocator::encodeTag(seq)`            | `encodeV1Scalar(ShapeIdentity)`         |
| `TagAllocator::extractDocumentId(tag)`    | `ShapeIdentity::documentId`             |
| `TagAllocator::extractCounter(tag)`       | `ShapeIdentity::counter`                |
| `TagAllocator::Snapshot`                  | `TagAllocator::SnapshotV2`              |
| `TagAllocator::snapshot() / restore()`    | `snapshotV2() / restoreV2()`            |
| `TagAllocator::toOcctTag(int64_t)`        | `toOcctTag(ShapeIdentity)`              |
| `MappedName::appendTag(int64_t)` (writes `;:H`) | `appendShapeIdentity(ShapeIdentity)` (writes `;:P`) |
| `ElementMap::extractTag(name)` (v1 compat, keep) | `ElementMap::extractShapeIdentity(name)` |
| `oreo_face_name(solid, i)` (legacy gate)  | `oreo_ctx_face_name(ctx, solid, i, buf, buflen, &needed)` |
| `oreo_edge_name(solid, i)` (legacy gate)  | `oreo_ctx_edge_name(...)`               |
| `oreo_serialize / oreo_deserialize`       | `oreo_ctx_serialize / oreo_ctx_deserialize` |

The deprecated methods still compile with `[[deprecated]]` attributes.
MSVC fires C4996; GCC/Clang fire `-Wdeprecated-declarations`. The build
doesn't pass `/WX`, so this is noise, not a failure — but plan to fix
warnings proactively to make the eventual removal frictionless.

---

## 3. Field renames (no compat shim)

These are struct field changes. Existing initializers must be updated:

```cpp
// Before
ChildElementMap child;
child.tag = opTag;              // int64_t
child.postfix = ";:Cfoo";

// After
ChildElementMap child;
child.id = oreo::ShapeIdentity{doc, counter};  // ShapeIdentity
child.postfix = ";:Cfoo";
```

Same change for `HistoryTraceItem::tag -> ::id`.

Don't try to carry the old `int64_t tag` value into the new `id` field —
that loses the high 32 bits of documentId. Either get a fresh
`ShapeIdentity` from `nextShapeIdentity()`, or reconstruct via
`oreo::decodeV1Scalar(tag, ctx.tags().documentId(), nullptr)`.

---

## 4. On-disk format

- The outer `OREO_SERIALIZE_FORMAT_VERSION` jumped from `1` to `3`
  (skipping `2` — reserved/illegal at the outer layer for clarity).
- The inner `ElementMap::FORMAT_VERSION` jumped from `2` to `3`.
- v3 writers are mandatory for new code; there is no v1/v2 writer.
- v3 readers accept v3 (current) and v1 (legacy) buffers. v2 is a
  read-only inner format still accepted by `ElementMap::deserialize`.

**Lossiness rules for reading legacy buffers:**
- Single-doc v1 scalars (high-32 == 0) decode losslessly regardless of
  the reader's context (Case A).
- Multi-doc v1 scalars decode losslessly when the reader's context
  documentId's low-32 matches the scalar's high-32 (Case C).
- Multi-doc v1 scalars with no matching hint preserve the low 32 bits
  faithfully and emit `ErrorCode::LEGACY_IDENTITY_DOWNGRADE` once per
  top-level deserialize (Case B).
- Mismatched hints throw `std::invalid_argument`, which the reader
  surfaces as `DESERIALIZE_FAILED` — there is no silent cross-doc
  recovery (Error Case).

No reader ever fabricates the missing high 32 bits of documentId from
anything except an explicit matching hint.

See [identity-model.md §3.2](identity-model.md) and §5 for details.

---

## 5. Element name postfix

The per-op identity carrier changed from `;:H<hex>` (v1, variable
width) to `;:P<16hex>.<16hex>` (v2, fixed width, 33 chars). Readers
handle mixed chains via the rightmost-marker rule
([identity-model.md §4.3 rule 4](identity-model.md)):

```
;:Pdeadbeefcafebabe.0000000000000042;:M;:H20;:G
                                            ^ rightmost tag → v1 ;:H
```

`extractShapeIdentity` walks right-to-left, picks the rightmost of
`{;:P, ;:H, ;:T}`, and branches on which marker won. Malformed payloads
return `ShapeIdentity{0, 0}` and emit `MALFORMED_ELEMENT_NAME` on the
supplied diag sink — the parser never throws.

Size impact: a 50-op history chain goes from ≤ 950 bytes (v1 worst-case)
to ≤ 1,950 bytes (v2). If your callsite holds a very long chain in hot
paths, measure — but the `bench_identity` harness shows hash/compare
cost remains well under a microsecond per operation.

---

## 6. C API

Two new surfaces, always available (no legacy gate):

```c
typedef struct {
    uint64_t document_id;
    uint64_t counter;
} OreoShapeId;  // sizeof == 16, alignof == 8

int oreo_face_shape_id(OreoSolid s, int index, OreoShapeId* out);
int oreo_edge_shape_id(OreoSolid s, int index, OreoShapeId* out);

int oreo_ctx_face_name(OreoContext ctx, OreoSolid s, int i,
                        char* buf, size_t buflen, size_t* needed);
int oreo_ctx_edge_name(OreoContext ctx, OreoSolid s, int i,
                        char* buf, size_t buflen, size_t* needed);

int oreo_ctx_serialize(OreoContext ctx, OreoSolid s,
                        uint8_t* buf, size_t buflen, size_t* needed);
OreoSolid oreo_ctx_deserialize(OreoContext ctx,
                                const uint8_t* data, size_t len);
```

**Size-probe protocol:** pass `(buf=NULL, buflen=0, needed=&n)` first.
On success, `*needed` holds the required byte count. Allocate
`max(*needed, *needed + 1 for name functions)` and call again. This
removes thread-local hidden state from the public API surface.

**Error codes:** four new values are appended to `OreoErrorCode` —
`OREO_LEGACY_IDENTITY_DOWNGRADE`, `OREO_V2_IDENTITY_NOT_REPRESENTABLE`,
`OREO_MALFORMED_ELEMENT_NAME`, `OREO_BUFFER_TOO_SMALL`. Existing
switch-on-enum callers are safe (new values at the end, no reordering).

---

## 7. `OREO_ENABLE_LEGACY_API=OFF` builds

A CI job configured with `-DOREO_ENABLE_LEGACY_API=OFF` is now a merge
gate. The no-legacy build:

- Compiles the library (no legacy singleton symbols exported).
- Runs `test_doc_identity_nolegacy` plus every non-legacy test.
- Uses only `oreo_ctx_*` and `oreo_*_shape_id` for all identity work.

Four existing tests are gated on `OREO_ENABLE_LEGACY_API`:
`test_doc_identity`, `test_integration`, `test_perf`, and — from
earlier hardening — `test_foundation_battle2`. Any new test that
reaches for a legacy API must similarly gate itself, or port to the
ctx-aware surface first.

---

## 8. Grep-gate

`ci/grep_gate.sh` enforces that the v1 squeeze idiom lives in exactly
three files:

- `src/core/shape_identity.{h,cpp}`
- `src/core/shape_identity_v1.{h,cpp}`
- `src/core/tag_allocator.{h,cpp}`

A deliberate-break-then-revert PR was landed in Phase 1 to validate
the gate fires on new squeeze idioms. If you genuinely need to add a
new file to the allow-list, update `ci/grep_gate.sh`'s `EXCLUDES`
list in the same PR that adds the file — and justify the exception
in the PR description.

---

## 9. Things you DON'T have to do

- Change call sites for `ctx.tags().documentId()` — unchanged.
- Change `ctx.tags().setDocumentId(id)` — unchanged.
- Swap persistence format for existing on-disk files — v1 buffers
  still read.
- Move unit tests to a new framework — all existing tests continue to
  use Google Test.
- Adjust to a new minimum C standard for the public `oreo_kernel.h` —
  the C11 `_Static_assert` on `OreoShapeId` is guarded by
  `__STDC_VERSION__`, so C89/C99 consumers still build.

---

## 10. Cookbook

**Q: I have `int64_t tag` from somewhere and need a `ShapeIdentity`.**

```cpp
// If `tag` came from this context's allocator, the hint makes it lossless.
oreo::ShapeIdentity id =
    oreo::decodeV1Scalar(tag, ctx.tags().documentId(), /*diag=*/nullptr);
```

**Q: I have a `ShapeIdentity` and need to persist via a v1-format API.**

```cpp
try {
    int64_t scalar = oreo::encodeV1Scalar(id);
    // use scalar ...
} catch (const std::overflow_error& e) {
    // counter > UINT32_MAX in multi-doc mode — v1 format can't express this.
    // Route to v3 writer or surface V2_IDENTITY_NOT_REPRESENTABLE.
}
```

**Q: I want to look up an element's name without using `oreo_face_name`.**

```c
OreoContext ctx = /* ... */;
OreoSolid   s   = /* ... */;

size_t needed = 0;
oreo_ctx_face_name(ctx, s, /*index=*/1, NULL, 0, &needed);
char* buf = malloc(needed + 1);
oreo_ctx_face_name(ctx, s, /*index=*/1, buf, needed + 1, &needed);
// use buf ...
free(buf);
```

**Q: I want the face identity for cross-session mapping.**

```c
OreoShapeId sid;
if (oreo_face_shape_id(s, /*index=*/1, &sid) == OREO_OK) {
    // sid.document_id and sid.counter are the full 64-bit fields.
}
```

---

## 11. Open follow-ups

- **~~`NamedShape::shapeId()` accessor~~** — done. `NamedShape` now
  stores `ShapeIdentity shapeId_` natively; `shapeId()` is the primary
  accessor; `tag()` is the deprecated v1 shim that routes through
  `encodeV1Scalar` and returns 0 for the invalid sentinel.
- **~~Geometry ops migrating to `nextShapeIdentity()`~~** — done.
  15+ call sites in `src/geometry/**`, `src/sketch/**`, `src/io/**`,
  `src/feature/**` all went through a batch migration. The adapter
  layer (`TopoShapeAdapter` / FreeCAD extraction) still uses
  `adapter.Tag : int64` as an internal contract, but the NamedShape
  surface that enters and leaves is pure v2.
- **~~Native v3 deserialize for counter > UINT32_MAX~~** — done.
  The v3 reader no longer encodes the restored identity back through
  `encodeV1Scalar`; it flows straight into
  `NamedShape(shape, map, ShapeIdentity)`. The
  `V2_IDENTITY_NOT_REPRESENTABLE` code still exists for the legacy
  writer path (`ElementMap::extractTag` on names whose counter > UINT32_MAX)
  but is no longer reachable from `oreo_ctx_serialize` /
  `oreo_ctx_deserialize`.
- **~~Primitive naming writes `;:H` + `;:Q`~~** — done. The primitive
  path in `buildPrimitiveNames` now writes `;:P<32hex>` — the
  canonical v2 carrier (no separator to dodge FreeCAD's mapped-name
  validator, which forbids `.` and whitespace; see
  `src/naming/freecad/ElementMap.cpp:545`).

**Still open (not blockers for v2):**

- **Determinism golden per-platform** — the recorded golden in
  `test_determinism` is MSVC-specific. A GCC run produces a different
  hash due to map-ordering differences. Record a second golden
  keyed on `__GNUC__` before making the gate Linux-binding.
- **`TopoShapeAdapter::Tag` is still int64** — the FreeCAD extracted
  algorithm operates on int64 master tags internally. We pass
  `encodeV1Scalar(opId)` in and reconstruct `ShapeIdentity` via
  `decodeV1Scalar(adapter.Tag, documentId_, nullptr)` on the way out.
  For identities the allocator produces, this is lossless. For
  hand-crafted v2 identities with counter > UINT32_MAX in multi-doc
  mode, the FreeCAD code path cannot be used — they'd throw
  `encodeV1Scalar` overflow on entry. This is a genuine limitation
  inherited from the upstream extraction, not a bug; document as a
  constraint if callers hit it.
- **Determinism of internal adapter tags** — `adapter.Tag` collisions
  aren't currently checked; two distinct identities that squeeze to
  the same v1 scalar would share `adapter.Tag`, which could cause
  FreeCAD-extraction logic to alias them internally. TagAllocator's
  process-global low-32 collision registry prevents this at allocation
  time, but a hand-constructed caller that bypasses `nextShapeIdentity`
  could trip it. Low priority — no existing caller does this.
