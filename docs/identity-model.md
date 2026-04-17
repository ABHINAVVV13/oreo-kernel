# oreo-kernel Identity Model — v2 Design Lock

**Status:** IMPLEMENTED (rev3 of the Phase 0 design lock, Phases 1–8 shipped).
The design decisions below are what shipped. See
[docs/migration-v1-to-v2.md](migration-v1-to-v2.md) for the concrete
migration cookbook.
**Author:** foundation team
**Date:** 2026-04-17 (rev2: same day, incorporates advisor review dated 2026-04-17)
**Supersedes:** v1 document-identity plumbing (landed earlier on this branch).
**Exit gate:** advisor approval on this document → only then does Phase 1 begin.

**Revision history:**
- rev1 (2026-04-17): initial draft.
- rev2 (2026-04-17): addresses advisor blockers B1 (grep-gate is a paper tiger),
  B2 (rightmost-marker rule for `extractShapeIdentity`), B3 (hint/scalar mismatch);
  material gaps M1 (`;:R` in landscape), M2 (`extractTag` on v2-only counters),
  M3 (C11 guard on public header asserts), M4 (version vs. identity terminology),
  M5 (warning dedup scope); nits N1–N8. One open question (§8.1, `;:I` conflict)
  remains for advisor pick; Q4 and Q8 resolved in place.
- rev3 (2026-04-17): addresses advisor rev2-review items R2-M1 (§§5.3/5.4
  reader semantics resynced with the rewritten §3.2 — no duplicate
  diagnostic logic; `decodeV1Scalar` is the single source of truth) and
  R2-N1 (§4.3 rule 4 now explicitly catches `std::invalid_argument` from
  the Error Case and funnels it through the malformed-payload path).
  R2-N2 and R2-N3 deferred to Phase 1/4 PR review per advisor guidance.
  §8 Q1 and Q8 remain for advisor sign-off comment, not doc revision.

---

## 1. Motivation

The v1 fix plumbed a 64-bit `documentId` through `KernelContext` and stamped it
into element names via the `;:Q` postfix, which closed the Windows truncation
and low-32 collision bugs. But the scalar *on-the-wire tag* produced by
`TagAllocator::nextTag()` is still:

```
tag_v1 = (low32(documentId) << 32) | uint32(counter)
```

Two engineering consequences follow:

1. **docId lossy in the tag itself.** The high 32 bits of `documentId` live
   only in `KernelContext` and in the top-level `;:Q` postfix. They are not
   in `NamedShape::tag_`, not in serialized per-entry tags, and not in
   `ChildElementMap::tag`. Anything that reads a tag without a context must
   guess or default.
2. **Counter capped at 2^32.** `TagAllocator` throws in multi-doc mode once
   any single document exceeds 2^32 ops. That's fine for a CAD session, but
   it's a cliff, not a ceiling we want to design around.

v2 promotes identity to a first-class 16-byte value and removes the squeeze.

---

## 2. `ShapeIdentity`

### 2.1 Definition

```cpp
// src/core/shape_identity.h
namespace oreo {

struct ShapeIdentity {
    std::uint64_t documentId;  // full 64-bit doc identity; 0 = single-doc
    std::uint64_t counter;     // per-document sequence number; 0 = invalid

    // Defaulted operator<=> synthesizes operator== in C++20 — no need to
    // declare both (N2). Field-wise compare on (documentId, counter).
    constexpr auto operator<=>(const ShapeIdentity&) const = default;

    constexpr bool isSingleDoc() const { return documentId == 0; }
    constexpr bool isValid()     const { return counter != 0; }

    // Display form — used by logs, diagnostics, and MappedName encoding.
    // Always 33 chars: "<16hex>.<16hex>". No leading-zero trim — fixed
    // width is required for deterministic byte-exact encoding goldens.
    std::string toHex() const;
    static std::optional<ShapeIdentity> fromHex(std::string_view);
};

}  // namespace oreo

// Hash specialization — Boost-style combine of the two 64-bit halves.
template <> struct std::hash<oreo::ShapeIdentity> { /* ... */ };
```

### 2.2 Invariants

| # | Invariant | Enforced by |
|---|-----------|-------------|
| I1 | `counter == 0` ⇒ identity is invalid (sentinel for "unset / default"). **Producers** (allocator, decoders) MUST NOT emit an identity with `counter == 0`. **Consumers** are not required to assert — `ShapeIdentity{docId, 0}` is constructible as an "unset" sentinel. Debug builds MAY add `assert(id.isValid())` at entry to functions that semantically require a live identity, but this is not part of the type contract. | Producers enforce; consumers may opt in. |
| I2 | If `documentId == 0` then the kernel is in single-doc mode and cross-doc equality is undefined. | `isSingleDoc()` callers. |
| I3 | `ShapeIdentity` values are immutable once produced by `TagAllocator`. | Type is a POD-with-operators; no mutating ops provided. |
| I4 | Layout is `{uint64_t, uint64_t}` with no padding — `sizeof == 16`, `alignof == 8`. | `static_assert` in header. |
| I5 | No implicit conversion to/from `int64_t`. Every v1 bridge is named. | No conversion operators. Grep-gate (§3.3). |

