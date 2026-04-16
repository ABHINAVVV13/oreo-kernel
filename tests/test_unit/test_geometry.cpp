// test_geometry.cpp — Unit tests for every geometry API function.

#include <gtest/gtest.h>

#include "core/oreo_error.h"
#include "geometry/oreo_geometry.h"
#include "naming/named_shape.h"
#include "query/oreo_query.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <gp_Circ.hxx>
#include <gp_Pnt.hxx>
#include <TopoDS.hxx>

namespace {

oreo::NamedShape makeBox(double x, double y, double z) {
    TopoDS_Shape box = BRepPrimAPI_MakeBox(x, y, z).Shape();
    return oreo::NamedShape(box, oreo::NamedShape::nextTag());
}

oreo::NamedShape makeRectWire(double w, double h) {
    gp_Pnt p1(0, 0, 0), p2(w, 0, 0), p3(w, h, 0), p4(0, h, 0);
    BRepBuilderAPI_MakeWire wireBuilder;
    wireBuilder.Add(BRepBuilderAPI_MakeEdge(p1, p2).Edge());
    wireBuilder.Add(BRepBuilderAPI_MakeEdge(p2, p3).Edge());
    wireBuilder.Add(BRepBuilderAPI_MakeEdge(p3, p4).Edge());
    wireBuilder.Add(BRepBuilderAPI_MakeEdge(p4, p1).Edge());
    return oreo::NamedShape(wireBuilder.Wire(), oreo::NamedShape::nextTag());
}

oreo::NamedShape makeRectFace(double w, double h) {
    gp_Pnt p1(0, 0, 0), p2(w, 0, 0), p3(w, h, 0), p4(0, h, 0);
    BRepBuilderAPI_MakeWire wireBuilder;
    wireBuilder.Add(BRepBuilderAPI_MakeEdge(p1, p2).Edge());
    wireBuilder.Add(BRepBuilderAPI_MakeEdge(p2, p3).Edge());
    wireBuilder.Add(BRepBuilderAPI_MakeEdge(p3, p4).Edge());
    wireBuilder.Add(BRepBuilderAPI_MakeEdge(p4, p1).Edge());
    BRepBuilderAPI_MakeFace faceBuilder(wireBuilder.Wire());
    return oreo::NamedShape(faceBuilder.Face(), oreo::NamedShape::nextTag());
}

} // anonymous namespace

// ── Extrude ──────────────────────────────────────────────────

TEST(Geometry, ExtrudeRectangle) {
    auto face = makeRectFace(10, 20);
    auto result = oreo::extrude(face, gp_Vec(0, 0, 30));
    ASSERT_FALSE(result.isNull());

    auto box = oreo::aabb(result);
    EXPECT_NEAR(box.xmax - box.xmin, 10.0, 0.01);
    EXPECT_NEAR(box.ymax - box.ymin, 20.0, 0.01);
    EXPECT_NEAR(box.zmax - box.zmin, 30.0, 0.01);
}

TEST(Geometry, ExtrudeNullShape) {
    oreo::NamedShape null;
    auto result = oreo::extrude(null, gp_Vec(0, 0, 10));
    EXPECT_TRUE(result.isNull());
    EXPECT_EQ(oreo::getLastError().code, oreo::ErrorCode::INVALID_INPUT);
}

TEST(Geometry, ExtrudeZeroVector) {
    auto face = makeRectFace(10, 20);
    auto result = oreo::extrude(face, gp_Vec(0, 0, 0));
    EXPECT_TRUE(result.isNull());
    EXPECT_EQ(oreo::getLastError().code, oreo::ErrorCode::INVALID_INPUT);
}

// ── Revolve ──────────────────────────────────────────────────

TEST(Geometry, RevolveFullCircle) {
    auto face = makeRectFace(5, 10);
    gp_Ax1 axis(gp_Pnt(20, 0, 0), gp_Dir(0, 0, 1));
    auto result = oreo::revolve(face, axis, 2.0 * M_PI);
    ASSERT_FALSE(result.isNull());
    // Result should be a toroidal-like shape
    EXPECT_GT(result.countSubShapes(TopAbs_FACE), 0);
}

// ── Boolean Operations ───────────────────────────────────────

TEST(Geometry, BooleanUnionTwoBoxes) {
    auto a = makeBox(10, 10, 10);
    // Second box offset by 5 in X — overlapping
    TopoDS_Shape box2 = BRepPrimAPI_MakeBox(gp_Pnt(5, 0, 0), 10, 10, 10).Shape();
    oreo::NamedShape b(box2, oreo::NamedShape::nextTag());

    auto result = oreo::booleanUnion(a, b);
    ASSERT_FALSE(result.isNull());

    auto bbox = oreo::aabb(result);
    EXPECT_NEAR(bbox.xmax - bbox.xmin, 15.0, 0.01);
    EXPECT_NEAR(bbox.ymax - bbox.ymin, 10.0, 0.01);
}

TEST(Geometry, BooleanSubtract) {
    auto a = makeBox(10, 10, 10);
    TopoDS_Shape box2 = BRepPrimAPI_MakeBox(gp_Pnt(2, 2, -1), 6, 6, 12).Shape();
    oreo::NamedShape b(box2, oreo::NamedShape::nextTag());

    auto result = oreo::booleanSubtract(a, b);
    ASSERT_FALSE(result.isNull());

    // Volume should be less than original
    auto origProps = oreo::massProperties(a);
    auto resultProps = oreo::massProperties(result);
    EXPECT_LT(resultProps.volume, origProps.volume);
}

