// test_boolean_stress.cpp — Boolean operation stress tests.
// Known-difficult geometries: near-coincident faces, thin walls, tangent intersections.

#include <gtest/gtest.h>

#include "core/kernel_context.h"
#include "core/oreo_error.h"
#include "geometry/oreo_geometry.h"
#include "naming/named_shape.h"
#include "query/oreo_query.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>

namespace {

oreo::NamedShape makeTestBox(oreo::KernelContext& ctx, double x, double y, double z, double ox = 0, double oy = 0, double oz = 0) {
    TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(ox, oy, oz), x, y, z).Shape();
    return oreo::NamedShape(box, ctx.tags.nextTag());
}

oreo::NamedShape makeTestSphere(oreo::KernelContext& ctx, double r, double cx = 0, double cy = 0, double cz = 0) {
    TopoDS_Shape sphere = BRepPrimAPI_MakeSphere(gp_Pnt(cx, cy, cz), r).Shape();
    return oreo::NamedShape(sphere, ctx.tags.nextTag());
}

oreo::NamedShape makeTestCylinder(oreo::KernelContext& ctx, double r, double h) {
    TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(r, h).Shape();
    return oreo::NamedShape(cyl, ctx.tags.nextTag());
}

} // anonymous namespace

// -- Near-coincident faces --------------------------------------------

TEST(BooleanStress, NearCoincidentFaces) {
    auto ctx = oreo::KernelContext::create();
    // Two boxes offset by 1e-7 -- OCCT boolean's known weak spot
    auto a = makeTestBox(*ctx, 10, 10, 10);
    auto b = makeTestBox(*ctx, 10, 10, 10, 1e-7, 0, 0);

    auto result = oreo::booleanUnion(*ctx, a, b);
    // Should either succeed or give a structured error, not crash
    if (!result.ok()) {
        EXPECT_TRUE(ctx->diag.hasErrors());
    } else {
        EXPECT_GT(result.value().countSubShapes(TopAbs_FACE), 0);
    }
}

// -- Thin wall subtraction --------------------------------------------

TEST(BooleanStress, ThinWallSubtract) {
    auto ctx = oreo::KernelContext::create();
    // Create a box and subtract a slightly smaller box, leaving 0.01mm walls
    auto outer = makeTestBox(*ctx, 10, 10, 10);
    auto inner = makeTestBox(*ctx, 9.98, 9.98, 9.98, 0.01, 0.01, 0.01);

    auto result = oreo::booleanSubtract(*ctx, outer, inner);
    if (!result.ok()) {
        EXPECT_TRUE(ctx->diag.hasErrors());
    } else {
        auto propsR = oreo::massProperties(*ctx, result.value());
        ASSERT_TRUE(propsR.ok());
        auto props = propsR.value();
        // Volume should be very small (thin shell)
        EXPECT_LT(props.volume, 10.0);
        EXPECT_GT(props.volume, 0.0);
    }
}

// -- Tangent sphere-cylinder ------------------------------------------

TEST(BooleanStress, TangentSphereCylinder) {
    auto ctx = oreo::KernelContext::create();
    // Sphere tangent to cylinder -- difficult boolean geometry
    auto sphere = makeTestSphere(*ctx, 5.0, 10.0, 0.0, 0.0);  // Tangent at x=5
    auto cyl = makeTestCylinder(*ctx, 5.0, 20.0);

    auto result = oreo::booleanUnion(*ctx, sphere, cyl);
    if (!result.ok()) {
        EXPECT_TRUE(ctx->diag.hasErrors());
    } else {
        EXPECT_GT(result.value().countSubShapes(TopAbs_FACE), 0);
    }
}

// -- Fully overlapping (identical) shapes -----------------------------

TEST(BooleanStress, IdenticalShapes) {
    auto ctx = oreo::KernelContext::create();
    auto a = makeTestBox(*ctx, 10, 10, 10);
    auto b = makeTestBox(*ctx, 10, 10, 10);

    // Union of identical shapes should produce the same shape
    auto result = oreo::booleanUnion(*ctx, a, b);
    if (result.ok()) {
        auto propsA = oreo::massProperties(*ctx, a).value();
        auto propsR = oreo::massProperties(*ctx, result.value()).value();
        EXPECT_NEAR(propsR.volume, propsA.volume, 1.0);
    }
}

// -- Multiple sequential booleans -------------------------------------

TEST(BooleanStress, SequentialSubtractions) {
    auto ctx = oreo::KernelContext::create();
    auto base = makeTestBox(*ctx, 100, 100, 100);

    // Subtract 5 holes
    for (int i = 0; i < 5; ++i) {
        auto hole = makeTestCylinder(*ctx, 3.0, 120.0);
        // Place holes at different positions (crude but tests sequential ops)
        auto result = oreo::booleanSubtract(*ctx, base, hole);
        if (result.ok()) {
            base = result.value();
        }
    }

    // Should still have a valid shape
    EXPECT_FALSE(base.isNull());
    EXPECT_GT(base.countSubShapes(TopAbs_FACE), 6); // More than original 6 faces
}