### 2.3 Equality, ordering, hashing

- **Equality:** defaulted, field-wise.
- **Ordering:** defaulted `<=>`, which gives `(documentId, counter)` lexicographic
  compare. This matches the v1 sort key when restricted to a single doc, so
  snapshot dumps stay stable after the migration.
- **Hashing:** combine `documentId` and `counter` via the standard 64-bit mix
  (Boost `hash_combine` or XXH3 — decision deferred to Phase 1, but the choice
  is not ABI-visible so this is safe to defer).

### 2.4 Display / encoding form

Single canonical text form: `"<16hex>.<16hex>"`, lowercase, zero-padded.
Example: `ShapeIdentity{0xDEADBEEFCAFEBABE, 0x42}` displays as
`"deadbeefcafebabe.0000000000000042"`.

Rationale for fixed 16+16 width: deterministic byte-exact encoding goldens
(MSVC vs GCC, vs future runs) require a width that does not depend on the
magnitude of either field. The ~17-byte overhead per occurrence vs. v1's
variable-length `;:H` is an accepted cost — see §4.4 for a size budget.

---

## 3. v1 ↔ v2 Interop

### 3.1 The only sanctioned bridges

```cpp
// src/core/shape_identity_v1.h

// Encode a ShapeIdentity into a v1 int64 tag. Throws std::overflow_error
// if counter > UINT32_MAX (v1 had this cliff; we keep it at the boundary
// rather than in the v2 core).
std::int64_t encodeV1Scalar(ShapeIdentity id);

// Decode a v1 int64 tag. See §3.2 for the full case table (three cases
// keyed on the scalar's high-32 bits vs. fullDocIdHint). Summary:
//   - Throws std::invalid_argument if low32(fullDocIdHint) does not match
//     the scalar's high-32 bits when both are nonzero (mismatch is always
//     a bug — caller passed the wrong hint or the scalar is corrupt).
//   - Fires ErrorCode::LEGACY_IDENTITY_DOWNGRADE via the caller-supplied
//     diag sink when high bits are inferred from low-32 alone (lossy path).
//   - Does NOT fabricate high-32 bits from anything other than an
//     explicit, matching hint.
ShapeIdentity decodeV1Scalar(std::int64_t scalar,
                             std::uint64_t fullDocIdHint = 0,
                             DiagnosticCollector* diag = nullptr);
```

### 3.2 `decodeV1Scalar` case table (non-negotiable)

The function's behavior is fully determined by two conditions: whether the
scalar's high-32 bits are zero (single-doc encoding) and whether a
`fullDocIdHint` was supplied. Three cases, no others:

**Case A — scalar was single-doc encoded (`high32(scalar) == 0`).**
The v1 encoder produced `scalar = counter` directly, with no docId bits
stolen. This case is **never lossy**, regardless of hint:

- `counter = uint64(scalar)` — use the full 64 bits. v1 single-doc scalars
  are bounded by `INT64_MAX` so the cast is well-defined.
- `documentId = fullDocIdHint` — if the caller supplies a hint, attach it;
  otherwise `documentId = 0` (single-doc output).
- No diagnostic fires.

**Case B — scalar was multi-doc encoded, no matching hint.**
`high32(scalar) != 0` AND (`fullDocIdHint == 0` OR `low32(fullDocIdHint) != high32(scalar)` — see Case C).
When the hint is zero:

- `documentId = uint64(high32(scalar))` — preserves the low 32 bits
  faithfully; the original high 32 bits of the docId are **irrecoverable**.
- `counter = uint64(low32(scalar))`.
- Fires `ErrorCode::LEGACY_IDENTITY_DOWNGRADE` on the caller-supplied diag
  sink. Caller is responsible for surfacing the warning.
- MUST NOT invent high-32 bits from any source other than an explicit
  matching hint (Case C).

**Case C — scalar was multi-doc encoded, hint matches.**
`high32(scalar) != 0` AND `fullDocIdHint != 0` AND `low32(fullDocIdHint) == high32(scalar)`.
This is the successful cross-document upgrade path.

- `documentId = fullDocIdHint` — full 64-bit reconstruction.
- `counter = uint64(low32(scalar))`.
- No diagnostic — the read was lossless.

**Error case — hint/scalar mismatch.**
`high32(scalar) != 0` AND `fullDocIdHint != 0` AND `low32(fullDocIdHint) != high32(scalar)`.
This is **always a bug**: either the caller passed the wrong context, the
buffer was tampered with, or two documents' serialized state got crossed.
There is no correct recovery that preserves data integrity.

- **Throw `std::invalid_argument`** with a message including both values.
- Do NOT trust the scalar (silently dropping the hint hides caller bugs).
- Do NOT trust the hint (would fabricate docId bits — violates §3.2
  core invariant).

**Invariants shared by all cases:**
- `counter == 0` in the output never occurs unless the scalar itself was
  zero. `encodeV1Scalar` rejects `counter == 0` inputs with
  `std::invalid_argument` to keep the round-trip symmetric.
