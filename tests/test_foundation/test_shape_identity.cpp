// SPDX-License-Identifier: LGPL-2.1-or-later

// test_shape_identity.cpp — Phase 1 acceptance tests.
//
// Covers the ShapeIdentity value type (§2) and the v1 bridge (§§3.1–3.2)
// from docs/identity-model.md. The v1 bridge tests exercise the four-case
// table (A/B/C + Error) exhaustively so future refactors can't regress it
// silently.

#include "core/shape_identity.h"
#include "core/shape_identity_v1.h"
#include "core/diagnostic.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>

using oreo::ShapeIdentity;
using oreo::decodeV1Scalar;
using oreo::encodeV1Scalar;
using oreo::DiagnosticCollector;
using oreo::ErrorCode;

// ─── Type / ABI layout ──────────────────────────────────────────────

TEST(ShapeIdentity, AbiLayoutIsStable) {
    // docs/identity-model.md I4: size 16, align 8. Producers rely on
    // memcpy across the OreoShapeId FFI boundary.
    static_assert(sizeof(ShapeIdentity) == 16, "size must be 16");
    static_assert(alignof(ShapeIdentity) == 8, "align must be 8");
    static_assert(std::is_trivially_copyable_v<ShapeIdentity>,
                  "must be trivially copyable for FFI memcpy");
    SUCCEED();
}

TEST(ShapeIdentity, DefaultIsInvalidSingleDoc) {
    ShapeIdentity id{};
    EXPECT_TRUE(id.isSingleDoc());
    EXPECT_FALSE(id.isValid());
    EXPECT_EQ(id.documentId, 0u);
    EXPECT_EQ(id.counter,    0u);
}

TEST(ShapeIdentity, IsValidTracksCounterOnly) {
    // I1: counter == 0 is the invalid sentinel. documentId is irrelevant.
    EXPECT_FALSE((ShapeIdentity{0xDEAD, 0}).isValid());
    EXPECT_TRUE((ShapeIdentity{0,      1}).isValid());
    EXPECT_TRUE((ShapeIdentity{0xDEAD, 1}).isValid());
}

// ─── Equality / ordering ────────────────────────────────────────────

TEST(ShapeIdentity, EqualityIsFieldwise) {
    EXPECT_EQ((ShapeIdentity{1, 2}), (ShapeIdentity{1, 2}));
    EXPECT_NE((ShapeIdentity{1, 2}), (ShapeIdentity{1, 3}));
    EXPECT_NE((ShapeIdentity{1, 2}), (ShapeIdentity{2, 2}));
    EXPECT_NE((ShapeIdentity{0, 1}), (ShapeIdentity{1, 0}));
}

TEST(ShapeIdentity, OrderingIsLexDocumentThenCounter) {
    EXPECT_LT((ShapeIdentity{0, 1}), (ShapeIdentity{0, 2}));
    EXPECT_LT((ShapeIdentity{0, 999}), (ShapeIdentity{1, 0}));
    EXPECT_LT((ShapeIdentity{0xAAAA, 5}), (ShapeIdentity{0xAAAB, 1}));
    EXPECT_FALSE((ShapeIdentity{1, 1}) < (ShapeIdentity{1, 1}));
    EXPECT_LE((ShapeIdentity{1, 1}), (ShapeIdentity{1, 1}));
    EXPECT_GE((ShapeIdentity{2, 1}), (ShapeIdentity{1, 999}));
    EXPECT_GT((ShapeIdentity{2, 1}), (ShapeIdentity{1, 999}));
}

// ─── Hashing ────────────────────────────────────────────────────────

TEST(ShapeIdentity, HashIsStableAndDistinguishes) {
    std::hash<ShapeIdentity> h;
    EXPECT_EQ(h(ShapeIdentity{1, 2}), h(ShapeIdentity{1, 2}));
    EXPECT_NE(h(ShapeIdentity{1, 2}), h(ShapeIdentity{2, 1}));
    // docId 0 + counter N must hash differently than docId N + counter 0.
    // (XOR alone would collapse them — the combine prevents that.)
    EXPECT_NE(h(ShapeIdentity{0, 7}), h(ShapeIdentity{7, 0}));
}

