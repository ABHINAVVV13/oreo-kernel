// SPDX-License-Identifier: LGPL-2.1-or-later

// test_naming_edge_cases.cpp — Tests the "hard" scenarios for the
// topological naming system:
//
//   1. Face-split: when a boolean operation partitions a parent face
//      into multiple children, every child's element map entry must
//      carry a non-empty ShapeIdentity. Closing this gap was flagged
//      in the April 2026 audit as a prerequisite to Onshape parity.
//
//   2. Face-merge / adjacent-face preservation: when a fillet round
//      replaces an edge with a swept face and trims adjacent faces,
//      the untouched faces' identities must survive the operation so
//      downstream references don't rebind mid-chain.
//
//   3. Feature reorder: moving a downstream feature ahead of its
//      referenced parent must fail CLEANLY (FeatureStatus::BrokenReference
//      + the broken-features list populated) rather than silently
//      succeeding, crashing, or returning a stale shape. Moving the
//      feature back must restore OK status on the next replay.
//
// These are the three canonical hard cases for parametric-CAD naming
// implementations. Onshape's naming handles them; pre-1.0 oreo-kernel
// had no dedicated test coverage for any of them. This file is that
// coverage.

#include <gtest/gtest.h>

#include "core/kernel_context.h"
#include "feature/feature.h"
#include "feature/feature_tree.h"
#include "geometry/oreo_geometry.h"
#include "naming/element_map.h"
#include "naming/named_shape.h"
#include "query/oreo_query.h"

#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <vector>

