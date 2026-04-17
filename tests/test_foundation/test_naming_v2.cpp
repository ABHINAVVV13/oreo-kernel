// SPDX-License-Identifier: LGPL-2.1-or-later

// test_naming_v2.cpp — Phase 3 acceptance tests.
//
// Covers:
//   1. MappedName::appendShapeIdentity writes the ;:P<16hex>.<16hex>
//      postfix and round-trips losslessly through extractShapeIdentity.
//   2. ElementMap::extractShapeIdentity implements the rightmost-marker
//      algorithm correctly for mixed v1/v2 chains (docs/identity-model.md
//      §4.3 rule 4).
//   3. ElementMap v3 serialize/deserialize round-trips a ShapeIdentity
//      without squeezing through the v1 scalar boundary.
//   4. ElementMap v2 reader decodes legacy buffers with the provided
//      root hint and emits LEGACY_IDENTITY_DOWNGRADE when no hint is
//      supplied (Case B of §3.2).
//   5. ChildElementMap::id replaces the old int64 tag field end-to-end
//      through serialize/deserialize.

#include "core/diagnostic.h"
#include "core/kernel_context.h"
#include "core/shape_identity.h"
#include "core/shape_identity_v1.h"
#include "geometry/oreo_geometry.h"
#include "naming/element_map.h"
#include "naming/mapped_name.h"
#include "naming/named_shape.h"
#include "query/oreo_query.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using oreo::ShapeIdentity;
using oreo::MappedName;
using oreo::IndexedName;
using oreo::ElementMap;
using oreo::ChildElementMap;
using oreo::DiagnosticCollector;
using oreo::ErrorCode;

// ─── appendShapeIdentity / extractShapeIdentity round-trip ──────────

TEST(NamingV2, AppendShapeIdentityProducesCanonicalPostfix) {
    MappedName name("Face1");
    name.appendShapeIdentity(ShapeIdentity{0xDEADBEEFCAFEBABEull, 0x42});
    // ;:P payload is 32 hex chars: docId16 + counter16, no separator
    // (FreeCAD's mapped-name validator rejects '.').
    EXPECT_EQ(name.data(), "Face1;:Pdeadbeefcafebabe0000000000000042");
}

TEST(NamingV2, ExtractShapeIdentityFromPostfix) {
    MappedName name("Face1");
    name.appendShapeIdentity(ShapeIdentity{0xDEADBEEFCAFEBABEull, 0x42});
    auto id = ElementMap::extractShapeIdentity(name);
    EXPECT_EQ(id, (ShapeIdentity{0xDEADBEEFCAFEBABEull, 0x42}));
}

TEST(NamingV2, ExtractTagRoundsThroughV1Boundary) {
    // For v2 postfixes, extractTag must return the encodeV1Scalar of the
    // extracted identity. Multi-doc with counter ≤ UINT32_MAX works.
    MappedName name("Face1");
    name.appendShapeIdentity(ShapeIdentity{0x00000000CAFEBABEull, 0x42});
    std::int64_t scalar = ElementMap::extractTag(name);
    EXPECT_EQ(static_cast<std::uint64_t>(scalar),
              (0xCAFEBABEull << 32) | 0x42);
}

// ─── Rightmost-marker algorithm (§4.3 rule 4) ──────────────────────

TEST(NamingV2, RightmostMarkerPWinsWhenLastOpIsV2) {
    // Chain: ;:P<…> then ;:M (v2 latest op, no further ops).
    MappedName name("Face1");
    name.appendShapeIdentity(ShapeIdentity{0xABCD, 0x10});
    name.appendPostfix(";:M");
    EXPECT_EQ(ElementMap::extractShapeIdentity(name),
              (ShapeIdentity{0xABCD, 0x10}));
}

TEST(NamingV2, RightmostMarkerHWinsWhenLastOpIsV1Legacy) {
    // Chain: ;:P<…>;:M;:H<scalar>;:G — latest op is v1 legacy. The
    // reader must prefer the rightmost ;:H, not the earlier ;:P.
    MappedName name("Face1");
    name.appendShapeIdentity(ShapeIdentity{0xABCD, 0x10});
    name.appendPostfix(";:M");
    // Manually append a v1 ;:H hop with hex "20" (decimal 32), then ;:G.
    name = MappedName(name.data() + ";:H20;:G");
    auto id = ElementMap::extractShapeIdentity(name);
    // No ;:Q postfix, so ambient doc = 0 — decodeV1Scalar Case A:
    // high32==0 → counter = scalar, docId = hint = 0.
    EXPECT_EQ(id, (ShapeIdentity{0, 0x20}));
}