TEST(ShapeIdentity, UsableAsUnorderedSetKey) {
    // Smoke test that the hash specialization is wired correctly for STL.
    std::unordered_set<ShapeIdentity> s;
    s.insert({1, 1});
    s.insert({1, 1});
    s.insert({1, 2});
    s.insert({2, 1});
    EXPECT_EQ(s.size(), 3u);
    EXPECT_EQ(s.count({1, 1}), 1u);
    EXPECT_EQ(s.count({3, 3}), 0u);
}

// ─── toHex / fromHex round-trip ─────────────────────────────────────

TEST(ShapeIdentity, ToHexIsFixedWidth) {
    // Fixed 16+16 width is a design decision (§2.4) for golden determinism.
    EXPECT_EQ((ShapeIdentity{0, 0}).toHex(),
              "0000000000000000.0000000000000000");
    EXPECT_EQ((ShapeIdentity{1, 1}).toHex(),
              "0000000000000001.0000000000000001");
    EXPECT_EQ((ShapeIdentity{0xDEADBEEFCAFEBABEULL, 0x42}).toHex(),
              "deadbeefcafebabe.0000000000000042");
}

TEST(ShapeIdentity, FromHexRoundTripsEveryBitPattern) {
    for (auto id : {
             ShapeIdentity{0, 0},
             ShapeIdentity{0, 1},
             ShapeIdentity{1, 0},
             ShapeIdentity{0xDEADBEEFCAFEBABEULL, 0x42},
             ShapeIdentity{0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL},
             ShapeIdentity{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL},
         }) {
        const auto hex = id.toHex();
        const auto back = ShapeIdentity::fromHex(hex);
        EXPECT_EQ(back, id) << "round-trip failed for " << hex;
    }
}

TEST(ShapeIdentity, FromHexRejectsMalformedInput) {
    // Wrong length
    EXPECT_EQ(ShapeIdentity::fromHex(""), (ShapeIdentity{}));
    EXPECT_EQ(ShapeIdentity::fromHex("short"), (ShapeIdentity{}));
    // Missing dot
    EXPECT_EQ(ShapeIdentity::fromHex("0000000000000000X0000000000000000"),
              (ShapeIdentity{}));
    // Non-hex char
    EXPECT_EQ(ShapeIdentity::fromHex("000000000000000z.0000000000000000"),
              (ShapeIdentity{}));
    // Leading 0x not allowed
    EXPECT_EQ(ShapeIdentity::fromHex("0x00000000000000.0000000000000000"),
              (ShapeIdentity{}));
}

TEST(ShapeIdentity, FromHexAcceptsUppercase) {
    EXPECT_EQ(ShapeIdentity::fromHex("DEADBEEFCAFEBABE.0000000000000042"),
              (ShapeIdentity{0xDEADBEEFCAFEBABEULL, 0x42}));
}

// ─── encodeV1Scalar ─────────────────────────────────────────────────

TEST(EncodeV1Scalar, SingleDocReturnsCounterAsIs) {
    // documentId == 0 → scalar == counter. Preserves v1 single-doc format.
    EXPECT_EQ(encodeV1Scalar({0, 1}),          1);
    EXPECT_EQ(encodeV1Scalar({0, 0xFFFFFFFF}), 0xFFFFFFFF);
}

TEST(EncodeV1Scalar, MultiDocSqueezesDocIdIntoHigh32) {
    const auto scalar = encodeV1Scalar({0xDEADBEEFCAFEBABEULL, 0x42});
    // high32 = low32(docId) = 0xCAFEBABE; low32 = counter = 0x42.
    EXPECT_EQ(static_cast<std::uint64_t>(scalar),
              (static_cast<std::uint64_t>(0xCAFEBABE) << 32) | 0x42);
}

TEST(EncodeV1Scalar, RejectsInvalidCounter) {
    EXPECT_THROW(encodeV1Scalar({0,      0}), std::invalid_argument);
    EXPECT_THROW(encodeV1Scalar({0xABCD, 0}), std::invalid_argument);
}

TEST(EncodeV1Scalar, MultiDocOverflowsOnCounterAboveUint32Max) {
    // v1 multi-doc cap — counter shares the scalar with docId, so 32 bits.
    const std::uint64_t overflow =
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1;
    EXPECT_THROW(encodeV1Scalar({1, overflow}), std::overflow_error);
}

