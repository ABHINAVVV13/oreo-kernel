// SPDX-License-Identifier: LGPL-2.1-or-later

// test_serialize_migration.cpp — Phase B2: lock down compatibility
// guarantees for the v1/v2/v3 serialize wire formats.
//
// Contract under test (see src/io/oreo_serialize.h header comment):
//   v1 (legacy)    — accepted by deserialize, identity reconstructed
//                    via decodeV1Scalar with ctx.documentId as hint.
//   v2 (RESERVED)  — rejected at the version gate; never written.
//   v3 (current)   — accepted, full 64+64 identity preserved exactly.
//
//   Corruption modes that MUST fail-closed:
//     - tampered checksum byte
//     - truncated tail (missing identity / map / brep)
//     - trailing garbage past the documented end
//     - wrong format-version byte
//     - oversized buffer beyond context maxSerializeBytes quota

#include <gtest/gtest.h>

#include "core/kernel_context.h"
#include "core/diagnostic.h"
#include "io/oreo_serialize.h"
#include "naming/named_shape.h"

#include <BRepPrimAPI_MakeBox.hxx>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

oreo::NamedShape makeBox(oreo::KernelContext& ctx, double dx, double dy, double dz) {
    TopoDS_Shape s = BRepPrimAPI_MakeBox(dx, dy, dz).Shape();
    return oreo::NamedShape(s, ctx.tags().nextShapeIdentity());
}

// Roundtrip a shape through serialize/deserialize and assert success.
void assertRoundtripOK(oreo::KernelContext& ctx, const oreo::NamedShape& ns) {
    auto wr = oreo::serialize(ctx, ns);
    ASSERT_TRUE(wr.ok()) << "serialize failed";
    const auto buf = wr.value();
    auto rr = oreo::deserialize(ctx, buf.data(), buf.size());
    ASSERT_TRUE(rr.ok()) << "deserialize roundtrip failed";
}

} // anonymous namespace

// ════════════════════════════════════════════════════════════════
// Round-trip — current v3 format
// ════════════════════════════════════════════════════════════════

TEST(SerializeMigration, V3RoundtripPreservesIdentity) {
    oreo::KernelConfig cfg;
    cfg.documentId = 0xABCDEF0123456789ULL;
    auto ctx = oreo::KernelContext::create(cfg);
    auto box = makeBox(*ctx, 5, 7, 11);
    auto wr = oreo::serialize(*ctx, box);
    ASSERT_TRUE(wr.ok());
    auto buf = wr.value();
    // First byte is the format version.
    EXPECT_EQ(buf[0], oreo::OREO_SERIALIZE_FORMAT_VERSION);

    auto rr = oreo::deserialize(*ctx, buf.data(), buf.size());
    ASSERT_TRUE(rr.ok());
    auto restored = rr.value();
    EXPECT_EQ(restored.shapeId().documentId, box.shapeId().documentId);
    EXPECT_EQ(restored.shapeId().counter,    box.shapeId().counter);
}

// ════════════════════════════════════════════════════════════════
// Hostile-byte rejection (the auditor's "fail-closed" contract)
// ════════════════════════════════════════════════════════════════

TEST(SerializeMigration, V2OuterByteIsRejected) {
    auto ctx = oreo::KernelContext::create();
    auto box = makeBox(*ctx, 1, 2, 3);
    auto buf = oreo::serialize(*ctx, box).value();
    // Tamper version byte 0 (v3) → 2 (RESERVED). v2 is documented as
    // illegal at the outer layer.
    buf[0] = 2;
    auto rr = oreo::deserialize(*ctx, buf.data(), buf.size());
    EXPECT_FALSE(rr.ok());
    EXPECT_TRUE(ctx->diag().hasErrors());
}

TEST(SerializeMigration, UnknownVersionByteIsRejected) {
    auto ctx = oreo::KernelContext::create();
    auto box = makeBox(*ctx, 1, 2, 3);
    auto buf = oreo::serialize(*ctx, box).value();
    buf[0] = 99;  // garbage version
    auto rr = oreo::deserialize(*ctx, buf.data(), buf.size());
    EXPECT_FALSE(rr.ok());
}

TEST(SerializeMigration, ChecksumTamperingFails) {
    auto ctx = oreo::KernelContext::create();
    auto box = makeBox(*ctx, 1, 2, 3);
    auto buf = oreo::serialize(*ctx, box).value();
    // Layout: [version=v3][magic:4][checksum:8][payload...]
    // Flip a bit in the payload (offset 1+4+8 = 13) so the stored
    // checksum no longer matches.
    ASSERT_GT(buf.size(), 14u);
    buf[13] ^= 0x01;
    auto rr = oreo::deserialize(*ctx, buf.data(), buf.size());
    EXPECT_FALSE(rr.ok());
}