TEST(NamingV2, RightmostMarkerBareBaseReturnsInvalid) {
    // A name with no identity markers is a bare base — returns {0,0}.
    MappedName name("Face1");
    auto id = ElementMap::extractShapeIdentity(name);
    EXPECT_EQ(id, (ShapeIdentity{}));
    EXPECT_FALSE(id.isValid());
}

TEST(NamingV2, RightmostMarkerMalformedPIdentityReturnsInvalid) {
    // Truncated ;:P payload — must return invalid, must not throw.
    MappedName name("Face1;:Pdeadbeef");  // short of 32 hex
    auto id = ElementMap::extractShapeIdentity(name);
    EXPECT_EQ(id, (ShapeIdentity{}));
}

TEST(NamingV2, RightmostMarkerV1WithAmbientDocUpgrades) {
    // Chain: ;:H<scalar>;:M;:Q<docId>. decodeV1Scalar uses the ;:Q
    // ambient doc as hint; low32(hint) must equal high32(scalar).
    // 0xCAFEBABE as low32 of docId matches high32 of scalar.
    const std::uint64_t docId = 0x1111AAAACAFEBABEull;
    const std::int64_t scalar =
        static_cast<std::int64_t>((static_cast<std::uint64_t>(0xCAFEBABE) << 32) | 0x42);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Face1;:H%llx;:M;:Q%016llx",
                  static_cast<unsigned long long>(scalar),
                  static_cast<unsigned long long>(docId));
    MappedName name(buf);
    auto id = ElementMap::extractShapeIdentity(name);
    // Case C: full docId reconstructed from hint.
    EXPECT_EQ(id, (ShapeIdentity{docId, 0x42}));
}

TEST(NamingV2, RightmostMarkerV1ErrorCaseReturnsInvalidWithoutThrowing) {
    // Hand-craft ;:H with a scalar whose high32 != low32(;:Q). The
    // decoder throws invalid_argument in §3.2 Error Case; the parser
    // must catch and return {0,0} per rule 4e.
    const std::uint64_t docId = 0x0000000011111111ull;  // low32 = 0x11111111
    const std::int64_t scalar =
        static_cast<std::int64_t>((static_cast<std::uint64_t>(0xCAFEBABE) << 32) | 0x42);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Face1;:H%llx;:Q%016llx",
                  static_cast<unsigned long long>(scalar),
                  static_cast<unsigned long long>(docId));
    MappedName name(buf);
    ShapeIdentity id;
    EXPECT_NO_THROW(id = ElementMap::extractShapeIdentity(name));
    EXPECT_EQ(id, (ShapeIdentity{}));
}

// ─── ElementMap v3 serialize / deserialize round-trip ──────────────

TEST(NamingV2, ElementMapV3RoundTripsFullIdentity) {
    auto orig = std::make_shared<ElementMap>();
    {
        MappedName name("Face1");
        name.appendShapeIdentity(ShapeIdentity{0xDEADBEEFCAFEBABEull, 0x42});
        orig->setElementName(IndexedName("Face", 1), name, /*tag=*/0);
    }
    {
        MappedName name("Edge1");
        name.appendShapeIdentity(ShapeIdentity{0xDEADBEEFCAFEBABEull, 0x43});
        orig->setElementName(IndexedName("Edge", 1), name, /*tag=*/0);
    }

    auto buf = orig->serialize();
    // First 4 bytes = FORMAT_VERSION = 3, little-endian.
    ASSERT_GE(buf.size(), 4u);
    EXPECT_EQ(buf[0], 3);
    EXPECT_EQ(buf[1], 0);

    auto restored = ElementMap::deserialize(buf.data(), buf.size());
    ASSERT_NE(restored, nullptr);

    auto faceName = restored->getMappedName(IndexedName("Face", 1));
    auto edgeName = restored->getMappedName(IndexedName("Edge", 1));
    EXPECT_EQ(faceName.data(), "Face1;:Pdeadbeefcafebabe0000000000000042");
    EXPECT_EQ(edgeName.data(), "Edge1;:Pdeadbeefcafebabe0000000000000043");

    // Per-entry ShapeIdentity survives end-to-end.
    EXPECT_EQ(ElementMap::extractShapeIdentity(faceName),
              (ShapeIdentity{0xDEADBEEFCAFEBABEull, 0x42}));
    EXPECT_EQ(ElementMap::extractShapeIdentity(edgeName),
              (ShapeIdentity{0xDEADBEEFCAFEBABEull, 0x43}));
}