TEST(EncodeV1Scalar, SingleDocPermitsCounterAboveUint32Max) {
    // Single-doc mode uses the full 63-bit positive range — pre-audit
    // nextTag() in docId=0 mode did this, so the refactored nextTag()
    // must match.
    const std::uint64_t above32 = 0x100000000ull;  // 2^32
    EXPECT_NO_THROW((void)encodeV1Scalar({0, above32}));
    EXPECT_EQ(encodeV1Scalar({0, above32}),
              static_cast<std::int64_t>(above32));
}

TEST(EncodeV1Scalar, SingleDocOverflowsAboveInt64Max) {
    // Signed int64 is the v1 scalar type; counter above INT64_MAX has
    // no representation.
    const std::uint64_t overflow =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1;
    EXPECT_THROW(encodeV1Scalar({0, overflow}), std::overflow_error);
}

TEST(EncodeV1Scalar, BoundaryCountersAccepted) {
    const std::uint64_t u32Max = std::numeric_limits<std::uint32_t>::max();
    EXPECT_NO_THROW((void)encodeV1Scalar({0,    u32Max}));
    EXPECT_NO_THROW((void)encodeV1Scalar({0xAB, u32Max}));
    // Single-doc: INT64_MAX is the ceiling, and it's accepted.
    const std::uint64_t i64Max =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    EXPECT_NO_THROW((void)encodeV1Scalar({0, i64Max}));
}

// ─── decodeV1Scalar — Case A (single-doc scalar) ────────────────────

TEST(DecodeV1Scalar, CaseASingleDocZeroHint) {
    // high32(scalar) == 0 → counter = scalar, docId = hint (0 here).
    auto id = decodeV1Scalar(42);
    EXPECT_EQ(id, (ShapeIdentity{0, 42}));
}

TEST(DecodeV1Scalar, CaseASingleDocWithHintAttachesDocId) {
    auto id = decodeV1Scalar(42, 0xDEADBEEFCAFEBABEULL);
    EXPECT_EQ(id, (ShapeIdentity{0xDEADBEEFCAFEBABEULL, 42}));
}

TEST(DecodeV1Scalar, CaseANeverFiresDiagnostic) {
    DiagnosticCollector diag;
    (void)decodeV1Scalar(42, 0, &diag);
    (void)decodeV1Scalar(42, 0xAAAA, &diag);
    EXPECT_FALSE(diag.hasWarnings()) << "Case A must be silent";
    EXPECT_FALSE(diag.hasErrors());
}

// ─── decodeV1Scalar — Case B (multi-doc scalar, no hint) ────────────

TEST(DecodeV1Scalar, CaseBNoHintPreservesLow32AndFlags) {
    DiagnosticCollector diag;
    // Scalar = 0xCAFEBABE.00000042 (high32 = low32(original docId)).
    const std::int64_t scalar =
        static_cast<std::int64_t>((static_cast<std::uint64_t>(0xCAFEBABE) << 32) | 0x42);
    auto id = decodeV1Scalar(scalar, 0, &diag);
    EXPECT_EQ(id, (ShapeIdentity{0xCAFEBABE, 0x42}));
    EXPECT_TRUE(diag.hasWarnings())
        << "Case B must emit LEGACY_IDENTITY_DOWNGRADE";
    // Should NOT appear as an error — it's a warning.
    EXPECT_FALSE(diag.hasErrors());
    auto warns = diag.warnings();
    ASSERT_EQ(warns.size(), 1u);
    EXPECT_EQ(warns[0].code, ErrorCode::LEGACY_IDENTITY_DOWNGRADE);
}

TEST(DecodeV1Scalar, CaseBWithoutDiagSinkStillReturnsValue) {
    // The diag pointer is optional — absence must not cause a different
    // return value; it's a silence, not a cancel.
    const std::int64_t scalar =
        static_cast<std::int64_t>((static_cast<std::uint64_t>(0xCAFEBABE) << 32) | 0x42);
    auto id = decodeV1Scalar(scalar, 0, nullptr);
    EXPECT_EQ(id, (ShapeIdentity{0xCAFEBABE, 0x42}));
}

