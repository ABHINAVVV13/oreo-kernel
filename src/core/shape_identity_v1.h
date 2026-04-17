// SPDX-License-Identifier: LGPL-2.1-or-later

// shape_identity_v1.h — The ONLY sanctioned v1 ↔ v2 identity bridge.
//
// See docs/identity-model.md §§3.1–3.3. In short:
//
//   * encodeV1Scalar(ShapeIdentity) → int64 for v1 serialization paths;
//     throws on counter > UINT32_MAX or counter == 0.
//   * decodeV1Scalar(int64, hint, diag) → ShapeIdentity, with a four-case
//     table that owns all diagnostic emission for legacy reads.
//   * oreo::internal::squeezeV1DocId(uint64) — the only place in the tree
//     allowed to materially perform the v1 low-32-bit squeeze. CI
//     grep-gate enforces this; see §3.3.
//
// Callers outside this file and tag_allocator.{h,cpp} must not replicate
// the squeeze idiom. Use squeezeV1DocId() for any new boundary that
// legitimately needs it (there should be none outside tag_allocator).

#ifndef OREO_SHAPE_IDENTITY_V1_H
#define OREO_SHAPE_IDENTITY_V1_H

#include "shape_identity.h"

#include <cstdint>

namespace oreo {

class DiagnosticCollector;  // fwd — real decl in diagnostic.h

namespace internal {

// The ONE sanctioned materialization of the v1 squeeze. Returns the low
// 32 bits of a 64-bit documentId, which is all the v1 on-the-wire tag
// can carry. Callers outside encodeV1Scalar / tag_allocator must not
// write `docId & 0xFFFFFFFF` themselves — the CI grep-gate rejects it.
// See docs/identity-model.md §3.3.
constexpr std::uint32_t squeezeV1DocId(std::uint64_t documentId) noexcept {
    return static_cast<std::uint32_t>(documentId & 0xFFFFFFFFu);
}

// LegacyDowngradeDedupScope — RAII scope that coalesces Case B
// LEGACY_IDENTITY_DOWNGRADE warnings for a single top-level read of
// a legacy-format buffer. Inside one of these scopes, at most one
// downgrade warning fires regardless of how many per-entry decodes
// occur. Nestable: inner scopes are no-ops; the outermost scope owns
// the "already warned" flag, which resets at scope construction.
//
// Top-level deserializers (oreo_serialize.cpp::deserialize,
// oreo_ctx_deserialize) construct one of these on the stack for the
// duration of the read. See docs/identity-model.md §5.4.
class LegacyDowngradeDedupScope {
public:
    LegacyDowngradeDedupScope() noexcept;
    ~LegacyDowngradeDedupScope() noexcept;
    LegacyDowngradeDedupScope(const LegacyDowngradeDedupScope&) = delete;
    LegacyDowngradeDedupScope& operator=(const LegacyDowngradeDedupScope&) = delete;
};

// Internal query — used by decodeV1Scalar Case B to decide whether
// to forward a downgrade warning to the diag sink. Returns true if
// the caller should emit; returns false (and silently marks warned)
// if the warning has already fired within the active dedup scope.
// Always returns true when no dedup scope is active, so one-off
// callers outside the top-level deserializer still see every warning.
bool shouldEmitDowngradeWarning() noexcept;

} // namespace internal

// Encode a ShapeIdentity into a v1 int64 tag for serialization paths
// that still speak the old format.
//
//   Throws std::invalid_argument if id.counter == 0 — producers must
//     never emit an invalid identity, and the boundary enforces it.
//   Throws std::overflow_error  if id.counter > UINT32_MAX — the v1
//     tag only has 32 bits for the counter and the cliff is legitimate
//     at this boundary (it's what makes v1 lossy).
//
// Single-doc identities (documentId == 0) encode as `counter` directly —
// same as the v1 single-doc allocator output, preserving backward
// compatibility on format version 1 buffers.
//
// Multi-doc identities pack `squeezeV1DocId(documentId)` into the high
// 32 bits and `counter` into the low 32 bits. The high 32 bits of
// documentId are LOST at this boundary — that is what decodeV1Scalar's
// Case B flags as lossy on the read side.
std::int64_t encodeV1Scalar(ShapeIdentity id);

// Decode a v1 int64 tag into a ShapeIdentity, with explicit behavior
// across the three legitimate cases and one error case. See
// docs/identity-model.md §3.2 for the full case table.
//
//   Case A (high32(scalar) == 0, single-doc encoded):
//     Returns {fullDocIdHint, uint64(scalar)}. Never lossy, never warns.
//
//   Case B (high32(scalar) != 0, hint == 0):
//     Returns {high32(scalar), low32(scalar)}. High 32 bits of the
//     original documentId are IRRECOVERABLE and stay zero. Emits
//     ErrorCode::LEGACY_IDENTITY_DOWNGRADE on `diag` if supplied.
//
//   Case C (high32(scalar) != 0, hint matches — low32(hint) == high32(scalar)):
//     Returns {hint, low32(scalar)}. Lossless upgrade.
//
//   Error case (high32(scalar) != 0, hint nonzero but mismatched):
//     Throws std::invalid_argument. Always a bug: wrong hint, corrupt
//     buffer, or cross-document paste. Callers that catch this treat it
//     as a hard deserialization failure (see §5.3 reader semantics).
//
// The function never fabricates the high 32 bits of documentId from
// anything other than an explicit matching hint.
ShapeIdentity decodeV1Scalar(std::int64_t scalar,
                             std::uint64_t fullDocIdHint = 0,
                             DiagnosticCollector* diag = nullptr);

} // namespace oreo

#endif // OREO_SHAPE_IDENTITY_V1_H