- The function never fabricates the missing high 32 bits from anything
  other than a matching `fullDocIdHint`.

### 3.3 CI-enforced squeeze containment (revised per advisor B1)

**The rev1 regex was a paper tiger.** Cross-checked against the real
idiom at [tag_allocator.h:132](../src/core/tag_allocator.h):

```cpp
return (static_cast<int64_t>(docId & 0xFFFFFFFFu) << 32)
     | (seq & 0xFFFFFFFFll);
```

Three independent misses in the rev1 pattern: (a) the local is `docId`,
not `documentId`; (b) `0xFFFFFFFFu?` followed by whitespace-then-`<<` fails
against `0xFFFFFFFFu) << 32` because of the closing paren; (c) integer
suffixes `ull`, `ll`, `llu`, `lu` are not covered. New callers would have
passed CI. The guardrail is replaced.

**Approach (chosen, not option-A vs option-B):** force every v1-squeeze
through a single named function, then ban the idiom everywhere else.

```cpp
// src/core/shape_identity_v1.h
namespace oreo::internal {
    // Extract the 32 encode-visible bits of a 64-bit documentId. The
    // ONLY place in the codebase allowed to materially perform this
    // operation. encodeV1Scalar and TagAllocator's v1 shim (§4.2) call
    // it; nothing else should.
    constexpr std::uint32_t squeezeV1DocId(std::uint64_t documentId) noexcept {
        return static_cast<std::uint32_t>(documentId & 0xFFFFFFFFu);
    }
}
```

**CI command:**

```
rg --no-heading -n \
   -e '\b(docId|documentId|id)\s*&\s*0x[fF]{8}[uUlL]*\b' \
   -e '\b0x[fF]{8}[uUlL]*\s*\)?\s*<<\s*32\b' \
   -e 'static_cast<\s*(uint32_t|std::uint32_t)\s*>\s*\(\s*(docId|documentId|id)\s*&' \
   src/ include/ \
   -g '!src/core/shape_identity_v1.{h,cpp}' \
   -g '!src/core/shape_identity.{h,cpp}' \
   -g '!src/core/tag_allocator.{h,cpp}'      # v1-compat shim (§4.2)
```

Non-empty output = build fails. Allow-list is exactly three files. Any
squeeze idiom outside them is either a new caller (reject) or a rename of
an existing site (review carefully — likely should move into
`squeezeV1DocId`).

**Guardrail validation (Phase 1 acceptance requirement):**
Before Phase 1 merges, land a throw-away PR that introduces a deliberate
squeeze outside the allow-list and confirm CI fails. Then revert. A
passing green CI on a known-broken input is proof the pattern works — no
"trust the regex." Without this validation step, Phase 1 is not considered
complete.

**Fallback if the regex proves brittle in practice:** switch to a
file-allow-list approach: CI diffs touched files against
`tools/ci/squeeze_allowlist.txt` and fails if any file not on the list
contains the literal substring `& 0xFFFFFFFF`. Simpler, fewer false
positives, requires review to add a file to the allow-list. Decision
deferred to Phase 1 experience; regex is the primary.

---

## 4. MappedName Postfix Encoding

### 4.1 Existing landscape

Source of truth: `src/naming/mapped_name.h::ElementCodes` and
`src/naming/freecad/ElementNamingUtils.h`. This table is hand-maintained
today; **M1 follow-up:** once Phase 3 lands, regenerate from the
`POSTFIX_*` constants in CI so it can't drift.

```
;:H<hex>       POSTFIX_TAG              — v1 op tag, hex (variable width, truncating-on-Win32-pre-audit)
;:T<dec>       POSTFIX_DECIMAL_TAG      — v1 op tag, decimal
;:M / ;:G      POSTFIX_MOD / GEN        — modified / generated
;:MG           POSTFIX_MODGEN
;:C / ;:U / ;:L                         — child / upper / lower
;D<n>          POSTFIX_DUPLICATE        — disambiguation (note single colon is absent)
;:I            POSTFIX_INDEX            — ALREADY TAKEN ←── conflict with the plan
;:X            POSTFIX_EXTERNAL         — POSTFIX_EXTERNAL_TAG in FreeCAD layer
;:R            MAPPED_CHILD_ELEMENTS_PREFIX — child-elements marker (FreeCAD ElementNamingUtils.h:53)
;:Q<16hex>     POSTFIX_DOC              — v1-audit top-level docId stamp (keep)
```

### 4.2 Open question — `;:I` conflict with the plan

The Phase 3 plan text proposes `;:I<16-hex>.<16-hex>` for the identity
postfix, but `;:I` is already `POSTFIX_INDEX` in
[mapped_name.h:49](../src/naming/mapped_name.h) and
[freecad/ElementNamingUtils.h:64](../src/naming/freecad/ElementNamingUtils.h).
Candidates (all currently free, none appearing as `POSTFIX_*` constants or
as single-letter markers used by the FreeCAD extraction):

