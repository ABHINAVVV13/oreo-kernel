// test_tag_allocator_v2.cpp — Phase 2 acceptance tests for the v2
// TagAllocator API (nextShapeIdentity, peekShapeIdentity, allocateRangeV2,
// SnapshotV2, snapshotV2/restoreV2, toOcctTag(ShapeIdentity)).
//
// Also verifies that the deprecated v1 methods remain behaviorally
// consistent with the v2 path so existing callers keep working during
// the migration. Call sites in this file are wrapped in MSVC /W4 C4996
// suppression because exercising the deprecated surface is deliberate
// test content.

#include "core/shape_identity.h"
#include "core/shape_identity_v1.h"
#include "core/tag_allocator.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>

// Scoped suppression for the deprecated v1 API calls in this file.
#if defined(_MSC_VER)
#  define OREO_DEPRECATED_BEGIN \
       __pragma(warning(push))  \
       __pragma(warning(disable : 4996))
#  define OREO_DEPRECATED_END __pragma(warning(pop))
#elif defined(__GNUC__) || defined(__clang__)
#  define OREO_DEPRECATED_BEGIN \
       _Pragma("GCC diagnostic push") \
       _Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")
#  define OREO_DEPRECATED_END _Pragma("GCC diagnostic pop")
#else
#  define OREO_DEPRECATED_BEGIN
#  define OREO_DEPRECATED_END
#endif

using oreo::ShapeIdentity;
using oreo::TagAllocator;

// ─── nextShapeIdentity primary path ─────────────────────────────────

TEST(TagAllocatorV2, NextShapeIdentitySingleDoc) {
    TagAllocator alloc;
    EXPECT_EQ(alloc.nextShapeIdentity(), (ShapeIdentity{0, 1}));
    EXPECT_EQ(alloc.nextShapeIdentity(), (ShapeIdentity{0, 2}));
    EXPECT_EQ(alloc.nextShapeIdentity(), (ShapeIdentity{0, 3}));
}

TEST(TagAllocatorV2, NextShapeIdentityMultiDoc) {
    TagAllocator::clearDocumentIdRegistry();
    const std::uint64_t docId = 0xDEADBEEFCAFEBABEull;
    TagAllocator alloc(docId);
    EXPECT_EQ(alloc.nextShapeIdentity(), (ShapeIdentity{docId, 1}));
    EXPECT_EQ(alloc.nextShapeIdentity(), (ShapeIdentity{docId, 2}));
    // Full 64-bit docId survives — not squeezed to 32 bits.
    auto id3 = alloc.nextShapeIdentity();
    EXPECT_EQ(id3.documentId, docId);
}

TEST(TagAllocatorV2, V2PathRemovesMultiDocUint32Cap) {
    // The v1 nextTag() threw std::overflow_error once counter > UINT32_MAX
    // in multi-doc mode. The v2 path must NOT inherit that cap.
    TagAllocator::clearDocumentIdRegistry();
    TagAllocator alloc(0xAAAAAAAABBBBBBBBull);

    // Seed the counter just below the v1 cap, then cross it via v2.
    alloc.seed(static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()) - 1);
    EXPECT_NO_THROW((void)alloc.nextShapeIdentity());   // counter = u32max
    EXPECT_NO_THROW((void)alloc.nextShapeIdentity());   // counter = u32max + 1
    auto above = alloc.peekShapeIdentity();
    EXPECT_GT(above.counter,
              static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()));
}

TEST(TagAllocatorV2, V1PathKeepsMultiDocUint32Cap) {
    // The deprecated nextTag() must still throw at UINT32_MAX — that
    // cliff is what makes v1 lossy, and callers depend on it.
    TagAllocator::clearDocumentIdRegistry();
    TagAllocator alloc(0xAAAAAAAABBBBBBBBull);
    alloc.seed(static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()));
    OREO_DEPRECATED_BEGIN
    EXPECT_THROW((void)alloc.nextTag(), std::overflow_error);
    OREO_DEPRECATED_END
}

// ─── peek doesn't advance ───────────────────────────────────────────

TEST(TagAllocatorV2, PeekDoesNotAdvance) {
    TagAllocator alloc;
    auto peek1 = alloc.peekShapeIdentity();
    auto peek2 = alloc.peekShapeIdentity();
    EXPECT_EQ(peek1, peek2);  // didn't advance
    auto next = alloc.nextShapeIdentity();
    EXPECT_EQ(next, peek1);   // peek matched the subsequent allocation
}

