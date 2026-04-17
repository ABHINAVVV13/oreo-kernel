// SPDX-License-Identifier: LGPL-2.1-or-later

// shape_identity.h — First-class identity value for the v2 model.
//
// See docs/identity-model.md for the full design (§2). In short:
//
//   struct ShapeIdentity { uint64_t documentId; uint64_t counter; };
//
// is the canonical in-memory and on-disk identity type. Every layer that
// used to carry `int64_t tag` in v1 carries a ShapeIdentity in v2. The v1
// scalar tag still exists as an interop shape on the serialization and C
// API boundaries, bridged through the two functions in shape_identity_v1.h.
//
// This file has ZERO dependencies beyond <cstdint>, <functional>, and
// <string> so it can be included anywhere in the kernel without pulling
// in heavier headers. It must remain dependency-free — if you're tempted
// to include diagnostic.h or kernel_context.h here, push that logic into
// shape_identity_v1 instead.

#ifndef OREO_SHAPE_IDENTITY_H
#define OREO_SHAPE_IDENTITY_H

#include <cstdint>
#include <cstdio>
#include <functional>
#include <ostream>
#include <string>
#include <string_view>

namespace oreo {

// A ShapeIdentity pairs the (full 64-bit) document identity with a
// per-document sequence counter. The document identity lives in its own
// value so that tag-carrying boundaries survive the truncation the v1
// model imposed on 32-bit-long platforms.
struct ShapeIdentity {
    std::uint64_t documentId = 0;   // 0 → single-document / default mode
    std::uint64_t counter    = 0;   // 0 → invalid / unset sentinel (see I1)

    // I2: single-doc mode treats cross-doc equality as undefined. Callers
    // mixing single- and multi-doc identities are responsible for not
    // trusting documentId == 0 comparisons against live documents.
    constexpr bool isSingleDoc() const noexcept { return documentId == 0; }

    // I1: producers never emit counter == 0; consumers may treat
    // counter == 0 as "unset" without asserting. See docs/identity-model.md.
    constexpr bool isValid() const noexcept { return counter != 0; }

    // Canonical display form: "<16hex>.<16hex>", lowercase, zero-padded.
    // Used by logs, diagnostics, and the MappedName ;:P encoding (Phase 3).
    std::string toHex() const {
        char buf[34];
        // 16 hex + "." + 16 hex + NUL = 34
        std::snprintf(buf, sizeof(buf), "%016llx.%016llx",
                      static_cast<unsigned long long>(documentId),
                      static_cast<unsigned long long>(counter));
        return std::string(buf);
    }

    // Parse the canonical form. Returns ShapeIdentity{0,0} on malformed
    // input so callers can branch on isValid(); does not throw.
    // Accepts lowercase and uppercase hex; requires exactly 16 hex digits
    // before and after the dot, no leading "0x", no trailing whitespace.
    static inline ShapeIdentity fromHex(std::string_view s) noexcept {
        // Expected layout: exactly 33 chars, positions 0-15 hex, pos 16 '.', 17-32 hex.
        if (s.size() != 33 || s[16] != '.') return {};

        auto parseHex16 = [](std::string_view hex, std::uint64_t& out) noexcept -> bool {
            std::uint64_t v = 0;
            for (char c : hex) {
                std::uint64_t d;
                if      (c >= '0' && c <= '9') d = static_cast<std::uint64_t>(c - '0');
                else if (c >= 'a' && c <= 'f') d = static_cast<std::uint64_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') d = static_cast<std::uint64_t>(c - 'A' + 10);
                else return false;
                v = (v << 4) | d;
            }
            out = v;
            return true;
        };

        ShapeIdentity id;
        if (!parseHex16(s.substr(0, 16),  id.documentId)) return {};
        if (!parseHex16(s.substr(17, 16), id.counter))    return {};
        return id;
    }
};

// Layout invariant I4 — must hold for the ABI guarantees in docs/identity-model.md §6.1
static_assert(sizeof(ShapeIdentity) == 16,
              "ShapeIdentity ABI: size must be exactly 16 bytes");
static_assert(alignof(ShapeIdentity) == 8,
              "ShapeIdentity ABI: alignment must be exactly 8 bytes");

// ─── Equality / ordering ─────────────────────────────────────

// Field-wise equality. Two ShapeIdentities compare equal iff both
// documentId and counter match.
constexpr bool operator==(const ShapeIdentity& a, const ShapeIdentity& b) noexcept {
    return a.documentId == b.documentId && a.counter == b.counter;
}
constexpr bool operator!=(const ShapeIdentity& a, const ShapeIdentity& b) noexcept {
    return !(a == b);
}

// Lexicographic ordering on (documentId, counter) — matches what a
// defaulted C++20 operator<=> would produce. Single-doc identities sort
// before any multi-doc identity that shares counter, which keeps
// snapshot dumps stable across the v1 → v2 migration.
constexpr bool operator<(const ShapeIdentity& a, const ShapeIdentity& b) noexcept {
    return (a.documentId != b.documentId) ? (a.documentId < b.documentId)
                                          : (a.counter    < b.counter);
}
constexpr bool operator>(const ShapeIdentity& a, const ShapeIdentity& b) noexcept {
    return b < a;
}
constexpr bool operator<=(const ShapeIdentity& a, const ShapeIdentity& b) noexcept {
    return !(b < a);
}
constexpr bool operator>=(const ShapeIdentity& a, const ShapeIdentity& b) noexcept {
    return !(a < b);
}

// ─── Stream insertion ────────────────────────────────────────

inline std::ostream& operator<<(std::ostream& os, const ShapeIdentity& id) {
    return os << id.toHex();
}

} // namespace oreo

// ─── Hash specialization ─────────────────────────────────────
//
// Advisor Q3 was left open between Boost `hash_combine` and XXH3.
// Implementing the Boost combine here is header-only and sufficient for
// unordered_map keys. If profiling shows hash collisions on realistic
// workloads, we can swap the body without touching any caller — the hash
// is not ABI-visible.

template <>
struct std::hash<oreo::ShapeIdentity> {
    std::size_t operator()(const oreo::ShapeIdentity& id) const noexcept {
        // Standard Boost-style combine. Good enough for 64-bit keys;
        // better distribution than XOR alone.
        std::size_t h = std::hash<std::uint64_t>{}(id.documentId);
        h ^= std::hash<std::uint64_t>{}(id.counter)
             + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

#endif // OREO_SHAPE_IDENTITY_H