TEST(NamingV2, ElementMapV3RoundTripsChildElementMap) {
    // Child map with its own identity — exercise the 16-byte child
    // serialization path.
    auto child = std::make_shared<ElementMap>();
    {
        MappedName n("Face1");
        n.appendShapeIdentity(ShapeIdentity{0xAAAA, 1});
        child->setElementName(IndexedName("Face", 1), n, /*tag=*/0);
    }
    auto parent = std::make_shared<ElementMap>();
    {
        MappedName n("Face2");
        n.appendShapeIdentity(ShapeIdentity{0xBBBB, 1});
        parent->setElementName(IndexedName("Face", 2), n, /*tag=*/0);
    }
    ChildElementMap ce;
    ce.map = child;
    ce.id  = ShapeIdentity{0xCCCC, 0x99};
    ce.postfix = ";:Ctest";
    parent->addChild(ce);

    auto buf = parent->serialize();
    auto restored = ElementMap::deserialize(buf.data(), buf.size());
    ASSERT_NE(restored, nullptr);
    ASSERT_EQ(restored->children().size(), 1u);
    const auto& rc = restored->children()[0];
    EXPECT_EQ(rc.id, (ShapeIdentity{0xCCCC, 0x99}));
    EXPECT_EQ(rc.postfix, ";:Ctest");
    ASSERT_NE(rc.map, nullptr);
    auto childFace = rc.map->getMappedName(IndexedName("Face", 1));
    EXPECT_EQ(ElementMap::extractShapeIdentity(childFace),
              (ShapeIdentity{0xAAAA, 1}));
}

TEST(NamingV2, ElementMapRejectsV1Buffer) {
    // A buffer claiming version 1 must be rejected (v1 was pre-audit,
    // never accepted since the April 2026 audit).
    std::vector<std::uint8_t> v1buf = {1, 0, 0, 0,   // version = 1
                                        0, 0, 0, 0}; // rest doesn't matter
    auto restored = ElementMap::deserialize(v1buf.data(), v1buf.size());
    EXPECT_EQ(restored, nullptr);
}

TEST(NamingV2, ElementMapRejectsUnknownFutureVersion) {
    std::vector<std::uint8_t> vFuture = {99, 0, 0, 0};  // version = 99
    auto restored = ElementMap::deserialize(vFuture.data(), vFuture.size());
    EXPECT_EQ(restored, nullptr);
}

// ─── v2 → v3 upgrade path ───────────────────────────────────────────
//
// We hand-craft a v2 ElementMap buffer (version byte = 2, per-entry
// 8-byte int64 scalar, no 16-byte identity) and verify the v3 reader
// accepts it, hint-upgrades it, and fires the downgrade warning.

namespace {

// Helpers mirroring the v2 wire format.
void appendU16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}
void appendU32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        b.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFF));
}
void appendI64(std::vector<std::uint8_t>& b, std::int64_t v) {
    auto u = static_cast<std::uint64_t>(v);
    for (int i = 0; i < 8; ++i)
        b.push_back(static_cast<std::uint8_t>((u >> (i * 8)) & 0xFF));
}
void appendStr(std::vector<std::uint8_t>& b, const std::string& s) {
    appendU32(b, static_cast<std::uint32_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}

std::vector<std::uint8_t> buildV2Buffer(
        const std::string& type, int index,
        const std::string& nameData, std::int64_t scalar) {
    std::vector<std::uint8_t> buf;
    appendU32(buf, 2);  // v2 version
    appendU32(buf, 1);  // one entry
    appendU16(buf, static_cast<std::uint16_t>(type.size()));
    buf.insert(buf.end(), type.begin(), type.end());
    appendU32(buf, static_cast<std::uint32_t>(index));
    appendStr(buf, nameData);
    appendI64(buf, scalar);
    appendU32(buf, 0);  // zero children
    return buf;
}

} // namespace