// ─── allocateRangeV2 ────────────────────────────────────────────────

TEST(TagAllocatorV2, AllocateRangeV2ReturnsHalfOpen) {
    TagAllocator alloc;
    auto [first, endExclusive] = alloc.allocateRangeV2(5);
    EXPECT_EQ(first, (ShapeIdentity{0, 1}));
    EXPECT_EQ(endExclusive, (ShapeIdentity{0, 6}));  // [1, 6) = 5 elements
    // Next allocation starts at 6.
    EXPECT_EQ(alloc.nextShapeIdentity(), (ShapeIdentity{0, 6}));
}

TEST(TagAllocatorV2, AllocateRangeV2RejectsNonPositive) {
    TagAllocator alloc;
    EXPECT_THROW((void)alloc.allocateRangeV2(0),  std::invalid_argument);
    EXPECT_THROW((void)alloc.allocateRangeV2(-1), std::invalid_argument);
}

// ─── SnapshotV2 round-trip ──────────────────────────────────────────

TEST(TagAllocatorV2, SnapshotV2RoundTrips) {
    TagAllocator::clearDocumentIdRegistry();
    const std::uint64_t docId = 0x1234567890ABCDEFull;
    TagAllocator alloc(docId);
    (void)alloc.nextShapeIdentity();
    (void)alloc.nextShapeIdentity();
    (void)alloc.nextShapeIdentity();
    auto snap = alloc.snapshotV2();
    EXPECT_EQ(snap.counter, 3u);
    EXPECT_EQ(snap.documentId, docId);

    // Advance further.
    (void)alloc.nextShapeIdentity();
    (void)alloc.nextShapeIdentity();

    // Restore with force=true (we're rolling back, which is the legitimate
    // use case for restore).
    alloc.restoreV2(snap, /*force=*/true);
    EXPECT_EQ(alloc.peekShapeIdentity(), (ShapeIdentity{docId, 4}));
}

TEST(TagAllocatorV2, RestoreV2RejectsTimeTravelWithoutForce) {
    // The invariant is: without `force`, you cannot roll BACK from a
    // higher current to a lower snapshot (doing so would re-issue
    // identities that were already handed out). Rolling forward or
    // replaying at the snapshot point is allowed.
    TagAllocator alloc;
    (void)alloc.nextShapeIdentity();
    auto snap1 = alloc.snapshotV2();   // counter = 1
    (void)alloc.nextShapeIdentity();
    (void)alloc.nextShapeIdentity();   // counter = 3
    // Roll back 3 → 1 without force: rejected.
    EXPECT_THROW(alloc.restoreV2(snap1), std::logic_error);
    // Same restore with force succeeds.
    EXPECT_NO_THROW(alloc.restoreV2(snap1, /*force=*/true));
    EXPECT_EQ(alloc.peekShapeIdentity(), (ShapeIdentity{0, 2}));
}

TEST(TagAllocatorV2, RestoreV2RejectsAboveInt64Max) {
    TagAllocator alloc;
    TagAllocator::SnapshotV2 huge{
        /*counter=*/static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1,
        /*docId=*/0};
    EXPECT_THROW(alloc.restoreV2(huge, /*force=*/true), std::invalid_argument);
}

// ─── toOcctTag(ShapeIdentity) ───────────────────────────────────────

TEST(TagAllocatorV2, ToOcctTagSingleDocFastPath) {
    TagAllocator alloc;
    EXPECT_EQ(alloc.toOcctTag(ShapeIdentity{0, 1}),        1);
    EXPECT_EQ(alloc.toOcctTag(ShapeIdentity{0, 42}),       42);
    EXPECT_EQ(alloc.toOcctTag(ShapeIdentity{0, 0x7FFFFFFF}),
              std::numeric_limits<std::int32_t>::max());
}