// ─── decodeV1Scalar — Case C (matching hint) ────────────────────────

TEST(DecodeV1Scalar, CaseCMatchingHintReconstructsFullDocId) {
    DiagnosticCollector diag;
    const std::uint64_t fullDocId = 0xDEADBEEFCAFEBABEULL;
    const std::int64_t scalar = encodeV1Scalar({fullDocId, 0x42});
    auto id = decodeV1Scalar(scalar, fullDocId, &diag);
    EXPECT_EQ(id, (ShapeIdentity{fullDocId, 0x42}));
    EXPECT_FALSE(diag.hasWarnings()) << "Case C must be silent";
    EXPECT_FALSE(diag.hasErrors());
}

TEST(DecodeV1Scalar, CaseCRoundTripsEncodeDecodeLossless) {
    // Fuzz-style — a handful of docIds, all small counters. Must round-trip.
    const std::uint64_t docIds[] = {
        0x0000000100000001ULL,  // low32 non-equal to high32
        0xAAAAAAAABBBBBBBBULL,
        0xDEADBEEFCAFEBABEULL,
    };
    for (auto docId : docIds) {
        for (std::uint64_t ctr : {1u, 2u, 1000u, 0xFFFFFFFEu, 0xFFFFFFFFu}) {
            ShapeIdentity in{docId, ctr};
            auto scalar = encodeV1Scalar(in);
            auto out    = decodeV1Scalar(scalar, docId);
            EXPECT_EQ(in, out) << "round-trip failed for " << in.toHex();
        }
    }
}

// ─── decodeV1Scalar — Error case (mismatch throws) ──────────────────

TEST(DecodeV1Scalar, ErrorCaseMismatchingHintThrows) {
    const std::int64_t scalar =
        static_cast<std::int64_t>((static_cast<std::uint64_t>(0xCAFEBABE) << 32) | 0x42);
    // Hint has DIFFERENT low 32 bits than the scalar's high 32 bits.
    EXPECT_THROW(decodeV1Scalar(scalar, 0x1111111122222222ULL),
                 std::invalid_argument);
}

TEST(DecodeV1Scalar, ErrorCaseNeverEmitsDiagBeforeThrow) {
    // Mismatch is a bug, not a warning. The diag sink must not receive a
    // downgrade warning that the caller's catch handler would then have
    // to suppress.
    DiagnosticCollector diag;
    const std::int64_t scalar =
        static_cast<std::int64_t>((static_cast<std::uint64_t>(0xCAFEBABE) << 32) | 0x42);
    EXPECT_THROW(decodeV1Scalar(scalar, 0x1111111122222222ULL, &diag),
                 std::invalid_argument);
    EXPECT_FALSE(diag.hasWarnings())
        << "Error case must throw without first emitting a warning";
    EXPECT_FALSE(diag.hasErrors());
}

TEST(DecodeV1Scalar, MatchingHighHighDocIdBitsStillMatches) {
    // Hint differs from (reconstructed) docId in the high 32 bits only —
    // that IS Case C, since v1 only carries low 32. No throw.
    const std::uint64_t hint = 0xAAAABBBBCAFEBABEULL;  // low32 = 0xCAFEBABE
    const std::int64_t scalar =
        static_cast<std::int64_t>((static_cast<std::uint64_t>(0xCAFEBABE) << 32) | 0x42);
    auto id = decodeV1Scalar(scalar, hint);
    EXPECT_EQ(id, (ShapeIdentity{hint, 0x42}));  // high-32 upgrade succeeds
}

// ─── Squeeze helper is the only sanctioned use ──────────────────────

TEST(SqueezeV1DocId, IsTheLow32Bits) {
    EXPECT_EQ(oreo::internal::squeezeV1DocId(0), 0u);
    EXPECT_EQ(oreo::internal::squeezeV1DocId(0xCAFEBABE), 0xCAFEBABEu);
    EXPECT_EQ(oreo::internal::squeezeV1DocId(0xDEADBEEFCAFEBABEULL),
              0xCAFEBABEu);
    // It is constexpr-evaluable.
    static_assert(oreo::internal::squeezeV1DocId(0x1234567890ABCDEFULL)
                  == 0x90ABCDEFu);
}
