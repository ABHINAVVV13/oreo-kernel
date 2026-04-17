// SPDX-License-Identifier: LGPL-2.1-or-later

// test_step_identity.cpp — Phase B1 regression: every STEP-imported
// face / edge / vertex must carry a valid v2 ShapeIdentity, with the
// importing context's documentId and a unique counter per element.
//
// Bug history: pre-2026-04-17 the STEP import path either left
// imported elements unnamed (no ShapeIdentity at all) or squeezed
// them into a v1 scalar without preserving the high 32 bits of the
// docId. The current import path calls ctx.tags().nextShapeIdentity()
// per sub-shape and embeds the full 64+64 identity via the ;:P
// marker in encodeElementName. This test locks that contract.

#include <gtest/gtest.h>

#include "oreo_kernel.h"
#include "core/kernel_context.h"
#include "io/oreo_step.h"
#include "naming/element_map.h"
#include "naming/named_shape.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>

#include <set>
#include <vector>

namespace {

// Build a simple STEP buffer in memory: round-trip a known box through
// the kernel's exporter so we can re-import it without depending on
// any on-disk fixture.
std::vector<uint8_t> exportBoxAsStep(oreo::KernelContext& ctx,
                                      double dx, double dy, double dz) {
    TopoDS_Shape s = BRepPrimAPI_MakeBox(dx, dy, dz).Shape();
    oreo::NamedShape ns(s, ctx.tags().nextTag());
    auto r = oreo::exportStep(ctx, {ns}, {});
    if (!r.ok()) return {};
    return r.value();
}

std::vector<uint8_t> exportCylinderAsStep(oreo::KernelContext& ctx,
                                           double radius, double height) {
    TopoDS_Shape s = BRepPrimAPI_MakeCylinder(radius, height).Shape();
    oreo::NamedShape ns(s, ctx.tags().nextTag());
    auto r = oreo::exportStep(ctx, {ns}, {});
    if (!r.ok()) return {};
    return r.value();
}

} // anonymous namespace

// ────────────────────────────────────────────────────────────────
// Phase B1: every imported sub-shape carries a v2 identity
// ────────────────────────────────────────────────────────────────

TEST(StepIdentity, ImportedBoxFacesHaveContextDocId) {
    auto exporter = oreo::KernelContext::create();
    auto step = exportBoxAsStep(*exporter, 10, 20, 30);
    ASSERT_FALSE(step.empty());

    // Import into a NEW context with a deterministic documentId so
    // we can assert every imported face carries that docId.
    oreo::KernelConfig cfg;
    cfg.documentId = 0xCAFEBABEDEADBEEFULL;
    auto importer = oreo::KernelContext::create(cfg);
    auto r = oreo::importStep(*importer, step.data(), step.size());
    ASSERT_TRUE(r.ok()) << "STEP import failed";

    auto ns = std::move(r).value().shape;
    ASSERT_FALSE(ns.isNull());
    ASSERT_NE(ns.elementMap(), nullptr);

    int faceCount = ns.countSubShapes(TopAbs_FACE);
    EXPECT_EQ(faceCount, 6);

    std::set<oreo::ShapeIdentity> seen;
    for (int i = 1; i <= faceCount; ++i) {
        oreo::IndexedName idx("Face", i);
        auto name = ns.getElementName(idx);
        auto id = oreo::ElementMap::extractShapeIdentity(name);
        EXPECT_TRUE(id.isValid())
            << "Face" << i << " has no ShapeIdentity (name=" << name.data() << ")";
        EXPECT_EQ(id.documentId, 0xCAFEBABEDEADBEEFULL)
            << "Face" << i << " docId mismatch (got 0x"
            << std::hex << id.documentId << ")";
        // Every face must have a UNIQUE counter.
        EXPECT_TRUE(seen.insert(id).second)
            << "Face" << i << " has duplicate identity";
    }
}