TEST(Geometry, BooleanIntersect) {
    auto a = makeBox(10, 10, 10);
    TopoDS_Shape box2 = BRepPrimAPI_MakeBox(gp_Pnt(5, 5, 5), 10, 10, 10).Shape();
    oreo::NamedShape b(box2, oreo::NamedShape::nextTag());

    auto result = oreo::booleanIntersect(a, b);
    ASSERT_FALSE(result.isNull());

    auto bbox = oreo::aabb(result);
    EXPECT_NEAR(bbox.xmax - bbox.xmin, 5.0, 0.01);
    EXPECT_NEAR(bbox.ymax - bbox.ymin, 5.0, 0.01);
    EXPECT_NEAR(bbox.zmax - bbox.zmin, 5.0, 0.01);
}

// ── Fillet ────────────────────────────────────────────────────

TEST(Geometry, FilletBox) {
    auto box = makeBox(20, 20, 20);
    auto edges = oreo::getEdges(box);
    ASSERT_GT(edges.size(), 0u);

    // Fillet the first edge
    std::vector<oreo::NamedEdge> filletEdges = {edges[0]};
    auto result = oreo::fillet(box, filletEdges, 2.0);
    ASSERT_FALSE(result.isNull());

    // Filleted box should have more faces than original (6 → 7)
    EXPECT_GT(result.countSubShapes(TopAbs_FACE), box.countSubShapes(TopAbs_FACE));
}

// ── Chamfer ──────────────────────────────────────────────────

TEST(Geometry, ChamferBox) {
    auto box = makeBox(20, 20, 20);
    auto edges = oreo::getEdges(box);
    ASSERT_GT(edges.size(), 0u);

    std::vector<oreo::NamedEdge> chamferEdges = {edges[0]};
    auto result = oreo::chamfer(box, chamferEdges, 3.0);
    ASSERT_FALSE(result.isNull());
    EXPECT_GT(result.countSubShapes(TopAbs_FACE), box.countSubShapes(TopAbs_FACE));
}

// ── Mirror ───────────────────────────────────────────────────

TEST(Geometry, MirrorBox) {
    auto box = makeBox(10, 10, 10);
    gp_Ax2 plane(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0));  // YZ plane
    auto result = oreo::mirror(box, plane);
    ASSERT_FALSE(result.isNull());

    auto bbox = oreo::aabb(result);
    // Mirrored box should be on the negative X side
    EXPECT_LT(bbox.xmin, 0.0);
}

// ── Pattern ──────────────────────────────────────────────────

TEST(Geometry, PatternLinear) {
    auto box = makeBox(5, 5, 5);
    auto result = oreo::patternLinear(box, gp_Vec(1, 0, 0), 3, 10.0);
    ASSERT_FALSE(result.isNull());

    auto bbox = oreo::aabb(result);
    // 3 boxes: 0-5, 10-15, 20-25
    EXPECT_NEAR(bbox.xmax - bbox.xmin, 25.0, 0.1);
}

TEST(Geometry, PatternCircular) {
    auto box = makeBox(5, 5, 5);
    gp_Ax1 axis(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1));
    auto result = oreo::patternCircular(box, axis, 4, 2.0 * M_PI);
    ASSERT_FALSE(result.isNull());
    EXPECT_GT(result.countSubShapes(TopAbs_FACE), box.countSubShapes(TopAbs_FACE));
}

// ── Queries ──────────────────────────────────────────────────

TEST(Query, AABBBox) {
    auto box = makeBox(10, 20, 30);
    auto bbox = oreo::aabb(box);
    EXPECT_NEAR(bbox.xmax - bbox.xmin, 10.0, 0.01);
    EXPECT_NEAR(bbox.ymax - bbox.ymin, 20.0, 0.01);
    EXPECT_NEAR(bbox.zmax - bbox.zmin, 30.0, 0.01);
}

TEST(Query, MassPropertiesBox) {
    auto box = makeBox(10, 20, 30);
    auto props = oreo::massProperties(box);
    EXPECT_NEAR(props.volume, 6000.0, 1.0);
    EXPECT_NEAR(props.centerOfMassX, 5.0, 0.1);
    EXPECT_NEAR(props.centerOfMassY, 10.0, 0.1);
    EXPECT_NEAR(props.centerOfMassZ, 15.0, 0.1);
}

TEST(Query, FaceEdgeCount) {
    auto box = makeBox(10, 10, 10);
    EXPECT_EQ(box.countSubShapes(TopAbs_FACE), 6);
    EXPECT_EQ(box.countSubShapes(TopAbs_EDGE), 12);
    EXPECT_EQ(box.countSubShapes(TopAbs_VERTEX), 8);
}

TEST(Query, MeasureDistance) {
    auto a = makeBox(10, 10, 10);
    TopoDS_Shape box2 = BRepPrimAPI_MakeBox(gp_Pnt(20, 0, 0), 10, 10, 10).Shape();
    oreo::NamedShape b(box2, oreo::NamedShape::nextTag());

    double dist = oreo::measureDistance(a, b);
    EXPECT_NEAR(dist, 10.0, 0.01);
}
