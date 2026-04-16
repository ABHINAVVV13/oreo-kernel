// test_robustness.cpp — Crash/malformed input tests.
// Verify no segfaults, only structured errors.

#include <gtest/gtest.h>

#include "core/kernel_context.h"
#include "core/oreo_error.h"
#include "geometry/oreo_geometry.h"
#include "io/oreo_serialize.h"
#include "io/oreo_step.h"
#include "naming/named_shape.h"
#include "query/oreo_query.h"

#include <BRepPrimAPI_MakeBox.hxx>

// -- Null handle operations should produce errors, not crashes --------

TEST(Robustness, ExtrudeNull) {
    auto ctx = oreo::KernelContext::create();
    oreo::NamedShape null;
    auto result = oreo::extrude(*ctx, null, gp_Vec(0, 0, 10));
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(ctx->diag.hasErrors());
}

TEST(Robustness, RevolveNull) {
    auto ctx = oreo::KernelContext::create();
    oreo::NamedShape null;
    gp_Ax1 axis(gp_Pnt(0,0,0), gp_Dir(0,0,1));
    auto result = oreo::revolve(*ctx, null, axis, 1.0);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(ctx->diag.hasErrors());
}

TEST(Robustness, BooleanWithNull) {
    auto ctx = oreo::KernelContext::create();
    oreo::NamedShape null;
    oreo::NamedShape box(BRepPrimAPI_MakeBox(10,10,10).Shape(), 1);

    auto r1 = oreo::booleanUnion(*ctx, null, box);
    EXPECT_FALSE(r1.ok());

    ctx->diag.clear();
    auto r2 = oreo::booleanSubtract(*ctx, box, null);
    EXPECT_FALSE(r2.ok());

    ctx->diag.clear();
    auto r3 = oreo::booleanIntersect(*ctx, null, null);
    EXPECT_FALSE(r3.ok());
}

TEST(Robustness, FilletEmptyEdges) {
    auto ctx = oreo::KernelContext::create();
    oreo::NamedShape box(BRepPrimAPI_MakeBox(10,10,10).Shape(), 1);
    std::vector<oreo::NamedEdge> empty;
    auto result = oreo::fillet(*ctx, box, empty, 2.0);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(ctx->diag.hasErrors());
}

TEST(Robustness, FilletZeroRadius) {
    auto ctx = oreo::KernelContext::create();
    oreo::NamedShape box(BRepPrimAPI_MakeBox(10,10,10).Shape(), 1);
    auto edges = oreo::getEdges(*ctx, box);
    ASSERT_GT(edges.size(), 0u);

    std::vector<oreo::NamedEdge> filletEdges = {edges[0]};
    auto result = oreo::fillet(*ctx, box, filletEdges, 0.0);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(ctx->diag.hasErrors());
}

TEST(Robustness, ChamferNegativeDistance) {
    auto ctx = oreo::KernelContext::create();
    oreo::NamedShape box(BRepPrimAPI_MakeBox(10,10,10).Shape(), 1);
    auto edges = oreo::getEdges(*ctx, box);
    ASSERT_GT(edges.size(), 0u);

    std::vector<oreo::NamedEdge> chamferEdges = {edges[0]};
    auto result = oreo::chamfer(*ctx, box, chamferEdges, -5.0);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(ctx->diag.hasErrors());
}

TEST(Robustness, PatternCountOne) {
    auto ctx = oreo::KernelContext::create();
    oreo::NamedShape box(BRepPrimAPI_MakeBox(10,10,10).Shape(), 1);
    auto result = oreo::patternLinear(*ctx, box, gp_Vec(1,0,0), 1, 10.0);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(ctx->diag.hasErrors());
}

TEST(Robustness, PatternZeroSpacing) {
    auto ctx = oreo::KernelContext::create();
    oreo::NamedShape box(BRepPrimAPI_MakeBox(10,10,10).Shape(), 1);
    auto result = oreo::patternLinear(*ctx, box, gp_Vec(1,0,0), 3, 0.0);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(ctx->diag.hasErrors());
}

// -- Queries on null shapes -------------------------------------------

TEST(Robustness, AABBNull) {
    auto ctx = oreo::KernelContext::create();
    oreo::NamedShape null;
    auto bbox = oreo::aabb(*ctx, null);
    EXPECT_TRUE(ctx->diag.hasErrors());
}

TEST(Robustness, MassPropertiesNull) {
    auto ctx = oreo::KernelContext::create();
    oreo::NamedShape null;
    auto props = oreo::massProperties(*ctx, null);
    EXPECT_TRUE(ctx->diag.hasErrors());
}

TEST(Robustness, MeasureDistanceNull) {
    auto ctx = oreo::KernelContext::create();
    oreo::NamedShape null;
    oreo::NamedShape box(BRepPrimAPI_MakeBox(10,10,10).Shape(), 1);
    double dist = oreo::measureDistance(*ctx, null, box);
    EXPECT_LT(dist, 0.0);
    EXPECT_TRUE(ctx->diag.hasErrors());
}

// -- Serialization robustness -----------------------------------------

TEST(Robustness, DeserializeEmpty) {
    auto ctx = oreo::KernelContext::create();
    auto result = oreo::deserialize(*ctx, nullptr, 0);
    EXPECT_TRUE(result.isNull());
}

TEST(Robustness, DeserializeGarbage) {
    auto ctx = oreo::KernelContext::create();
    uint8_t garbage[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
                         0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
    auto result = oreo::deserialize(*ctx, garbage, sizeof(garbage));
    // Should not crash -- either returns null or a shape
    if (result.isNull()) {
        SUCCEED();
    }
}

// -- STEP robustness --------------------------------------------------

TEST(Robustness, ImportStepNull) {
    auto ctx = oreo::KernelContext::create();
    auto result = oreo::importStep(*ctx, nullptr, 0);
    EXPECT_TRUE(result.isNull());
}

TEST(Robustness, ImportStepNonexistentFile) {
    auto ctx = oreo::KernelContext::create();
    auto result = oreo::importStepFile(*ctx, "this_file_does_not_exist.step");
    EXPECT_TRUE(result.isNull());
    EXPECT_TRUE(ctx->diag.hasErrors());
}

TEST(Robustness, ImportStepShapeLegacy) {
    auto ctx = oreo::KernelContext::create();
    // Legacy API should also handle null gracefully
    auto result = oreo::importStepShape(*ctx, nullptr, 0);
    EXPECT_TRUE(result.isNull());
}