TEST(NamingV2, V2ReaderMultiDocWithoutHintFiresDowngrade) {
    // Scalar encodes ShapeIdentity{0xCAFEBABE, 0x42}. No hint → Case B
    // emits LEGACY_IDENTITY_DOWNGRADE.
    const std::int64_t scalar =
        static_cast<std::int64_t>((static_cast<std::uint64_t>(0xCAFEBABE) << 32) | 0x42);
    auto buf = buildV2Buffer("Face", 1, "Face1", scalar);

    DiagnosticCollector diag;
    auto restored = ElementMap::deserialize(buf.data(), buf.size(),
                                            /*rootHint=*/{}, &diag);
    ASSERT_NE(restored, nullptr);
    EXPECT_TRUE(diag.hasWarnings())
        << "Case B multi-doc read without hint must emit the downgrade warning";
    auto warns = diag.warnings();
    ASSERT_GE(warns.size(), 1u);
    EXPECT_EQ(warns[0].code, ErrorCode::LEGACY_IDENTITY_DOWNGRADE);
}

TEST(NamingV2, V2ReaderSingleDocScalarNeverFiresDowngrade) {
    // Scalar 42 with high32==0 is Case A — no warning even without hint.
    auto buf = buildV2Buffer("Face", 1, "Face1", 42);
    DiagnosticCollector diag;
    auto restored = ElementMap::deserialize(buf.data(), buf.size(),
                                            /*rootHint=*/{}, &diag);
    ASSERT_NE(restored, nullptr);
    EXPECT_FALSE(diag.hasWarnings())
        << "Case A (single-doc scalar) must be silent";
}

TEST(NamingV2, V2ReaderMismatchedHintBailsWithoutPartialMap) {
    // Scalar's high32 = 0xCAFEBABE, hint's low32 = 0x11111111 — mismatch.
    // Reader catches and returns nullptr; no partial map leaks.
    const std::int64_t scalar =
        static_cast<std::int64_t>((static_cast<std::uint64_t>(0xCAFEBABE) << 32) | 0x42);
    auto buf = buildV2Buffer("Face", 1, "Face1", scalar);
    DiagnosticCollector diag;
    auto restored = ElementMap::deserialize(buf.data(), buf.size(),
                                            /*rootHint=*/{0x0000000011111111ull, 1},
                                            &diag);
    EXPECT_EQ(restored, nullptr);
}

// ─── P1 native v2 — v3 round-trips counter > UINT32_MAX ─────────────
//
// This is the auditor's P1 acceptance criterion. Before NamedShape
// carried a ShapeIdentity directly, the v3 deserializer encoded the
// 16-byte identity back to a v1 int64 scalar, which threw
// V2_IDENTITY_NOT_REPRESENTABLE for multi-doc counter > UINT32_MAX.
// Now the identity flows straight through the constructor.

TEST(NamingV2, V3RoundTripsMultiDocCounterAboveUint32Max) {
    const std::uint64_t kHugeCounter =
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1;
    const ShapeIdentity bigId{0xDEADBEEFCAFEBABEull, kHugeCounter};

    auto map = std::make_shared<ElementMap>();
    MappedName name("Face1");
    name.appendShapeIdentity(bigId);
    map->setElementName(IndexedName("Face", 1), name, /*tag=*/0);

    // Serialize the map and re-read it. No throw, identity preserved.
    auto mapBuf = map->serialize();
    auto restoredMap = ElementMap::deserialize(mapBuf.data(), mapBuf.size());
    ASSERT_NE(restoredMap, nullptr);
    auto restoredName = restoredMap->getMappedName(IndexedName("Face", 1));
    EXPECT_EQ(ElementMap::extractShapeIdentity(restoredName), bigId);
}

// ─── Auditor fix #1 — LEGACY_IDENTITY_DOWNGRADE dedup per top-level ─

