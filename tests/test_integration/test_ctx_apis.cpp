// SPDX-License-Identifier: LGPL-2.1-or-later

// test_ctx_apis.cpp — Acceptance tests for the no-legacy ctx-aware
// sketch (oreo_ctx_sketch_*) and feature-edit (oreo_ctx_feature_*)
// C APIs. Exercised in both OREO_ENABLE_LEGACY_API ON and OFF builds.

#include <gtest/gtest.h>

#include "oreo_kernel.h"

#include <cstring>
#include <string>

namespace {

struct CtxGuard {
    OreoContext h = nullptr;
    explicit CtxGuard(OreoContext c) : h(c) {}
    ~CtxGuard() { if (h) oreo_context_free(h); }
    CtxGuard(const CtxGuard&) = delete;
    CtxGuard& operator=(const CtxGuard&) = delete;
};

struct SolidGuard {
    OreoSolid h = nullptr;
    SolidGuard() = default;
    explicit SolidGuard(OreoSolid s) : h(s) {}
    ~SolidGuard() { if (h) oreo_free_solid(h); }
    SolidGuard(const SolidGuard&) = delete;
    SolidGuard& operator=(const SolidGuard&) = delete;
};

struct WireGuard {
    OreoWire h = nullptr;
    explicit WireGuard(OreoWire w) : h(w) {}
    ~WireGuard() { if (h) oreo_free_wire(h); }
    WireGuard(const WireGuard&) = delete;
    WireGuard& operator=(const WireGuard&) = delete;
};

struct SketchGuard {
    OreoSketch h = nullptr;
    explicit SketchGuard(OreoSketch s) : h(s) {}
    ~SketchGuard() { if (h) oreo_ctx_sketch_free(h); }
    SketchGuard(const SketchGuard&) = delete;
    SketchGuard& operator=(const SketchGuard&) = delete;
};

struct BuilderGuard {
    OreoFeatureBuilder h = nullptr;
    explicit BuilderGuard(OreoFeatureBuilder b) : h(b) {}
    ~BuilderGuard() { if (h) oreo_feature_builder_free(h); }
    BuilderGuard(const BuilderGuard&) = delete;
    BuilderGuard& operator=(const BuilderGuard&) = delete;
};

struct TreeGuard {
    OreoFeatureTree h = nullptr;
    explicit TreeGuard(OreoFeatureTree t) : h(t) {}
    ~TreeGuard() { if (h) oreo_ctx_feature_tree_free(h); }
    TreeGuard(const TreeGuard&) = delete;
    TreeGuard& operator=(const TreeGuard&) = delete;
};

} // anonymous namespace

// ════════════════════════════════════════════════════════════════
// Ctx-aware sketch
// ════════════════════════════════════════════════════════════════

TEST(CtxSketch, CreateAndFree) {
    CtxGuard ctx(oreo_context_create());
    ASSERT_NE(ctx.h, nullptr);
    SketchGuard s(oreo_ctx_sketch_create(ctx.h));
    EXPECT_NE(s.h, nullptr);
}

TEST(CtxSketch, NullContextRejected) {
    EXPECT_EQ(oreo_ctx_sketch_create(nullptr), nullptr);
}

TEST(CtxSketch, AddPointReturnsZeroBasedIndex) {
    CtxGuard ctx(oreo_context_create());
    SketchGuard s(oreo_ctx_sketch_create(ctx.h));
    EXPECT_EQ(oreo_ctx_sketch_add_point(s.h, 0.0, 0.0), 0);
    EXPECT_EQ(oreo_ctx_sketch_add_point(s.h, 1.0, 0.0), 1);
    EXPECT_EQ(oreo_ctx_sketch_add_point(s.h, 0.0, 1.0), 2);
}