| Candidate | Pros | Cons |
|-----------|------|------|
| `;:P`     | "Pair" — mnemonic for docId+counter; short. | None identified. |
| `;:Y`     | "identitY" — avoids overloading P. | Less mnemonic. |
| `;:Z`     | Unused, avoids any existing convention. | No semantic hint. |

**Advisor decision required.** The rest of this document assumes `;:P`.
Replace globally if advisor chooses otherwise; no other design impact.

### 4.3 v2 postfix definitions (additions, not replacements)

```
;:P<16hex>.<16hex>   POSTFIX_IDENTITY   — full v2 identity; docId.counter, fixed-width, lowercase
;:Q<16hex>           POSTFIX_DOC        — kept; top-level docId stamp (redundant when ;:P is present
                                          on every sub-name, but needed for v1-era names that only
                                          carry ;:H)
```

**Rules (enforced at the naming layer):**

1. Every `MappedName` produced by the v2 naming path ends in one `;:P` per op
   hop and does **not** also carry a `;:H` for the same op — `;:P` is the
   authoritative carrier.
2. `;:Q` is written **at most once** per top-level name. A re-encoded
   sub-name that inherits `;:Q` from its input must not duplicate the stamp.
3. `;:H` is still emitted by legacy code paths (`appendTag(std::int64_t)`)
   which is marked `[[deprecated]]` in Phase 3. Parsers must read it; new
   code must not write it.
4. **`extractShapeIdentity(name)` — rightmost-marker algorithm** (revised
   per advisor B2). Mixed chains are the common case during migration
   (e.g., `;:Pdeadbeef….0001;:M;:H<v1-scalar>;:G` — a v2 op followed by a
   v1 op), so the reader cannot simply prefer `;:P`. Instead:

   ```
   a. Let hDoc = extractDocumentId(name)   // reads ;:Q, returns 0 if absent
   b. Find all rightmost-preceding occurrences of the three markers:
        posP = name.rfind(";:P")
        posH = name.rfind(";:H")
        posT = name.rfind(";:T")
      "Rightmost" means the one with the LARGEST index; ties cannot occur
      because each marker starts with a distinct char.
   c. Branch on which marker won:
      - winner == ;:P  → parse "<16hex>.<16hex>" at posP+3, return directly.
      - winner == ;:H  → parse hex scalar at posH+3, return
                         decodeV1Scalar(scalar, hDoc, /*diag=*/...).
      - winner == ;:T  → parse decimal scalar at posT+3, same as ;:H path.
      - no marker      → return ShapeIdentity{0, 0} (invalid). This aligns
                         with the current element_map.cpp:246 convention
                         where extractTag returns 0 on a bare base name.
   d. Malformed payload at the chosen marker (non-hex digit immediately
      after ;:P, missing "." separator, short hex field, etc.) → return
      ShapeIdentity{0, 0} AND emit ErrorCode::MALFORMED_ELEMENT_NAME if a
      diag sink was supplied. Never throw from the parser — it runs on
      untrusted buffer input.
   e. If the ;:H or ;:T branch invokes decodeV1Scalar and §3.2's Error
      Case fires (hint/scalar mismatch — reachable from a crafted name
      like ";:H<scalar>;:Q<unrelated-docId>"), decodeV1Scalar throws
      std::invalid_argument. extractShapeIdentity MUST catch it and
      funnel through rule 4d: return ShapeIdentity{0, 0} and emit
      MALFORMED_ELEMENT_NAME. The parser's no-throw guarantee extends
      across this boundary so callers reading untrusted buffers can rely
      on it unconditionally.
   ```

   The `decodeV1Scalar` branch inherits the §3.2 case table: if a scalar
   has zero high-32 bits (single-doc encoding) the read is not flagged as
   lossy, even when `hDoc == 0`. Only multi-doc v1 scalars without a `;:Q`
   produce the downgrade warning.

5. **`extractTag(name) -> int64_t` — v1-compat, specified behavior on
   v2-only counters** (revised per advisor M2). The function returns
   `encodeV1Scalar(extractShapeIdentity(name))`. `encodeV1Scalar` throws
   `std::overflow_error` when `counter > UINT32_MAX`. This is unreachable
   in v1-produced names but reachable in v2-produced names once the
   counter cap is removed.

   **Locked contract:** the legacy serializer (`oreo_serialize`,
   `ElementMap::serialize`) wraps every `extractTag` call in a try/catch:

   ```cpp
   try {
       int64_t scalar = ElementMap::extractTag(name);
       writeI64(buf, scalar);
   } catch (const std::overflow_error&) {
       ctx.diag().error(ErrorCode::V2_IDENTITY_NOT_REPRESENTABLE,
           "v2 identity cannot be expressed in the v1/v2 serialization "
           "format (counter > UINT32_MAX). Use the v3 writer.");
       return scope.makeFailure<std::vector<std::uint8_t>>();
   }
   ```

   Net effect: a v2 shape with a too-large counter serialized via a legacy
   API fails loudly and returns a null buffer. No partial writes, no
   silent truncation. The v3 writer (`oreo_ctx_serialize`, §6.2) has no
   such limit — the cliff exists only on the v1/v2 compat path, which is
   correct.