TEST(NamingV2, LegacyDowngradeDedupsAcrossManyEntries) {
    // Build a v2 ElementMap buffer with many entries, all of which are
    // multi-doc encoded scalars whose hint would be missing at read
    // time. Each decode Case B would fire a LEGACY_IDENTITY_DOWNGRADE
    // on its own; inside a top-level dedup scope only the FIRST fires.
    std::vector<std::uint8_t> buf;
    appendU32(buf, 2);              // v2 version (triggers legacy read path)
    const int kEntries = 12;
    appendU32(buf, static_cast<std::uint32_t>(kEntries));
    for (int i = 0; i < kEntries; ++i) {
        appendU16(buf, 4);  // "Face"
        buf.insert(buf.end(), {'F', 'a', 'c', 'e'});
        appendU32(buf, static_cast<std::uint32_t>(i + 1));
        appendStr(buf, "Face" + std::to_string(i + 1));
        // Multi-doc scalar with docId 0xCAFEBABE. No hint → Case B.
        const std::int64_t scalar =
            static_cast<std::int64_t>((static_cast<std::uint64_t>(0xCAFEBABE) << 32)
                                      | static_cast<std::uint64_t>(i + 1));
        appendI64(buf, scalar);
    }
    appendU32(buf, 0);  // zero children

    DiagnosticCollector diag;
    {
        // Install a dedup scope — simulating what oreo_serialize.cpp's
        // top-level deserialize does.
        oreo::internal::LegacyDowngradeDedupScope dedup;
        auto restored = ElementMap::deserialize(buf.data(), buf.size(),
                                                /*rootHint=*/{}, &diag);
        ASSERT_NE(restored, nullptr);
    }
    // Exactly one LEGACY_IDENTITY_DOWNGRADE despite 12 per-entry decodes.
    auto warns = diag.warnings();
    int downgradeCount = 0;
    for (const auto& w : warns) {
        if (w.code == ErrorCode::LEGACY_IDENTITY_DOWNGRADE) ++downgradeCount;
    }
    EXPECT_EQ(downgradeCount, 1)
        << "Dedup scope must coalesce downgrade warnings to one per top-level deserialize";
}

TEST(NamingV2, LegacyDowngradeWithoutScopeFiresEveryTime) {
    // Outside a dedup scope, every Case B fires its own warning — this
    // is the contract for one-off callers that don't participate in
    // top-level deserialize.
    DiagnosticCollector diag;
    const std::int64_t scalar =
        static_cast<std::int64_t>((static_cast<std::uint64_t>(0xCAFEBABE) << 32) | 1);
    (void)oreo::decodeV1Scalar(scalar, 0, &diag);
    (void)oreo::decodeV1Scalar(scalar, 0, &diag);
    (void)oreo::decodeV1Scalar(scalar, 0, &diag);
    int downgradeCount = 0;
    for (const auto& w : diag.warnings()) {
        if (w.code == ErrorCode::LEGACY_IDENTITY_DOWNGRADE) ++downgradeCount;
    }
    EXPECT_EQ(downgradeCount, 3);
}

TEST(NamingV2, LegacyDowngradeScopeResetsBetweenTopLevelCalls) {
    // Two independent dedup scopes → each fires its own first warning.
    // Simulates two sequential oreo_deserialize calls on the same
    // thread.
    DiagnosticCollector diag;
    const std::int64_t scalar =
        static_cast<std::int64_t>((static_cast<std::uint64_t>(0xCAFEBABE) << 32) | 1);
    {
        oreo::internal::LegacyDowngradeDedupScope s1;
        (void)oreo::decodeV1Scalar(scalar, 0, &diag);
        (void)oreo::decodeV1Scalar(scalar, 0, &diag);  // deduped
    }
    {
        oreo::internal::LegacyDowngradeDedupScope s2;
        (void)oreo::decodeV1Scalar(scalar, 0, &diag);  // fresh scope → fires
        (void)oreo::decodeV1Scalar(scalar, 0, &diag);  // deduped
    }
    int downgradeCount = 0;
    for (const auto& w : diag.warnings()) {
        if (w.code == ErrorCode::LEGACY_IDENTITY_DOWNGRADE) ++downgradeCount;
    }
    EXPECT_EQ(downgradeCount, 2);
}

TEST(NamingV2, LegacyDowngradeNestedScopesShareTopmostFlag) {
    // Nested scopes don't fire more warnings — only the outermost's
    // contract matters. This keeps the contract stable when an inner
    // deserialize is invoked by an outer one.
    DiagnosticCollector diag;
    const std::int64_t scalar =
        static_cast<std::int64_t>((static_cast<std::uint64_t>(0xCAFEBABE) << 32) | 1);
    {
        oreo::internal::LegacyDowngradeDedupScope outer;
        (void)oreo::decodeV1Scalar(scalar, 0, &diag);  // fires
        {
            oreo::internal::LegacyDowngradeDedupScope inner;
            (void)oreo::decodeV1Scalar(scalar, 0, &diag);  // inner — deduped
        }
        (void)oreo::decodeV1Scalar(scalar, 0, &diag);  // still outer — deduped
    }
    int downgradeCount = 0;
    for (const auto& w : diag.warnings()) {
        if (w.code == ErrorCode::LEGACY_IDENTITY_DOWNGRADE) ++downgradeCount;
    }
    EXPECT_EQ(downgradeCount, 1);
}

