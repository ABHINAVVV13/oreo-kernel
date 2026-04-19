// SPDX-License-Identifier: LGPL-2.1-or-later

// test_feature_extended.cpp — Coverage for the feature-tree types
// added post-0.9.0-rc1 audit: MakeCone, MakeTorus, MakeWedge, Loft,
// Sweep, BooleanIntersect. Each used to be reachable only via the
// legacy singleton C API; these tests lock the feature-tree
// dispatch + schema validation as the supported path.

#include <gtest/gtest.h>

#include "core/kernel_context.h"
#include "feature/feature.h"
#include "feature/feature_schema.h"
#include "feature/feature_tree.h"
#include "geometry/oreo_geometry.h"
#include "query/oreo_query.h"

#include <gp_Vec.hxx>

#include <vector>

namespace {

oreo::Feature makeFeature(const std::string& id, const std::string& type) {
    oreo::Feature f;
    f.id = id;
    f.type = type;
    return f;
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════
// Schema registration
// ═══════════════════════════════════════════════════════════════

TEST(FeatureExtended, SchemasRegistered) {
    const auto& reg = oreo::FeatureSchemaRegistry::instance();
    for (const char* t : {"MakeCone", "MakeTorus", "MakeWedge",
                          "Loft", "Sweep", "BooleanIntersect"}) {
        EXPECT_NE(reg.find(t), nullptr) << "Schema missing for type: " << t;
    }
}

// ═══════════════════════════════════════════════════════════════
// Primitive dispatch — cone, torus, wedge
// ═══════════════════════════════════════════════════════════════

TEST(FeatureExtended, MakeConeInTree) {
    auto ctx = oreo::KernelContext::create();
    oreo::FeatureTree tree(ctx);

    auto f = makeFeature("F1", "MakeCone");
    f.params["radius1"] = 5.0;
    f.params["radius2"] = 2.0;
    f.params["height"]  = 10.0;
    (void)tree.addFeature(f);

    auto result = tree.replay();
    ASSERT_FALSE(result.isNull()) << "Cone replay produced null shape";
    EXPECT_EQ(tree.getFeature("F1")->status, oreo::FeatureStatus::OK);
    EXPECT_GT(result.countSubShapes(TopAbs_FACE), 0);
}

TEST(FeatureExtended, MakeTorusInTree) {
    auto ctx = oreo::KernelContext::create();
    oreo::FeatureTree tree(ctx);

    auto f = makeFeature("F1", "MakeTorus");
    f.params["majorRadius"] = 10.0;
    f.params["minorRadius"] = 2.0;
    (void)tree.addFeature(f);
    auto result = tree.replay();
    ASSERT_FALSE(result.isNull());
    EXPECT_EQ(tree.getFeature("F1")->status, oreo::FeatureStatus::OK);
}

TEST(FeatureExtended, MakeWedgeInTree) {
    auto ctx = oreo::KernelContext::create();
    oreo::FeatureTree tree(ctx);

    auto f = makeFeature("F1", "MakeWedge");
    f.params["dimensions"] = gp_Vec(10, 8, 6);
    f.params["ltx"] = 4.0;
    (void)tree.addFeature(f);
    auto result = tree.replay();
    ASSERT_FALSE(result.isNull());
    EXPECT_EQ(tree.getFeature("F1")->status, oreo::FeatureStatus::OK);
}

// ═══════════════════════════════════════════════════════════════
// BooleanIntersect
// ═══════════════════════════════════════════════════════════════

TEST(FeatureExtended, BooleanIntersectInTree) {
    auto ctx = oreo::KernelContext::create();
    oreo::FeatureTree tree(ctx);

    // F1: base box 20×20×20
    auto f1 = makeFeature("F1", "MakeBox");
    f1.params["dimensions"] = gp_Vec(20, 20, 20);
    (void)tree.addFeature(f1);

    // F2: cylinder that overlaps — intersect should yield the shared
    // volume. Build it via direct geometry; embed as a Combine
    // "helper" feature that injects the cylinder, then reference it
    // from F3 as the intersect tool.
    //
    // Simpler pattern: add F2 as MakeSphere (so it exists as a feature)
    // and then BooleanIntersect with F2 as tool.
    auto f2 = makeFeature("F2", "MakeSphere");
    f2.params["radius"] = 12.0;
    (void)tree.addFeature(f2);

    // F3: BooleanIntersect(currentShape /*= F2*/, F1 ref)
    // Important: currentShape at F3 IS F2's result (sphere); tool is F1 (box).
    // Wait, actually the "currentShape" is whatever F2 produced.
    // So to Intersect box ∩ sphere, we want F3 = BooleanIntersect with
    // tool=F1 (box) while currentShape is F2 (sphere).
    auto f3 = makeFeature("F3", "BooleanIntersect");
    f3.params["tool"] = oreo::ElementRef{"F1", "ROOT", "Solid"};
    (void)tree.addFeature(f3);

    tree.replay();
    // As of the 2026-04-18 kernel-hardening sprint, whole-shape
    // refs (elementName=="ROOT") resolve to the referenced feature's
    // full NamedShape. Box ∩ sphere must succeed geometrically and the
    // feature's status must reach FeatureStatus::OK.
    const auto* f3p = tree.getFeature("F3");
    ASSERT_NE(f3p, nullptr);
    EXPECT_EQ(f3p->status, oreo::FeatureStatus::OK)
        << "BooleanIntersect(box, sphere) via whole-shape ref must now succeed; "
           "status=" << static_cast<int>(f3p->status)
        << " errorMessage=" << f3p->errorMessage;
}

// ═══════════════════════════════════════════════════════════════
// Loft + Sweep
// ═══════════════════════════════════════════════════════════════
//
// Full end-to-end Loft/Sweep in the feature tree requires ElementRef
// resolution for wires produced by earlier features, which is a
// pre-existing plumbing detail (sketch feature → wire → ref). Until
// the wire-ref pipeline lands we assert schema registration + dispatch
// reachability (no "Unknown feature type" error).

TEST(FeatureExtended, LoftDispatchReachable) {
    auto ctx = oreo::KernelContext::create();
    oreo::FeatureTree tree(ctx);

    // Loft with 0 profiles should fail schema validation (listNonEmpty).
    auto f = makeFeature("F1", "Loft");
    f.params["profiles"] = std::vector<oreo::ElementRef>{};
    (void)tree.addFeature(f);
    tree.replay();
    const auto* fp = tree.getFeature("F1");
    ASSERT_NE(fp, nullptr);
    EXPECT_EQ(fp->status, oreo::FeatureStatus::ExecutionFailed);
    // The failure must come from schema validation, NOT from
    // "Unknown feature type" (which would mean dispatch is missing).
    EXPECT_EQ(fp->errorMessage.find("Unknown feature type"), std::string::npos)
        << "Loft dispatch missing; errorMessage=" << fp->errorMessage;
}

TEST(FeatureExtended, SweepDispatchReachable) {
    auto ctx = oreo::KernelContext::create();
    oreo::FeatureTree tree(ctx);

    auto f = makeFeature("F1", "Sweep");
    // Missing required params; schema validation should fail BEFORE
    // dispatch. Again we just care that "Sweep" is known.
    (void)tree.addFeature(f);
    tree.replay();
    const auto* fp = tree.getFeature("F1");
    ASSERT_NE(fp, nullptr);
    EXPECT_EQ(fp->status, oreo::FeatureStatus::ExecutionFailed);
    EXPECT_EQ(fp->errorMessage.find("Unknown feature type"), std::string::npos)
        << "Sweep dispatch missing; errorMessage=" << fp->errorMessage;
}