### 4.4 Size budget

Per op hop:

- v1 (best case, single-doc, small counter): `;:H1` — 4 bytes.
- v1 (worst case, multi-doc, full hex): `;:Hdeadbeefcafebabe` — 19 bytes.
- v2: `;:Pdeadbeefcafebabe.0000000000000042` — 36 bytes.

A 50-op history chain is therefore ≤ 1,800 bytes in v2 (vs. ≤ 950 in v1
worst-case) counting only `;:P`/`;:H` hops. Real names also carry `;:M`,
`;:G`, `;:MG`, disambiguation suffixes, and one `;:Q` per top-level name —
adding roughly 150 bytes on top, giving a realistic 50-op v2 ceiling near
~1,950 bytes. The `ElementMap` stores each name once and hashes by value,
so the dominant cost is memcmp on name compare. Phase 7 benchmarks gate
the actual impact.

---

## 5. Serialization

> **Terminology callout (M4).** This document uses "identity v1" to mean
> the current state of the branch — where `OREO_SERIALIZE_FORMAT_VERSION = 1`
> at the outer buffer AND `ElementMap::FORMAT_VERSION = 2` at the inner
> layer. "Identity v2" is what this plan delivers: both version numbers
> become `3`. The "v1" / "v2" labels throughout §§2–7 refer to the
> identity-model generation, not to any specific on-disk format byte.
> When a format version byte is meant, the doc says "outer-format N" or
> "ElementMap format N" explicitly.

### 5.1 Version numbering

| Layer                                  | Pre-v1 | v1   | v2 (this doc) |
|----------------------------------------|--------|------|---------------|
| `OREO_SERIALIZE_FORMAT_VERSION` (outer buffer) | — (no byte) | `1` | `3` |
| `ElementMap::FORMAT_VERSION`           | `1`    | `2`  | `3`           |

The outer buffer jumps 1 → 3 to **unify** the version number with
`ElementMap::FORMAT_VERSION`. Version 2 at the outer layer is **reserved and
illegal**: a reader seeing an outer version byte of 2 must reject with
`DESERIALIZE_FAILED`. This prevents confusion with intermediate in-tree
builds.

### 5.2 Endianness

Little-endian, same as v1. All multi-byte integers are written LSB-first.
The existing writer/reader helpers (`writeI64`/`readI64` in `oreo_serialize.cpp`,
and the in-file lambdas in `element_map.cpp`) already satisfy this.

### 5.3 Outer buffer layout — v3

```
offset  size  field
 0      1     format_version = 3
 1      4     brep_len : u32
 5      N     brep_payload
 5+N    4     element_map_len : u32
 9+N    M     element_map_payload (inner format, see §5.4)
 9+N+M  16    root_identity : { u64 documentId, u64 counter }  ← was 8 bytes in v1
```

Net delta vs. v1: `+8` bytes per buffer (header unchanged, tail grows 8
bytes). The `format_version` byte location is unchanged, so a v3 reader
can reject v1 buffers with a single-byte check before any further parsing.

**Reader semantics (v3 writer / v3 reader path):**
Read 16 bytes at offset `9+N+M` as `{u64 docId, u64 counter}`.

**Reader semantics (v1 buffer opened by v3 reader)** — resynced per
R2-M1 so §3.2 is the single source of truth for decode + diagnostics.

1. Read tag as 8 bytes (v1 layout).
2. Call `ShapeIdentity root = decodeV1Scalar(tag, ctx.tags().documentId(), &ctx.diag())`.
   §3.2's case table fully determines the outcome:
   - Case A (`high32(tag) == 0`, single-doc scalar): `root` fully
     reconstructed from the scalar, no diagnostic.
   - Case B (multi-doc scalar, no hint or zero hint): low-32 preserved,
     high-32 from scalar's upper half, `LEGACY_IDENTITY_DOWNGRADE` fires.
   - Case C (multi-doc scalar, matching hint): `root` fully reconstructed
     with the context's 64-bit docId, no diagnostic.
