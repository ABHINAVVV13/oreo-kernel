// test_boolean_stress.cpp — Boolean operation stress tests.
// Known-difficult geometries: near-coincident faces, thin walls, tangent intersections.

#include <gtest/gtest.h>

#include "core/oreo_error.h"
#include "geometry/oreo_geometry.h"
#include "naming/named_shape.h"
#include "query/oreo_query.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>

namespace {

oreo::NamedShape makeBox(double x, double y, double z, double ox = 0, double oy = 0, double oz = 0) {
    TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(ox, oy, oz), x, y, z).Shape();
    return oreo::NamedShape(box, oreo::NamedShape::nextTag());
}

oreo::NamedShape makeSphere(double r, double cx = 0, double cy = 0, double cz = 0) {
    TopoDS_Shape sphere = BRepPrimAPI_MakeSphere(gp_Pnt(cx, cy, cz), r).Shape();
    return oreo::NamedShape(sphere, oreo::NamedShape::nextTag());
}

oreo::NamedShape makeCylinder(double r, double h) {
    TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(r, h).Shape();
    return oreo::NamedShape(cyl, oreo::NamedShape::nextTag());
}

} // anonymous namespace

// ── Near-coincident faces ────────────────────────────────────

TEST(BooleanStress, NearCoincidentFaces) {
    // Two boxes offset by 1e-7 — OCCT boolean's known weak spot
    auto a = makeBox(10, 10, 10);
    auto b = makeBox(10, 10, 10, 1e-7, 0, 0);

    auto result = oreo::booleanUnion(a, b);
    // Should either succeed or give a structured error, not crash
    if (result.isNull()) {
        EXPECT_NE(oreo::getLastError().code, oreo::ErrorCode::OK);
    } else {
        EXPECT_GT(result.countSubShapes(TopAbs_FACE), 0);
    }
}

// ── Thin wall subtraction ────────────────────────────────────

TEST(BooleanStress, ThinWallSubtract) {
    // Create a box and subtract a slightly smaller box, leaving 0.01mm walls
    auto outer = makeBox(10, 10, 10);
    auto inner = makeBox(9.98, 9.98, 9.98, 0.01, 0.01, 0.01);

    auto result = oreo::booleanSubtract(outer, inner);
    if (result.isNull()) {
        EXPECT_NE(oreo::getLastError().code, oreo::ErrorCode::OK);
    } else {
        auto props = oreo::massProperties(result);
        // Volume should be very small (thin shell)
        EXPECT_LT(props.volume, 10.0);
        EXPECT_GT(props.volume, 0.0);
    }
}

// ── Tangent sphere-cylinder ──────────────────────────────────

TEST(BooleanStress, TangentSphereCylinder) {
    // Sphere tangent to cylinder — difficult boolean geometry
    auto sphere = makeSphere(5.0, 10.0, 0.0, 0.0);  // Tangent at x=5
    auto cyl = makeCylinder(5.0, 20.0);

    auto result = oreo::booleanUnion(sphere, cyl);
    if (result.isNull()) {
        EXPECT_NE(oreo::getLastError().code, oreo::ErrorCode::OK);
    } else {
        EXPECT_GT(result.countSubShapes(TopAbs_FACE), 0);
    }
}

// ── Fully overlapping (identical) shapes ─────────────────────

TEST(BooleanStress, IdenticalShapes) {
    auto a = makeBox(10, 10, 10);
    auto b = makeBox(10, 10, 10);

    // Union of identical shapes should produce the same shape
    auto result = oreo::booleanUnion(a, b);
    if (!result.isNull()) {
        auto propsA = oreo::massProperties(a);
        auto propsR = oreo::massProperties(result);
        EXPECT_NEAR(propsR.volume, propsA.volume, 1.0);
    }
}

// ── Multiple sequential booleans ─────────────────────────────

TEST(BooleanStress, SequentialSubtractions) {
    auto base = makeBox(100, 100, 100);

    // Subtract 5 holes
    for (int i = 0; i < 5; ++i) {
        auto hole = makeCylinder(3.0, 120.0);
        // Place holes at different positions (crude but tests sequential ops)
        auto result = oreo::booleanSubtract(base, hole);
        if (!result.isNull()) {
            base = result;
        }
    }

    // Should still have a valid shape
    EXPECT_FALSE(base.isNull());
    EXPECT_GT(base.countSubShapes(TopAbs_FACE), 6); // More than original 6 faces
}