// ─── Auditor fix #2 — MALFORMED_ELEMENT_NAME emission ───────────────

TEST(NamingV2, MalformedElementNameEmitsOnTruncatedPayload) {
    DiagnosticCollector diag;
    MappedName name("Face1;:Pdeadbeef");  // short of 32 hex
    auto id = ElementMap::extractShapeIdentity(name, &diag);
    EXPECT_EQ(id, (ShapeIdentity{}));
    bool sawMalformed = false;
    for (const auto& w : diag.warnings()) {
        if (w.code == ErrorCode::MALFORMED_ELEMENT_NAME) sawMalformed = true;
    }
    EXPECT_TRUE(sawMalformed)
        << "Truncated ;:P payload must raise MALFORMED_ELEMENT_NAME";
}

TEST(NamingV2, MalformedElementNameEmitsOnNonHexCounter) {
    DiagnosticCollector diag;
    // 16 hex for docId + 16 chars with a non-hex in the counter field.
    MappedName name("Face1;:Pdeadbeefcafebabe000000000000000Z");
    auto id = ElementMap::extractShapeIdentity(name, &diag);
    EXPECT_EQ(id, (ShapeIdentity{}));
    bool sawMalformed = false;
    for (const auto& w : diag.warnings()) {
        if (w.code == ErrorCode::MALFORMED_ELEMENT_NAME) sawMalformed = true;
    }
    EXPECT_TRUE(sawMalformed);
}

TEST(NamingV2, MalformedElementNameSilentOnBareBaseName) {
    // A bare "Face1" with no identity marker is legitimate (e.g. a
    // freshly imported STEP shape). Must NOT fire MALFORMED.
    DiagnosticCollector diag;
    MappedName name("Face1");
    auto id = ElementMap::extractShapeIdentity(name, &diag);
    EXPECT_EQ(id, (ShapeIdentity{}));
    EXPECT_FALSE(diag.hasWarnings())
        << "Bare base name must not trigger MALFORMED_ELEMENT_NAME";
}

// ─── Auditor fix #3 — canonical ;:P naming from non-primitive ops ───

TEST(NamingV2, BooleanOpProducesOnlyV2CanonicalNames) {
    // Before the canonicalizer landed, non-primitive ops (boolean,
    // fillet, extrude, ...) routed names through the FreeCAD
    // extraction which emits ;:H<hex>. The primitive path wrote ;:P
    // but everything else stayed v1-shaped. Readers tolerated mixed
    // chains, but "v2 canonical naming" was a half-truth.
    //
    // Now toNamedShape runs canonicalizeToV2 at the adapter boundary
    // and every hop — including inputs traced from primitives — is
    // ;:P<32hex>. Verify by doing a boolean and scanning every face
    // name for either v1 marker.
    auto ctx = oreo::KernelContext::create();
    auto a = oreo::makeBox(*ctx, 10, 10, 10);
    ASSERT_TRUE(a.ok());
    auto b = oreo::makeBox(*ctx, 10, 10, 10);
    ASSERT_TRUE(b.ok());
    auto r = oreo::booleanUnion(*ctx, a.value(), b.value());
    ASSERT_TRUE(r.ok());
    auto result = r.value();
    const int faceCount = result.countSubShapes(TopAbs_FACE);
    ASSERT_GT(faceCount, 0);

    for (int i = 1; i <= faceCount; ++i) {
        auto name = result.elementMap()->getMappedName(
            oreo::IndexedName("Face", i));
        ASSERT_FALSE(name.empty()) << "Face" << i << " has no mapped name";
        const std::string& s = name.data();
        EXPECT_EQ(s.find(";:H"), std::string::npos)
            << "Face" << i << " still carries v1 ;:H postfix: " << s;
        EXPECT_EQ(s.find(";:T"), std::string::npos)
            << "Face" << i << " still carries v1 ;:T postfix: " << s;
        EXPECT_NE(s.find(";:P"), std::string::npos)
            << "Face" << i << " has no ;:P marker at all: " << s;
    }
}

