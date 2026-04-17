// SPDX-License-Identifier: LGPL-2.1-or-later

// shape_identity_v1.cpp — v1 bridge implementation.

#include "shape_identity_v1.h"

#include "diagnostic.h"

#include <limits>
#include <sstream>
#include <stdexcept>

namespace oreo {

// ─── LegacyDowngradeDedupScope thread-local state ────────────────────
//
// depth > 0   → a top-level deserializer has installed a dedup scope
// warned      → within the outermost scope, whether the LEGACY_IDENTITY
//               _DOWNGRADE warning has already fired once
//
// Nested scopes just bump the depth; only the outermost scope's
// destructor resets `warned`. This lets one read-path call another
// (e.g. inner ElementMap::deserialize invoked by oreo_serialize) with
// the dedup contract still intact.

namespace {
thread_local int    g_dedupDepth  = 0;
thread_local bool   g_dedupWarned = false;
}

namespace internal {

LegacyDowngradeDedupScope::LegacyDowngradeDedupScope() noexcept {
    if (g_dedupDepth == 0) {
        g_dedupWarned = false;
    }
    ++g_dedupDepth;
}

LegacyDowngradeDedupScope::~LegacyDowngradeDedupScope() noexcept {
    --g_dedupDepth;
    // Outermost scope destroying — reset the warned flag so an
    // immediately-following scope on the same thread doesn't inherit
    // stale "already warned" state.
    if (g_dedupDepth == 0) {
        g_dedupWarned = false;
    }
}

bool shouldEmitDowngradeWarning() noexcept {
    if (g_dedupDepth == 0) {
        // No scope active — not in a top-level deserialize, so fire
        // every warning. This matches the one-off caller contract.
        return true;
    }
    if (g_dedupWarned) {
        return false;
    }
    g_dedupWarned = true;
    return true;
}

} // namespace internal

std::int64_t encodeV1Scalar(ShapeIdentity id) {
    if (id.counter == 0) {
        throw std::invalid_argument(
            "encodeV1Scalar: identity has counter == 0 (invalid sentinel)");
    }

    // Single-doc: the scalar IS the counter, cast to int64_t. The cap
    // here is INT64_MAX (not UINT32_MAX) because v1 single-doc scalars
    // legitimately use the full 63-bit positive range — that's the
    // pre-audit nextTag() behavior in documentId=0 mode, preserved so
    // the refactored nextTag() (which routes through this function) is
    // backward-compatible.
    if (id.documentId == 0) {
        if (id.counter > static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
            std::ostringstream oss;
            oss << "encodeV1Scalar: single-doc counter " << id.counter
                << " exceeds INT64_MAX; the v1 scalar format cannot "
                   "represent it. Use the v3 writer.";
            throw std::overflow_error(oss.str());
        }
        return static_cast<std::int64_t>(id.counter);
    }

    // Multi-doc: the upper 32 bits carry documentId and the lower 32
    // bits carry the counter — the v1 squeeze. This caps the counter
    // at UINT32_MAX, which is the legitimate v1 cliff that makes the
    // format lossy in multi-doc mode.
    if (id.counter > static_cast<std::uint64_t>(
            std::numeric_limits<std::uint32_t>::max())) {
        std::ostringstream oss;
        oss << "encodeV1Scalar: multi-doc counter " << id.counter
            << " exceeds UINT32_MAX; this identity cannot be expressed "
               "in the v1 serialization format. Use the v3 writer.";
        throw std::overflow_error(oss.str());
    }

    // This is the ONLY sanctioned use of the squeeze outside
    // tag_allocator. See docs/identity-model.md §3.3.
    const std::uint64_t hi = static_cast<std::uint64_t>(
        internal::squeezeV1DocId(id.documentId)) << 32;
    const std::uint64_t lo = id.counter & 0xFFFFFFFFu;
    return static_cast<std::int64_t>(hi | lo);
}

ShapeIdentity decodeV1Scalar(std::int64_t scalar,
                             std::uint64_t fullDocIdHint,
                             DiagnosticCollector* diag) {
    const std::uint64_t uscalar = static_cast<std::uint64_t>(scalar);
    const std::uint32_t hi32    = static_cast<std::uint32_t>(uscalar >> 32);
    const std::uint32_t lo32    = static_cast<std::uint32_t>(uscalar & 0xFFFFFFFFu);

    // Case A: single-doc encoded scalar. Never lossy. The full 64-bit
    // value is the counter; documentId comes from the hint (which may
    // be 0, meaning "still single-doc in the reader's view").
    if (hi32 == 0) {
        return ShapeIdentity{fullDocIdHint, uscalar};
    }

    // Cases B/C/Error: multi-doc encoded scalar.
    if (fullDocIdHint == 0) {
        // Case B: low-32 preserved, high-32 irrecoverable. Emit the
        // downgrade warning so the caller surfaces the fact — but
        // coalesce via the thread-local dedup scope installed by the
        // top-level deserializer (docs/identity-model.md §5.4).
        if (diag && internal::shouldEmitDowngradeWarning()) {
            std::ostringstream msg;
            msg << "Legacy v1 identity read with no document hint — "
                << "high 32 bits of documentId are lost. "
                << "Encoded tag: 0x" << std::hex << uscalar
                << " → identity {0x" << std::hex << hi32
                << ", counter=" << std::dec << lo32 << "}. "
                << "Save the shape with the v3 writer to eliminate this "
                << "warning on future reads. (Further downgrade warnings "
                << "in this deserialize have been coalesced.)";
            diag->warning(ErrorCode::LEGACY_IDENTITY_DOWNGRADE, msg.str());
        }
        return ShapeIdentity{static_cast<std::uint64_t>(hi32),
                             static_cast<std::uint64_t>(lo32)};
    }

    // Hint is nonzero. Check for Case C vs. Error.
    const std::uint32_t hintLo = internal::squeezeV1DocId(fullDocIdHint);
    if (hintLo != hi32) {
        // Error case: mismatch is always a bug. Caller must investigate.
        std::ostringstream oss;
        oss << "decodeV1Scalar: hint/scalar mismatch — "
            << "low32(hint)=0x" << std::hex << hintLo
            << " does not match high32(scalar)=0x" << hi32
            << ". This indicates wrong context, tampered buffer, or a "
               "cross-document paste. No correct recovery preserves data "
               "integrity — refusing to decode.";
        throw std::invalid_argument(oss.str());
    }

    // Case C: lossless upgrade.
    return ShapeIdentity{fullDocIdHint, static_cast<std::uint64_t>(lo32)};
}

} // namespace oreo