3. If `decodeV1Scalar` throws `std::invalid_argument` (§3.2 Error Case —
   hint/scalar mismatch: the context's docId low-32 does not equal the
   scalar's high-32): the outer deserialize catches it, records
   `ErrorCode::DESERIALIZE_FAILED` with the thrown message, and returns
   a null shape. This state indicates a tampered buffer or a cross-
   document paste and must not be silently recovered.
4. No independent post-hoc `root.documentId != ctx.tags().documentId()`
   check. §3.2 owns that branch entirely — the rev1 duplicate logic is
   gone.

Crucially: the v3 reader **never fabricates** high 32 bits from anything
other than the caller-supplied context hint. An open-and-save round-trip
without a matching hint preserves low-32 faithfully and fills high-32 with
zero. This property is now a direct consequence of §3.2 Case B; the
reader does nothing additional to enforce it.

### 5.4 Inner `ElementMap` layout — v3

```
offset  size  field
 0      4     format_version = 3
 4      4     entry_count : u32
        ...   entries ...
       4     child_count : u32
        ...  children ...

entry (v3):
  2     type_string_length : u16
  N     type_string
  4     index : i32
  4     name_length : u32
  M     mapped_name_bytes
  16    identity : { u64 documentId, u64 counter }   ← was 8 bytes (int64 tag) in v2

child (v3):
  16    identity : { u64 documentId, u64 counter }   ← was 8 bytes in v2
  4     postfix_length : u32
  N     postfix_bytes
  4     child_map_length : u32
  M     child_map_payload (recursive, v3 inner format)
```

**v2 → v3 upgrade path (reader only, no writer)** — resynced per R2-M1.

- Outer-layer v1 reader is already kept (see §5.3). Inner-layer v2 reader
  must be added alongside the v3 reader.
- On each per-entry 8-byte tag, call
  `decodeV1Scalar(tag, rootIdentity.documentId, &ctx.diag())`.
  `rootIdentity` was read from the outer buffer one layer up; it is the
  hint. All diagnostic emission is owned by §3.2 — the inner reader does
  no independent "did-the-hint-match" check.
- If `decodeV1Scalar` throws `std::invalid_argument` mid-entry (§3.2 Error
  Case), the inner `ElementMap::deserialize` catches, records
  `ErrorCode::DESERIALIZE_FAILED`, and returns `nullptr`. The caller
  (outer deserialize in §5.3) observes a null map and propagates the
  failure to the top-level return. No partial maps are kept. The
  exception is not rethrown across the C ABI boundary — `OREO_C_CATCH_RETURN`
  would otherwise translate it to `OREO_INTERNAL_ERROR`, which would mask
  the more specific `DESERIALIZE_FAILED` signal.
- **Warning dedup scope (M5).** At most one
  `LEGACY_IDENTITY_DOWNGRADE` diagnostic fires per top-level
  `oreo_deserialize` (or `oreo_ctx_deserialize`) call, across all nested
  `ElementMap` levels. Implementation: a thread-local `bool
  alreadyWarnedThisBuffer` flag set by the outermost reader and reset in
  its RAII scope exit. Inside `decodeV1Scalar`, the `DiagnosticCollector*`
  sink layer checks the flag before emitting — this keeps §3.2's contract
  (always emit on Case B) formally intact while §5.4's per-buffer
  coalescing lives in the diag sink, not in the decode function. Alternate
  implementation: the caller passes a coalescing wrapper around the
  real `DiagnosticCollector`. Decision deferred to Phase 4 — either is
  correct; whichever is simpler at that point wins. This is the only
  per-call state on the reader — everything else is stateless.

### 5.5 Fuzz corpus

Phase 7 re-runs the existing `test_fuzz` corpus against the v3 reader.
Additionally seed the corpus with:

- v1 outer + v2 inner (the current production format on this branch).
- v1 outer + v1 inner (pre-audit format) — expected: rejected.
- v3 outer + v2 inner — expected: accepted with one warning.
- Byte-truncated v3 buffers at every field boundary.

---

## 6. C ABI

### 6.1 `OreoShapeId`

```c
// include/oreo_kernel.h

typedef struct {
    uint64_t document_id;
    uint64_t counter;
} OreoShapeId;

/* The ABI layout checks require C11. Guard them so C89/C99 consumers
 * (Python cffi on some older builds, certain embedded toolchains) can
 * still include this header. The kernel's own TUs are C11+ and will
 * still enforce the invariant; the guarded assertions are belt-and-
 * suspenders for consumers. (Advisor review M3.) */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(OreoShapeId) == 16, "OreoShapeId ABI: size 16");
_Static_assert(_Alignof(OreoShapeId) == 8, "OreoShapeId ABI: align 8");
#endif
```

Layout matches `oreo::ShapeIdentity` exactly so we can `memcpy` across the
FFI boundary. No endianness wrapping — the struct is always read/written in
host byte order by both sides (serialization is a separate layer).

**Consumer C-standard impact:** none. C89/C99 consumers continue to work;
the asserts are skipped. The kernel's own TUs compile as C11+ (already
true — MSVC `/std:c11` or newer, GCC `-std=c11` or newer), so the
invariant is still enforced inside the kernel. An unguarded C++20 version
of the same check also lives in `shape_identity.h` as a belt-and-braces
check in the C++ surface.

### 6.2 New C API surface