TEST(StepIdentity, ImportedBoxEdgesHaveContextDocId) {
    auto exporter = oreo::KernelContext::create();
    auto step = exportBoxAsStep(*exporter, 10, 20, 30);
    ASSERT_FALSE(step.empty());

    oreo::KernelConfig cfg;
    cfg.documentId = 0x1111222233334444ULL;
    auto importer = oreo::KernelContext::create(cfg);
    auto r = oreo::importStep(*importer, step.data(), step.size());
    ASSERT_TRUE(r.ok());

    auto ns = std::move(r).value().shape;
    int edgeCount = ns.countSubShapes(TopAbs_EDGE);
    EXPECT_EQ(edgeCount, 12);

    std::set<oreo::ShapeIdentity> seen;
    for (int i = 1; i <= edgeCount; ++i) {
        auto name = ns.getElementName(oreo::IndexedName("Edge", i));
        auto id = oreo::ElementMap::extractShapeIdentity(name);
        EXPECT_TRUE(id.isValid()) << "Edge" << i << " missing identity";
        EXPECT_EQ(id.documentId, 0x1111222233334444ULL);
        EXPECT_TRUE(seen.insert(id).second) << "Edge" << i << " duplicate identity";
    }
}

TEST(StepIdentity, TwoImportsHaveDistinctIdentitiesPerContext) {
    // Different contexts (different documentIds) MUST produce identities
    // that don't collide across documents — that's the whole point of v2.
    auto exporter = oreo::KernelContext::create();
    auto step = exportCylinderAsStep(*exporter, 5, 10);
    ASSERT_FALSE(step.empty());

    // docIds must differ in the low 32 bits — TagAllocator refuses
    // to register two distinct 64-bit docIds whose low halves alias
    // (the v1 squeeze would lose them).
    oreo::KernelConfig cfgA; cfgA.documentId = 0xAAAA0000000000A1ULL;
    oreo::KernelConfig cfgB; cfgB.documentId = 0xBBBB0000000000B2ULL;
    auto ctxA = oreo::KernelContext::create(cfgA);
    auto ctxB = oreo::KernelContext::create(cfgB);

    auto rA = oreo::importStep(*ctxA, step.data(), step.size());
    auto rB = oreo::importStep(*ctxB, step.data(), step.size());
    ASSERT_TRUE(rA.ok()); ASSERT_TRUE(rB.ok());
    auto nsA = std::move(rA).value().shape;
    auto nsB = std::move(rB).value().shape;

    auto faceA = oreo::ElementMap::extractShapeIdentity(
        nsA.getElementName(oreo::IndexedName("Face", 1)));
    auto faceB = oreo::ElementMap::extractShapeIdentity(
        nsB.getElementName(oreo::IndexedName("Face", 1)));

    EXPECT_NE(faceA.documentId, faceB.documentId);
    EXPECT_EQ(faceA.documentId, 0xAAAA0000000000A1ULL);
    EXPECT_EQ(faceB.documentId, 0xBBBB0000000000B2ULL);
}

TEST(StepIdentity, PublicCApiReturnsNonZeroIdsForImportedFaces) {
    // End-to-end: the public C API surface (oreo_face_shape_id)
    // must observe the same v2 identity that the C++ extract path sees.
    auto exporter = oreo::KernelContext::create();
    auto step = exportBoxAsStep(*exporter, 5, 5, 5);
    ASSERT_FALSE(step.empty());

    OreoContext ctx = oreo_context_create_with_doc(0x9999000000000123ULL, nullptr);
    ASSERT_NE(ctx, nullptr);

    OreoSolid solid = oreo_ctx_import_step(ctx, step.data(), step.size());
    ASSERT_NE(solid, nullptr);

    int faceCount = oreo_ctx_face_count(ctx, solid);
    EXPECT_EQ(faceCount, 6);

    std::set<std::pair<uint64_t, uint64_t>> seen;
    for (int i = 1; i <= faceCount; ++i) {
        OreoShapeId sid{0, 0};
        EXPECT_EQ(oreo_face_shape_id(solid, i, &sid), OREO_OK);
        EXPECT_EQ(sid.document_id, 0x9999000000000123ULL)
            << "Face " << i << " documentId mismatch";
        EXPECT_NE(sid.counter, 0u) << "Face " << i << " has zero counter";
        EXPECT_TRUE(seen.insert({sid.document_id, sid.counter}).second)
            << "Face " << i << " duplicate identity";
    }

    oreo_free_solid(solid);
    oreo_context_free(ctx);
}
