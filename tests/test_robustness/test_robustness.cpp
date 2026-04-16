// test_robustness.cpp — Crash/malformed input tests.
// Verify no segfaults, only structured errors.

#include <gtest/gtest.h>

#include "core/oreo_error.h"
#include "geometry/oreo_geometry.h"
#include "io/oreo_serialize.h"
#include "io/oreo_step.h"
#include "naming/named_shape.h"
#include "query/oreo_query.h"

#include <BRepPrimAPI_MakeBox.hxx>

// ── Null handle operations should produce errors, not crashes ─

TEST(Robustness, ExtrudeNull) {
    oreo::NamedShape null;
    auto result = oreo::extrude(null, gp_Vec(0, 0, 10));
    EXPECT_TRUE(result.isNull());
    EXPECT_EQ(oreo::getLastError().code, oreo::ErrorCode::INVALID_INPUT);
}

TEST(Robustness, RevolveNull) {
    oreo::NamedShape null;
    gp_Ax1 axis(gp_Pnt(0,0,0), gp_Dir(0,0,1));
    auto result = oreo::revolve(null, axis, 1.0);
    EXPECT_TRUE(result.isNull());
    EXPECT_EQ(oreo::getLastError().code, oreo::ErrorCode::INVALID_INPUT);
}

TEST(Robustness, BooleanWithNull) {
    oreo::NamedShape null;
    oreo::NamedShape box(BRepPrimAPI_MakeBox(10,10,10).Shape(), 1);

    auto r1 = oreo::booleanUnion(null, box);
    EXPECT_TRUE(r1.isNull());

    auto r2 = oreo::booleanSubtract(box, null);
    EXPECT_TRUE(r2.isNull());

    auto r3 = oreo::booleanIntersect(null, null);
    EXPECT_TRUE(r3.isNull());
}

TEST(Robustness, FilletEmptyEdges) {
    oreo::NamedShape box(BRepPrimAPI_MakeBox(10,10,10).Shape(), 1);
    std::vector<oreo::NamedEdge> empty;
    auto result = oreo::fillet(box, empty, 2.0);
    EXPECT_TRUE(result.isNull());
    EXPECT_EQ(oreo::getLastError().code, oreo::ErrorCode::INVALID_INPUT);
}

TEST(Robustness, FilletZeroRadius) {
    oreo::NamedShape box(BRepPrimAPI_MakeBox(10,10,10).Shape(), 1);
    auto edges = oreo::getEdges(box);
    ASSERT_GT(edges.size(), 0u);

    std::vector<oreo::NamedEdge> filletEdges = {edges[0]};
    auto result = oreo::fillet(box, filletEdges, 0.0);
    EXPECT_TRUE(result.isNull());
    EXPECT_EQ(oreo::getLastError().code, oreo::ErrorCode::INVALID_INPUT);
}

TEST(Robustness, ChamferNegativeDistance) {
    oreo::NamedShape box(BRepPrimAPI_MakeBox(10,10,10).Shape(), 1);
    auto edges = oreo::getEdges(box);
    ASSERT_GT(edges.size(), 0u);

    std::vector<oreo::NamedEdge> chamferEdges = {edges[0]};
    auto result = oreo::chamfer(box, chamferEdges, -5.0);
    EXPECT_TRUE(result.isNull());
    EXPECT_EQ(oreo::getLastError().code, oreo::ErrorCode::INVALID_INPUT);
}

TEST(Robustness, PatternCountOne) {
    oreo::NamedShape box(BRepPrimAPI_MakeBox(10,10,10).Shape(), 1);
    auto result = oreo::patternLinear(box, gp_Vec(1,0,0), 1, 10.0);
    EXPECT_TRUE(result.isNull());
    EXPECT_EQ(oreo::getLastError().code, oreo::ErrorCode::INVALID_INPUT);
}

TEST(Robustness, PatternZeroSpacing) {
    oreo::NamedShape box(BRepPrimAPI_MakeBox(10,10,10).Shape(), 1);
    auto result = oreo::patternLinear(box, gp_Vec(1,0,0), 3, 0.0);
    EXPECT_TRUE(result.isNull());
    EXPECT_EQ(oreo::getLastError().code, oreo::ErrorCode::INVALID_INPUT);
}

// ── Queries on null shapes ───────────────────────────────────

TEST(Robustness, AABBNull) {
    oreo::NamedShape null;
    auto bbox = oreo::aabb(null);
    EXPECT_EQ(oreo::getLastError().code, oreo::ErrorCode::INVALID_INPUT);
}

TEST(Robustness, MassPropertiesNull) {
    oreo::NamedShape null;
    auto props = oreo::massProperties(null);
    EXPECT_EQ(oreo::getLastError().code, oreo::ErrorCode::INVALID_INPUT);
}

TEST(Robustness, MeasureDistanceNull) {
    oreo::NamedShape null;
    oreo::NamedShape box(BRepPrimAPI_MakeBox(10,10,10).Shape(), 1);
    double dist = oreo::measureDistance(null, box);
    EXPECT_LT(dist, 0.0);
    EXPECT_EQ(oreo::getLastError().code, oreo::ErrorCode::INVALID_INPUT);
}

// ── Serialization robustness ─────────────────────────────────

TEST(Robustness, DeserializeEmpty) {
    auto result = oreo::deserialize(nullptr, 0);
    EXPECT_TRUE(result.isNull());
}

TEST(Robustness, DeserializeGarbage) {
    uint8_t garbage[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
                         0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
    auto result = oreo::deserialize(garbage, sizeof(garbage));
    // Should not crash — either returns null or a shape
    if (result.isNull()) {
        SUCCEED();
    }
}

// ── STEP robustness ──────────────────────────────────────────

TEST(Robustness, ImportStepNull) {
    auto result = oreo::importStep(nullptr, 0);
    EXPECT_TRUE(result.isNull());
}

TEST(Robustness, ImportStepNonexistentFile) {
    auto result = oreo::importStepFile("this_file_does_not_exist.step");
    EXPECT_TRUE(result.isNull());
    EXPECT_EQ(oreo::getLastError().code, oreo::ErrorCode::STEP_IMPORT_FAILED);
}

TEST(Robustness, ImportStepShapeLegacy) {
    // Legacy API should also handle null gracefully
    auto result = oreo::importStepShape(nullptr, 0);
    EXPECT_TRUE(result.isNull());
}