namespace {

// Count faces in `shape` whose element-map entry carries a *resolvable*
// ShapeIdentity. A face with documentId=0 AND counter=0 is considered
// unnamed (extractShapeIdentity returns the invalid sentinel); anything
// else is named.
int countNamedFaces(const oreo::NamedShape& shape) {
    if (shape.isNull() || !shape.elementMap()) return 0;
    int named = 0;
    const int faces = shape.countSubShapes(TopAbs_FACE);
    for (int i = 1; i <= faces; ++i) {
        auto name = shape.getElementName(oreo::IndexedName("Face", i));
        auto id   = oreo::ElementMap::extractShapeIdentity(name);
        if (!(id.documentId == 0 && id.counter == 0)) ++named;
    }
    return named;
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════
// Face-split on boolean subtract
// ═══════════════════════════════════════════════════════════════

TEST(NamingEdgeCases, BooleanSubtractFacesAreNamed) {
    auto ctx = oreo::KernelContext::create();

    // Box A: 20×20×20 at origin.
    auto a = oreo::makeBox(*ctx, 20.0, 20.0, 20.0);
    ASSERT_TRUE(a.ok()) << "Baseline makeBox failed";

    // Box B: a 30×5×5 rod positioned to pierce the top face of A.
    // OCCT's boolean will split A's top face into a ring-shaped
    // remainder plus the sidewalls of the carved-out channel.
    auto bMaker = oreo::makeBox(*ctx, 30.0, 5.0, 5.0);
    ASSERT_TRUE(bMaker.ok());
    // (Position tweak intentionally minimal — we just need a through-cut
    // that actually partitions faces rather than a pure subtraction
    // that leaves A's faces intact.)
    auto sub = oreo::booleanSubtract(*ctx, a.value(), bMaker.value());
    ASSERT_TRUE(sub.ok()) << "Boolean subtract failed";
    auto result = sub.value();

    const int totalFaces = result.countSubShapes(TopAbs_FACE);
    EXPECT_GT(totalFaces, 6)
        << "Expected >6 faces after through-cut (6 from box A + new "
           "sidewall faces from the subtraction), got " << totalFaces;

    // The critical invariant: EVERY face must carry a ShapeIdentity.
    // An unnamed face means the FreeCAD-derived naming algorithm
    // dropped the child somewhere and downstream references to the
    // result would be unresolvable.
    const int named = countNamedFaces(result);
    EXPECT_EQ(named, totalFaces)
        << "Only " << named << "/" << totalFaces
        << " faces carry ShapeIdentities; missing names block downstream "
           "features from referencing the cut-result geometry.";
}

// ═══════════════════════════════════════════════════════════════
// Fillet / adjacent-face preservation
// ═══════════════════════════════════════════════════════════════

TEST(NamingEdgeCases, FilletAdjacentFacesRetainIdentity) {
    auto ctx = oreo::KernelContext::create();

    auto boxR = oreo::makeBox(*ctx, 20.0, 20.0, 20.0);
    ASSERT_TRUE(boxR.ok());
    auto box = boxR.value();

    // Snapshot the pre-fillet identity of every face so we can look
    // for survivors after the operation. A box always has 6 faces.
    ASSERT_EQ(box.countSubShapes(TopAbs_FACE), 6);
    std::vector<oreo::ShapeIdentity> preIds;
    for (int i = 1; i <= 6; ++i) {
        auto name = box.getElementName(oreo::IndexedName("Face", i));
        preIds.push_back(oreo::ElementMap::extractShapeIdentity(name));
    }

    auto edgesR = oreo::getEdges(*ctx, box);
    ASSERT_TRUE(edgesR.ok());
    ASSERT_GE(edgesR.value().size(), 1u);

    std::vector<oreo::NamedEdge> filletEdges = {edgesR.value()[0]};
    auto filletR = oreo::fillet(*ctx, box, filletEdges, 1.0);
    ASSERT_TRUE(filletR.ok()) << "Fillet failed";
    auto filleted = filletR.value();
    EXPECT_GT(filleted.countSubShapes(TopAbs_FACE), 6);

    // Count how many of the pre-fillet identities still resolve on
    // the post-fillet body. A fillet typically trims at most the two
    // faces adjacent to the filleted edge; the other four faces
    // should carry their pre-fillet identities through.
    int survivors = 0;
    const int postFaces = filleted.countSubShapes(TopAbs_FACE);
    for (int i = 1; i <= postFaces; ++i) {
        auto name = filleted.getElementName(oreo::IndexedName("Face", i));
        auto id   = oreo::ElementMap::extractShapeIdentity(name);
        for (const auto& pre : preIds) {
            if (pre.documentId == id.documentId && pre.counter == id.counter) {
                ++survivors;
                break;
            }
        }
    }
    // At least the four faces not adjacent to the filleted edge should
    // keep their identity. (OCCT's history mapper may in some cases
    // renumber every face, in which case this test has caught a
    // regression — treat the failure as a signal to audit
    // map_shape_elements.cpp's modify-pass handling.)
    EXPECT_GE(survivors, 4)
        << "Fillet should preserve the identities of non-adjacent faces; "
           "only " << survivors << " of the original 6 identities survive.";
}

// ═══════════════════════════════════════════════════════════════
// Feature reorder: breaks downstream refs, moving back recovers
// ═══════════════════════════════════════════════════════════════

TEST(NamingEdgeCases, FeatureReorderBreaksDownstreamRef) {
    auto ctx = oreo::KernelContext::create();
    oreo::FeatureTree tree(ctx);

    // F1: MakeBox
    oreo::Feature f1;
    f1.id = "F1";
    f1.type = "MakeBox";
    f1.params["dimensions"] = gp_Vec(20, 20, 20);
    (void)tree.addFeature(f1);

    // Prime F1 so we have a real shape to crib an edge name from.
    tree.replay();
    auto boxShape = tree.getShapeAt("F1");
    ASSERT_FALSE(boxShape.isNull());
    ASSERT_GE(boxShape.countSubShapes(TopAbs_EDGE), 1);
    auto edgeName = boxShape.getElementName(oreo::IndexedName("Edge", 1));
    ASSERT_FALSE(edgeName.empty());

    // F2: Fillet, referencing F1.Edge1 under the name we just looked up.
    oreo::Feature f2;
    f2.id = "F2";
    f2.type = "Fillet";
    oreo::ElementRef ref{"F1", edgeName.data(), "Edge"};
    f2.params["edges"] = std::vector<oreo::ElementRef>{ref};
    f2.params["radius"] = 1.0;
    (void)tree.addFeature(f2);
    tree.replay();

    {
        const auto* f2p = tree.getFeature("F2");
        ASSERT_NE(f2p, nullptr);
        EXPECT_EQ(f2p->status, oreo::FeatureStatus::OK)
            << "Fillet should succeed with a valid edge ref; errorMessage="
            << f2p->errorMessage;
    }

    // Now move F2 to index 0 (before F1). F2's replay will try to
    // resolve its F1 reference before F1 has run — the tree must
    // report BrokenReference cleanly, not crash, not silently succeed.
    ASSERT_TRUE(tree.moveFeature("F2", 0));
    tree.replay();

    {
        const auto* f2p = tree.getFeature("F2");
        ASSERT_NE(f2p, nullptr);
        EXPECT_EQ(f2p->status, oreo::FeatureStatus::BrokenReference)
            << "Reordering F2 before F1 must surface as BrokenReference; "
               "got status=" << static_cast<int>(f2p->status)
            << " msg=" << f2p->errorMessage;
    }

    // getBrokenFeatures should include F2.
    auto broken = tree.getBrokenFeatures();
    bool includesF2 = false;
    for (const auto* bf : broken) if (bf && bf->id == "F2") includesF2 = true;
    EXPECT_TRUE(includesF2)
        << "getBrokenFeatures() should list the broken feature id=F2";
}

TEST(NamingEdgeCases, FeatureReorderMoveBackRecovers) {
    auto ctx = oreo::KernelContext::create();
    oreo::FeatureTree tree(ctx);

    oreo::Feature f1;
    f1.id = "F1"; f1.type = "MakeBox";
    f1.params["dimensions"] = gp_Vec(20, 20, 20);
    (void)tree.addFeature(f1);
    tree.replay();

    auto box = tree.getShapeAt("F1");
    ASSERT_FALSE(box.isNull());
    auto edgeName = box.getElementName(oreo::IndexedName("Edge", 1));
    ASSERT_FALSE(edgeName.empty());

    oreo::Feature f2;
    f2.id = "F2"; f2.type = "Fillet";
    f2.params["edges"] = std::vector<oreo::ElementRef>{
        oreo::ElementRef{"F1", edgeName.data(), "Edge"}};
    f2.params["radius"] = 1.0;
    (void)tree.addFeature(f2);
    tree.replay();
    ASSERT_EQ(tree.getFeature("F2")->status, oreo::FeatureStatus::OK);

    // Break by reordering.
    ASSERT_TRUE(tree.moveFeature("F2", 0));
    tree.replay();
    ASSERT_EQ(tree.getFeature("F2")->status, oreo::FeatureStatus::BrokenReference);

    // Recover by moving F2 back past F1.
    ASSERT_TRUE(tree.moveFeature("F2", 1));
    tree.replay();
    EXPECT_EQ(tree.getFeature("F2")->status, oreo::FeatureStatus::OK)
        << "Moving F2 back after F1 should restore OK; got "
        << static_cast<int>(tree.getFeature("F2")->status)
        << " msg=" << tree.getFeature("F2")->errorMessage;
}