TEST(CtxSketch, SolveSimpleHorizontalLine) {
    CtxGuard ctx(oreo_context_create());
    SketchGuard s(oreo_ctx_sketch_create(ctx.h));
    int line = oreo_ctx_sketch_add_line(s.h, 0.0, 0.0, 5.0, 0.5);
    ASSERT_EQ(line, 0);
    // Apply Horizontal constraint to flatten the line. ConstraintType::
    // Horizontal == 13 (per src/sketch/oreo_sketch.h enum order).
    int c = oreo_ctx_sketch_add_constraint(s.h, 13, line, -1, -1, 0.0);
    EXPECT_GE(c, 0);
    int rc = oreo_ctx_sketch_solve(s.h);
    EXPECT_EQ(rc, OREO_OK);

    double x1=0, y1=0, x2=0, y2=0;
    EXPECT_EQ(oreo_ctx_sketch_get_line(s.h, line, &x1, &y1, &x2, &y2), OREO_OK);
    EXPECT_NEAR(y1, y2, 1e-6);
}

TEST(CtxSketch, ToWireProducesNonNullWire) {
    CtxGuard ctx(oreo_context_create());
    SketchGuard s(oreo_ctx_sketch_create(ctx.h));
    oreo_ctx_sketch_add_line(s.h, 0, 0, 10, 0);
    oreo_ctx_sketch_add_line(s.h, 10, 0, 10, 5);
    oreo_ctx_sketch_add_line(s.h, 10, 5, 0, 5);
    oreo_ctx_sketch_add_line(s.h, 0, 5, 0, 0);
    EXPECT_EQ(oreo_ctx_sketch_solve(s.h), OREO_OK);
    WireGuard w(oreo_ctx_sketch_to_wire(s.h));
    EXPECT_NE(w.h, nullptr);
}

// ════════════════════════════════════════════════════════════════
// Feature builder
// ════════════════════════════════════════════════════════════════

TEST(CtxFeatureBuilder, CreateAndFree) {
    BuilderGuard b(oreo_feature_builder_create("F1", "MakeBox"));
    EXPECT_NE(b.h, nullptr);
}

TEST(CtxFeatureBuilder, RejectsNullName) {
    BuilderGuard b(oreo_feature_builder_create("F1", "MakeBox"));
    EXPECT_EQ(oreo_feature_builder_set_double(b.h, nullptr, 1.0),
              OREO_INVALID_INPUT);
}

TEST(CtxFeatureBuilder, ZeroDirRejected) {
    BuilderGuard b(oreo_feature_builder_create("F1", "Extrude"));
    EXPECT_EQ(oreo_feature_builder_set_dir(b.h, "n", 0, 0, 0),
              OREO_INVALID_INPUT);
}

TEST(CtxFeatureBuilder, ResetClearsState) {
    BuilderGuard b(oreo_feature_builder_create("F1", "MakeBox"));
    oreo_feature_builder_set_vec(b.h, "dimensions", 1, 2, 3);
    oreo_feature_builder_reset(b.h, "F2", "MakeSphere");
    // After reset there are no params, so adding the sphere via the
    // tree validator will require setting `radius` again.
    oreo_feature_builder_set_double(b.h, "radius", 4.0);
    // No public way to introspect params, but we can build a tree and
    // see that replay produces a sphere (4 radius).
    CtxGuard ctx(oreo_context_create());
    TreeGuard t(oreo_ctx_feature_tree_create(ctx.h));
    EXPECT_EQ(oreo_ctx_feature_tree_add(t.h, b.h), OREO_OK);
    SolidGuard s(oreo_ctx_feature_tree_replay(t.h));
    EXPECT_NE(s.h, nullptr);
}

TEST(CtxFeatureBuilder, AddRefAppendsToList) {
    BuilderGuard b(oreo_feature_builder_create("F1", "Fillet"));
    EXPECT_EQ(oreo_feature_builder_add_ref(b.h, "edges", "F0", "Edge1", "Edge"),
              OREO_OK);
    EXPECT_EQ(oreo_feature_builder_add_ref(b.h, "edges", "F0", "Edge2", "Edge"),
              OREO_OK);
}

TEST(CtxFeatureBuilder, AddRefMixedWithSetRefRejected) {
    BuilderGuard b(oreo_feature_builder_create("F1", "Fillet"));
    EXPECT_EQ(oreo_feature_builder_set_ref(b.h, "edges", "F0", "Edge1", "Edge"),
              OREO_OK);
    // set_ref wrote ElementRef; add_ref expects ElementRefList.
    EXPECT_EQ(oreo_feature_builder_add_ref(b.h, "edges", "F0", "Edge2", "Edge"),
              OREO_INVALID_STATE);
}