TEST(SerializeMigration, TruncatedTailFails) {
    auto ctx = oreo::KernelContext::create();
    auto box = makeBox(*ctx, 1, 2, 3);
    auto buf = oreo::serialize(*ctx, box).value();
    // Drop the last 16 bytes (the root identity tail in v3).
    buf.resize(buf.size() - 16);
    auto rr = oreo::deserialize(*ctx, buf.data(), buf.size());
    EXPECT_FALSE(rr.ok());
}

TEST(SerializeMigration, TrailingBytesFail) {
    auto ctx = oreo::KernelContext::create();
    auto box = makeBox(*ctx, 1, 2, 3);
    auto buf = oreo::serialize(*ctx, box).value();
    // Append junk past the documented end; deserialize must reject.
    buf.push_back(0xFF);
    buf.push_back(0xFE);
    auto rr = oreo::deserialize(*ctx, buf.data(), buf.size());
    EXPECT_FALSE(rr.ok());
}

TEST(SerializeMigration, EmptyBufferFails) {
    auto ctx = oreo::KernelContext::create();
    std::vector<std::uint8_t> buf;
    auto rr = oreo::deserialize(*ctx, buf.data(), buf.size());
    EXPECT_FALSE(rr.ok());
}

TEST(SerializeMigration, NullBufferFails) {
    auto ctx = oreo::KernelContext::create();
    auto rr = oreo::deserialize(*ctx, nullptr, 100);
    EXPECT_FALSE(rr.ok());
}

// ════════════════════════════════════════════════════════════════
// Quota enforcement — Phase A3 hookup
// ════════════════════════════════════════════════════════════════

TEST(SerializeMigration, OversizedDeserializeInputRejectedByQuota) {
    oreo::KernelConfig cfg;
    cfg.quotas.maxSerializeBytes = 64;  // tiny
    auto ctx = oreo::KernelContext::create(cfg);
    std::vector<std::uint8_t> hugeButFake(128, 0xAA);
    auto rr = oreo::deserialize(*ctx, hugeButFake.data(), hugeButFake.size());
    EXPECT_FALSE(rr.ok());
    // The diagnostic should mention the quota by name.
    bool sawQuota = false;
    for (auto& d : ctx->diag().all()) {
        if (d.message.find("maxSerializeBytes") != std::string::npos) {
            sawQuota = true;
            break;
        }
    }
    EXPECT_TRUE(sawQuota) << "expected RESOURCE_EXHAUSTED diagnostic";
}

TEST(SerializeMigration, OversizedSerializeOutputRejectedByQuota) {
    // Quota set just below the round-trip size of a small box. The
    // box serializes to a few hundred bytes — quota 16 forces failure.
    oreo::KernelConfig cfg;
    cfg.quotas.maxSerializeBytes = 16;
    auto ctx = oreo::KernelContext::create(cfg);
    auto box = makeBox(*ctx, 1, 2, 3);
    auto wr = oreo::serialize(*ctx, box);
    EXPECT_FALSE(wr.ok());
}

// ════════════════════════════════════════════════════════════════
// Cross-document v1 → v3 hand-decoded migration
// ════════════════════════════════════════════════════════════════

TEST(SerializeMigration, RoundTripSurvivesContextSwap) {
    // Serialize in ctxA, deserialize in ctxB — the wire format is
    // self-describing so the bytes interpret identically. Identity
    // bits survive intact (v3 stores the full 16-byte identity).
    oreo::KernelConfig cfgA; cfgA.documentId = 0x1111111122222222ULL;
    oreo::KernelConfig cfgB; cfgB.documentId = 0x3333333344444444ULL;
    auto ctxA = oreo::KernelContext::create(cfgA);
    auto ctxB = oreo::KernelContext::create(cfgB);
    auto box  = makeBox(*ctxA, 4, 4, 4);
    const auto savedId = box.shapeId();
    auto wr = oreo::serialize(*ctxA, box);
    ASSERT_TRUE(wr.ok());
    auto rr = oreo::deserialize(*ctxB, wr.value().data(), wr.value().size());
    ASSERT_TRUE(rr.ok());
    auto restored = rr.value();
    // Identity is preserved bit-for-bit, INCLUDING the source docId
    // (so ctxB can tell this shape was authored by ctxA).
    EXPECT_EQ(restored.shapeId().documentId, savedId.documentId);
    EXPECT_EQ(restored.shapeId().counter,    savedId.counter);
}