```c
// Identity accessors (both always available — no OREO_ENABLE_LEGACY_API gate).
//
// Thread-safety (N5): same as all ctx-bound APIs — one OreoContext per
// thread. Concurrent calls with the same ctx are undefined. Concurrent
// calls with DIFFERENT ctx arguments are safe.
//
// Returns 0 on success, non-zero OreoErrorCode on failure. `*out` is set
// to {0, 0} on failure.
OREO_API int oreo_face_shape_id(OreoSolid s, int index, OreoShapeId* out);
OREO_API int oreo_edge_shape_id(OreoSolid s, int index, OreoShapeId* out);

// Context-bound name accessors — the no-legacy replacements for
// oreo_face_name / oreo_edge_name. Caller owns `buf`; `buflen` is the
// buffer size in bytes (including space for the NUL terminator).
//
// Protocol: pass (buf=NULL, buflen=0) to size-probe. *needed is set to
// the number of bytes required (excluding NUL). Then allocate buflen >=
// *needed + 1 and call again. Returns 0 on success, non-zero OreoErrorCode
// on failure. On buflen-too-small, writes as much as fits + NUL, sets
// *needed, and returns OREO_BUFFER_TOO_SMALL (new error code).
//
// Name lengths are bounded by kernel-internal limits (< 2^31 bytes in
// practice), so size_t for *needed is sufficient.
OREO_API int oreo_ctx_face_name(OreoContext ctx, OreoSolid s, int index,
                                 char* buf, size_t buflen, size_t* needed);
OREO_API int oreo_ctx_edge_name(OreoContext ctx, OreoSolid s, int index,
                                 char* buf, size_t buflen, size_t* needed);

// Also add ctx-aware serialize/deserialize — the new test suite in Phase 6
// cannot use oreo_serialize / oreo_deserialize (legacy-gated).
//
// Return-type note (N4): serialized buffers can legitimately exceed 2 GB
// for large assemblies, so the `int` return used by oreo_serialize (via
// size_t* out-param) is kept here — the function returns an OreoErrorCode
// and writes the byte count to *needed. An `int`-valued byte count would
// overflow at 2 GB.
//
// Thread-safety: one OreoContext per thread (see above).
OREO_API int oreo_ctx_serialize  (OreoContext ctx, OreoSolid s,
                                   uint8_t* buf, size_t buflen,
                                   size_t* needed);
OREO_API OreoSolid oreo_ctx_deserialize(OreoContext ctx,
                                         const uint8_t* data, size_t len);
```

`oreo_face_name` / `oreo_edge_name` remain under `#ifdef OREO_ENABLE_LEGACY_API`
unchanged. They internally call `oreo_ctx_face_name` against
`internalDefaultCtx()` and copy into the thread-local buffer for back-compat.

### 6.3 ABI stability tests

Two new tests:
1. `static_assert` in a C translation unit that the struct size and
   alignment match expectations on MSVC, GCC, Clang.
2. A DLL-boundary round-trip (produce `OreoShapeId` in
   `oreo-kernel.dll`, read it back from a caller in a separate TU compiled
   with a different compiler on CI) — covered in Phase 6.

---

## 7. API Status Matrix

Legend: v1-only = deprecated, kept for source-compat. both = accepts v1 and
v2 inputs. v2-only = no v1 fallback; will reject v1 inputs.

| Surface                                           | v1 | v2 | Notes |
|---------------------------------------------------|:--:|:--:|-------|
| `TagAllocator::nextTag() / peek() / allocateRange() / encodeTag()` | ✓* | — | `[[deprecated]]`; internally calls v2 then `encodeV1Scalar`. |
| `TagAllocator::extractDocumentId / extractCounter` | ✓* | — | `[[deprecated]]`; returns 32-bit halves of the v1 squeeze. Callers migrate to `ShapeIdentity` fields. |
| `TagAllocator::nextShapeIdentity / peekShapeIdentity / allocateRangeV2` | — | ✓ | Primary path. Counter is 64-bit; no UINT32_MAX cap. |
| `TagAllocator::toOcctTag(int64_t)` | ✓* | — | `[[deprecated]]`; replaced by `toOcctTag(ShapeIdentity)` which maps the whole identity. |
| `TagAllocator::Snapshot { counter:int64, docId:uint64 }` | ✓* | — | `[[deprecated]]`; struct replaced by `SnapshotV2`. |
| `TagAllocator::snapshot() / restore(Snapshot, force)`     | ✓* | — | `[[deprecated]]`; internally call `snapshotV2` / `restoreV2` and narrow. Narrowing throws if `counter > INT64_MAX`. |
| `TagAllocator::SnapshotV2 { counter:uint64, docId:uint64 }` | — | ✓ | New. Counter widened to 64-bit matching `ShapeIdentity::counter`. |
| `TagAllocator::snapshotV2() / restoreV2(SnapshotV2, force)` | — | ✓ | New. Primary path for clone/rollback. |
| `MappedName::appendTag(int64_t)` (`;:H`) | ✓* | — | `[[deprecated]]`; parsers still read `;:H`. |
| `MappedName::appendShapeIdentity(ShapeIdentity)` (`;:P`) | — | ✓ | Primary path. |
| `MappedName::appendDocumentId(uint64_t)` (`;:Q`) | ✓ | ✓ | Kept — top-level docId stamp is still useful. |
| `ElementMap::extractTag(name) -> int64_t` | ✓ | ✓ | Returns `encodeV1Scalar(extractShapeIdentity(name))`. Serialization-only. |
| `ElementMap::extractShapeIdentity(name)` | — | ✓ | Primary path. |
| `ElementMap::FORMAT_VERSION` | `2` (rejected) | `3` | v1 (`1`) already rejected; v2 (`2`) reader kept for upgrade. |
| `ChildElementMap::tag : int64_t`                  | ✓* | — | Replaced by `id : ShapeIdentity`. No compat — it's a struct field. Callers must migrate at Phase 3. |
| `HistoryTraceItem::tag : int64_t`                 | ✓* | — | Same as above. |
| `NamedShape::tag() -> int64_t`                    | ✓* | — | `[[deprecated]]`; new `shapeId() -> ShapeIdentity`. |
| `oreo_serialize / oreo_deserialize` (C)           | ✓* | — | Legacy-gated; kept. Write v3, read v1/v2/v3. |
| `oreo_ctx_serialize / oreo_ctx_deserialize` (C)   | — | ✓ | New. Always available. |
| `oreo_face_name / oreo_edge_name` (C)             | ✓* | — | Legacy-gated. Thin wrapper over `oreo_ctx_*_name`. |
| `oreo_ctx_face_name / oreo_ctx_edge_name` (C)     | — | ✓ | New. Always available. Caller-owned buffer. |
| `oreo_face_shape_id / oreo_edge_shape_id` (C)     | — | ✓ | New. Always available. |