// ════════════════════════════════════════════════════════════════
// Feature tree
// ════════════════════════════════════════════════════════════════

TEST(CtxFeatureTree, CreateAndFree) {
    CtxGuard ctx(oreo_context_create());
    TreeGuard t(oreo_ctx_feature_tree_create(ctx.h));
    EXPECT_NE(t.h, nullptr);
    EXPECT_EQ(oreo_ctx_feature_tree_count(t.h), 0);
}

TEST(CtxFeatureTree, NullContextRejected) {
    EXPECT_EQ(oreo_ctx_feature_tree_create(nullptr), nullptr);
}

TEST(CtxFeatureTree, AddBuilderIncrementsCount) {
    CtxGuard ctx(oreo_context_create());
    TreeGuard t(oreo_ctx_feature_tree_create(ctx.h));
    BuilderGuard b(oreo_feature_builder_create("F1", "MakeBox"));
    oreo_feature_builder_set_vec(b.h, "dimensions", 10, 20, 30);
    EXPECT_EQ(oreo_ctx_feature_tree_add(t.h, b.h), OREO_OK);
    EXPECT_EQ(oreo_ctx_feature_tree_count(t.h), 1);
}

TEST(CtxFeatureTree, DuplicateIdRejected) {
    CtxGuard ctx(oreo_context_create());
    TreeGuard t(oreo_ctx_feature_tree_create(ctx.h));
    BuilderGuard b(oreo_feature_builder_create("F1", "MakeBox"));
    oreo_feature_builder_set_vec(b.h, "dimensions", 10, 20, 30);
    EXPECT_EQ(oreo_ctx_feature_tree_add(t.h, b.h), OREO_OK);
    // Same builder, same id — second add should fail.
    EXPECT_EQ(oreo_ctx_feature_tree_add(t.h, b.h), OREO_INVALID_STATE);
    EXPECT_EQ(oreo_ctx_feature_tree_count(t.h), 1);
}

TEST(CtxFeatureTree, BuilderMissingTypeRejected) {
    CtxGuard ctx(oreo_context_create());
    TreeGuard t(oreo_ctx_feature_tree_create(ctx.h));
    BuilderGuard b(oreo_feature_builder_create("F1", ""));
    EXPECT_EQ(oreo_ctx_feature_tree_add(t.h, b.h), OREO_INVALID_INPUT);
}

TEST(CtxFeatureTree, RemoveDecrementsCount) {
    CtxGuard ctx(oreo_context_create());
    TreeGuard t(oreo_ctx_feature_tree_create(ctx.h));
    BuilderGuard b(oreo_feature_builder_create("F1", "MakeBox"));
    oreo_feature_builder_set_vec(b.h, "dimensions", 10, 20, 30);
    oreo_ctx_feature_tree_add(t.h, b.h);
    EXPECT_EQ(oreo_ctx_feature_tree_remove(t.h, "F1"), OREO_OK);
    EXPECT_EQ(oreo_ctx_feature_tree_count(t.h), 0);
}

TEST(CtxFeatureTree, RemoveUnknownReturnsInvalidInput) {
    CtxGuard ctx(oreo_context_create());
    TreeGuard t(oreo_ctx_feature_tree_create(ctx.h));
    EXPECT_EQ(oreo_ctx_feature_tree_remove(t.h, "Nope"), OREO_INVALID_INPUT);
}

TEST(CtxFeatureTree, ReplayMakeBoxReturnsSolid) {
    CtxGuard ctx(oreo_context_create());
    TreeGuard t(oreo_ctx_feature_tree_create(ctx.h));
    BuilderGuard b(oreo_feature_builder_create("F1", "MakeBox"));
    oreo_feature_builder_set_vec(b.h, "dimensions", 10, 20, 30);
    ASSERT_EQ(oreo_ctx_feature_tree_add(t.h, b.h), OREO_OK);

    SolidGuard solid(oreo_ctx_feature_tree_replay(t.h));
    ASSERT_NE(solid.h, nullptr);
    OreoBBox bb = oreo_ctx_aabb(ctx.h, solid.h);
    EXPECT_NEAR(bb.xmax - bb.xmin, 10.0, 1e-6);
    EXPECT_NEAR(bb.ymax - bb.ymin, 20.0, 1e-6);
    EXPECT_NEAR(bb.zmax - bb.zmin, 30.0, 1e-6);
}