TEST(NamingV2, FilletOutputAlsoCanonical) {
    // Fillet goes through TopoShapeAdapter (non-primitive path).
    // Same expectation as booleanUnion.
    auto ctx = oreo::KernelContext::create();
    auto boxR = oreo::makeBox(*ctx, 20, 20, 20);
    ASSERT_TRUE(boxR.ok());
    auto box = boxR.value();
    auto edgesR = oreo::getEdges(*ctx, box);
    ASSERT_TRUE(edgesR.ok());
    auto edges = edgesR.value();
    ASSERT_GT(edges.size(), 0u);
    std::vector<oreo::NamedEdge> filletEdges = {edges[0]};
    auto r = oreo::fillet(*ctx, box, filletEdges, 2.0);
    ASSERT_TRUE(r.ok());
    const auto& result = r.value();
    const int faceCount = result.countSubShapes(TopAbs_FACE);
    for (int i = 1; i <= faceCount; ++i) {
        auto name = result.elementMap()->getMappedName(
            oreo::IndexedName("Face", i));
        if (name.empty()) continue;  // FreeCAD algo may leave some unnamed
        const std::string& s = name.data();
        EXPECT_EQ(s.find(";:H"), std::string::npos) << s;
        EXPECT_EQ(s.find(";:T"), std::string::npos) << s;
    }
}

TEST(NamingV2, MultiHopChainAllHopsAreV2After) {
    // Build a chain: box → fillet → boolean. Every hop should land as
    // ;:P. This exercises canonicalizeToV2 on names that already
    // contain multiple ;:H hops stacked by the FreeCAD algorithm
    // across multiple ops.
    auto ctx = oreo::KernelContext::create();
    auto box1 = oreo::makeBox(*ctx, 10, 10, 10).value();
    auto box2 = oreo::makeBox(*ctx, 5, 5, 5).value();

    auto edges = oreo::getEdges(*ctx, box1).value();
    ASSERT_GT(edges.size(), 0u);
    std::vector<oreo::NamedEdge> filletEdges = {edges[0]};
    auto filleted = oreo::fillet(*ctx, box1, filletEdges, 1.0).value();
    auto combined = oreo::booleanUnion(*ctx, filleted, box2).value();

    const int faceCount = combined.countSubShapes(TopAbs_FACE);
    ASSERT_GT(faceCount, 0);

    // Count ;:P hops per name. Boolean/fillet stack means a typical
    // face has two or more identity hops; every one must be ;:P.
    int totalPHops = 0;
    int totalHHops = 0;
    for (int i = 1; i <= faceCount; ++i) {
        auto name = combined.elementMap()->getMappedName(
            oreo::IndexedName("Face", i));
        const std::string& s = name.data();
        for (size_t p = s.find(";:P"); p != std::string::npos; p = s.find(";:P", p + 3)) {
            ++totalPHops;
        }
        for (size_t p = s.find(";:H"); p != std::string::npos; p = s.find(";:H", p + 3)) {
            ++totalHHops;
        }
    }
    EXPECT_GT(totalPHops, 0);
    EXPECT_EQ(totalHHops, 0)
        << "Multi-hop chain leaked v1 ;:H hops past canonicalizeToV2";
}

TEST(NamingV2, MalformedElementNameOnHintScalarMismatch) {
    // ;:H scalar whose high32 doesn't match ;:Q's low32 — §3.2 Error
    // Case. extractShapeIdentity catches and emits MALFORMED.
    DiagnosticCollector diag;
    const std::uint64_t docQ    = 0x0000000011111111ull;  // low32 = 0x11111111
    const std::int64_t  scalarH =
        static_cast<std::int64_t>((static_cast<std::uint64_t>(0xCAFEBABE) << 32) | 1);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Face1;:H%llx;:Q%016llx",
                  static_cast<unsigned long long>(scalarH),
                  static_cast<unsigned long long>(docQ));
    MappedName name(buf);
    auto id = ElementMap::extractShapeIdentity(name, &diag);
    EXPECT_EQ(id, (ShapeIdentity{}));
    bool sawMalformed = false;
    for (const auto& w : diag.warnings()) {
        if (w.code == ErrorCode::MALFORMED_ELEMENT_NAME) sawMalformed = true;
    }
    EXPECT_TRUE(sawMalformed);
}