TEST(TagAllocatorV2, ToOcctTagMultiDocMapsStably) {
    TagAllocator::clearDocumentIdRegistry();
    TagAllocator alloc(0xAAAAAAAABBBBBBBBull);
    auto id1 = alloc.nextShapeIdentity();
    auto id2 = alloc.nextShapeIdentity();
    auto t1a = alloc.toOcctTag(id1);
    auto t2a = alloc.toOcctTag(id2);
    auto t1b = alloc.toOcctTag(id1);
    auto t2b = alloc.toOcctTag(id2);
    EXPECT_EQ(t1a, t1b);       // stable for the same identity
    EXPECT_EQ(t2a, t2b);
    EXPECT_NE(t1a, t2a);       // distinct identities → distinct handles
    EXPECT_GT(t1a, 0);
    EXPECT_GT(t2a, 0);
}

TEST(TagAllocatorV2, ToOcctTagRejectsInvalidIdentity) {
    TagAllocator alloc;
    EXPECT_THROW((void)alloc.toOcctTag(ShapeIdentity{0, 0}),
                 std::invalid_argument);
}

TEST(TagAllocatorV2, ToOcctTagSingleDocCounterAboveInt32MaxGoesThroughMap) {
    TagAllocator alloc;
    // Single-doc counter above INT32_MAX: needs the map. Fast path only
    // applies when counter fits in int32.
    ShapeIdentity big{0, static_cast<std::uint64_t>(
        std::numeric_limits<std::int32_t>::max()) + 1};
    auto mapped = alloc.toOcctTag(big);
    EXPECT_GT(mapped, 0);
    // Stable under re-query.
    EXPECT_EQ(alloc.toOcctTag(big), mapped);
}

// ─── v1/v2 consistency under deprecation ────────────────────────────

TEST(TagAllocatorV2, LegacyNextTagMatchesEncodedV2) {
    // nextTag() is now `encodeV1Scalar(nextShapeIdentity())`. Verify the
    // byte-for-byte equivalence on both single- and multi-doc paths.
    TagAllocator::clearDocumentIdRegistry();
    TagAllocator singleDoc;
    TagAllocator multiDoc(0xCAFEBABEu);  // low32 only so encode is lossless

    OREO_DEPRECATED_BEGIN
    for (int i = 0; i < 5; ++i) {
        auto id       = singleDoc.peekShapeIdentity();
        auto scalar   = singleDoc.nextTag();
        EXPECT_EQ(scalar, oreo::encodeV1Scalar(id));
    }
    for (int i = 0; i < 5; ++i) {
        auto id       = multiDoc.peekShapeIdentity();
        auto scalar   = multiDoc.nextTag();
        EXPECT_EQ(scalar, oreo::encodeV1Scalar(id));
    }
    OREO_DEPRECATED_END
}

TEST(TagAllocatorV2, LegacyToOcctTagInt64MatchesV2) {
    // toOcctTag(scalar) should route to the same map entry as
    // toOcctTag(ShapeIdentity) for the matching identity on this
    // allocator. This is the consistency contract that callers of the
    // two overloads rely on during the migration.
    //
    // We pick a docId whose LOW 32 BITS have the high bit clear
    // (< 0x80000000u) so the v1 multi-doc encoding stays in the
    // positive int64_t range — the deprecated toOcctTag(int64_t)
    // rejects non-positive inputs, and that rejection is pre-existing
    // behavior we're preserving unchanged.
    TagAllocator::clearDocumentIdRegistry();
    TagAllocator alloc(0x0000000012345678ull);
    for (int i = 0; i < 8; ++i) {
        auto id = alloc.nextShapeIdentity();
        std::int64_t scalar = oreo::encodeV1Scalar(id);
        auto viaV2 = alloc.toOcctTag(id);
        OREO_DEPRECATED_BEGIN
        auto viaV1 = alloc.toOcctTag(scalar);
        OREO_DEPRECATED_END
        EXPECT_EQ(viaV1, viaV2);
    }
}

TEST(TagAllocatorV2, LegacySnapshotRoundTripsThroughV2Narrowing) {
    TagAllocator alloc;
    (void)alloc.nextShapeIdentity();
    (void)alloc.nextShapeIdentity();
    OREO_DEPRECATED_BEGIN
    auto snap = alloc.snapshot();
    EXPECT_EQ(snap.counter, 2);
    (void)alloc.nextShapeIdentity();
    (void)alloc.nextShapeIdentity();
    alloc.restore(snap, /*force=*/true);
    OREO_DEPRECATED_END
    EXPECT_EQ(alloc.peekShapeIdentity(), (ShapeIdentity{0, 3}));
}