TEST(CtxFeatureTree, ValidateCatchesMissingRequiredParam) {
    CtxGuard ctx(oreo_context_create());
    TreeGuard t(oreo_ctx_feature_tree_create(ctx.h));
    // MakeSphere requires 'radius' — leave it unset.
    BuilderGuard b(oreo_feature_builder_create("F1", "MakeSphere"));
    ASSERT_EQ(oreo_ctx_feature_tree_add(t.h, b.h), OREO_OK);
    EXPECT_EQ(oreo_ctx_feature_tree_validate(t.h), OREO_INVALID_INPUT);
    EXPECT_TRUE(oreo_context_has_errors(ctx.h));
}

TEST(CtxFeatureTree, ValidateCatchesNegativeRadius) {
    CtxGuard ctx(oreo_context_create());
    TreeGuard t(oreo_ctx_feature_tree_create(ctx.h));
    BuilderGuard b(oreo_feature_builder_create("F1", "MakeSphere"));
    oreo_feature_builder_set_double(b.h, "radius", -5.0);
    ASSERT_EQ(oreo_ctx_feature_tree_add(t.h, b.h), OREO_OK);
    EXPECT_EQ(oreo_ctx_feature_tree_validate(t.h), OREO_INVALID_INPUT);
}

TEST(CtxFeatureTree, ReplayWithBadParamFails) {
    CtxGuard ctx(oreo_context_create());
    TreeGuard t(oreo_ctx_feature_tree_create(ctx.h));
    BuilderGuard b(oreo_feature_builder_create("F1", "MakeSphere"));
    oreo_feature_builder_set_double(b.h, "radius", -5.0);
    ASSERT_EQ(oreo_ctx_feature_tree_add(t.h, b.h), OREO_OK);
    SolidGuard s(oreo_ctx_feature_tree_replay(t.h));
    EXPECT_EQ(s.h, nullptr);
    EXPECT_TRUE(oreo_context_has_errors(ctx.h));
}

TEST(CtxFeatureTree, SetParamDoubleUpdatesValue) {
    CtxGuard ctx(oreo_context_create());
    TreeGuard t(oreo_ctx_feature_tree_create(ctx.h));
    BuilderGuard b(oreo_feature_builder_create("F1", "MakeSphere"));
    oreo_feature_builder_set_double(b.h, "radius", 5.0);
    ASSERT_EQ(oreo_ctx_feature_tree_add(t.h, b.h), OREO_OK);

    SolidGuard s5(oreo_ctx_feature_tree_replay(t.h));
    ASSERT_NE(s5.h, nullptr);
    OreoBBox bb5 = oreo_ctx_aabb(ctx.h, s5.h);
    EXPECT_NEAR(bb5.xmax - bb5.xmin, 10.0, 1e-3);

    EXPECT_EQ(oreo_ctx_feature_tree_set_param_double(t.h, "F1", "radius", 7.5),
              OREO_OK);
    SolidGuard s7(oreo_ctx_feature_tree_replay(t.h));
    ASSERT_NE(s7.h, nullptr);
    OreoBBox bb7 = oreo_ctx_aabb(ctx.h, s7.h);
    EXPECT_NEAR(bb7.xmax - bb7.xmin, 15.0, 1e-3);
}

TEST(CtxFeatureTree, SuppressShortcutsExecution) {
    CtxGuard ctx(oreo_context_create());
    TreeGuard t(oreo_ctx_feature_tree_create(ctx.h));
    BuilderGuard b(oreo_feature_builder_create("F1", "MakeBox"));
    oreo_feature_builder_set_vec(b.h, "dimensions", 4, 4, 4);
    ASSERT_EQ(oreo_ctx_feature_tree_add(t.h, b.h), OREO_OK);
    EXPECT_EQ(oreo_ctx_feature_tree_suppress(t.h, "F1", 1), OREO_OK);
    // Sole feature suppressed — replay should produce no shape.
    SolidGuard s(oreo_ctx_feature_tree_replay(t.h));
    EXPECT_EQ(s.h, nullptr);
}