*`✓*` = present but `[[deprecated]]` / `OREO_DEPRECATED`; CI-enforced grep-gate
prevents **new** callers.

---

## 8. Open questions for advisor review

1. **`;:I` postfix conflict.** ~~Pick `;:P` / `;:Y` / `;:Z` / other.~~
   **RESOLVED: `;:P` shipped.** The encoded postfix is
   `;:P<16hex>.<16hex>`. `;:I` remains `POSTFIX_INDEX` unchanged.

2. **Diagnostic channel for `LEGACY_IDENTITY_DOWNGRADE`.** Add a new
   `ErrorCode` enum value (bumps the public `OreoErrorCode` enum — source-
   compatible, but callers matching on specific values may need a catch-all
   default). OK or shall we reuse an existing code with a specific
   message prefix? Four new codes are now proposed: `LEGACY_IDENTITY_DOWNGRADE`,
   `V2_IDENTITY_NOT_REPRESENTABLE` (§4.3 rule 5), `MALFORMED_ELEMENT_NAME`
   (§4.3 rule 4d), and `BUFFER_TOO_SMALL` (§6.2 size-probe). Confirm all
   four are acceptable, or collapse.

3. **Hash algorithm for `std::hash<ShapeIdentity>`.** Boost `hash_combine`
   (fast, well-known, tolerable entropy) vs. XXH3 (better distribution,
   pulls in a tiny header). Either is invisible at ABI level.

4. ~~**`TagAllocator::toOcctTag` mapping strategy for v2.**~~ **Resolved (N7):**
   use `std::unordered_map<ShapeIdentity, int32_t>` keyed by `ShapeIdentity`
   with the hash from §2.3. No further discussion needed.

5. **Version-byte unification (1 → 3 on the outer buffer, skipping 2).**
   Any preference for keeping the outer format version independent of
   `ElementMap::FORMAT_VERSION` instead?

6. **Fixed-width vs. trimmed-hex `;:P`.** Current design: 16 + 16 lowercase,
   always. Trimming leading zeros saves bytes on small identities but
   complicates parsing and breaks determinism goldens. Recommendation:
   keep fixed-width. Confirm?

7. **Size overhead tolerance.** Phase 7 enforces a 20% regression gate on
   name-compare / serialize / `ElementMap::insert`. Is that the right
   bound, or should the gate be tighter (e.g., 10%) given how foundational
   this layer is?

8. **CI matrix — `OREO_ENABLE_LEGACY_API=OFF` job.** Per advisor N8, this
   needs a named owner before the doc merges. Local `build_nolegacy/`
   already exists, proving the build configures and compiles; what's
   missing is a CI job. **Owner: TBD — tech lead to assign before this
   doc merges.** The Phase 6 merge is blocked until this is named;
   Phases 1–5 are not. Candidate owners:

   - QA/tooling lead (advisor's table row for Phase 6) — most natural fit.
   - Foundation team lead — if QA bandwidth is constrained, foundation
     owns it through Phase 6 and hands off after green.

   Advisor: pick one, or name a third person.

---

## 9. What does *not* change

- `KernelContext` documentId semantics (already 64-bit since v1).
- `documentUUID` → `documentId` derivation (SipHash-2-4, unchanged).
- Process-global low-32 collision registry (`registerDocumentId`) — stays
  as a defense-in-depth even once every boundary carries full 64-bit
  identity, so legacy-format reads still surface collisions early.
- Determinism contract: same ops + same docId ⇒ same names. v2 widens the
  encoding; it does not change which ops produce which counters.
- Thread-safety contract on `KernelContext` and `NamedShape`.
- CMake options. `OREO_ENABLE_LEGACY_API=OFF` continues to work and becomes
  the Phase 6 test target.

---

## 10. Exit gate

This document is the Phase 0 deliverable. **Advisor sign-off required
before any Phase 1 code lands.** Sign-off form: one comment on the PR that
introduces this file, approving §§2–6 or requesting specific changes, and
answering each item in §8.
