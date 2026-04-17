// SPDX-License-Identifier: LGPL-2.1-or-later

// test_geometry.cpp — Unit tests for every geometry API function.

#include <gtest/gtest.h>

#include "core/kernel_context.h"
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

oreo::NamedShape makeRectWire(oreo::KernelContext& ctx, double w, double h) {
    gp_Pnt p1(0, 0, 0), p2(w, 0, 0), p3(w, h, 0), p4(0, h, 0);
    BRepBuilderAPI_MakeWire wireBuilder;
    wireBuilder.Add(BRepBuilderAPI_MakeEdge(p1, p2).Edge());
    wireBuilder.Add(BRepBuilderAPI_MakeEdge(p2, p3).Edge());
    wireBuilder.Add(BRepBuilderAPI_MakeEdge(p3, p4).Edge());
    wireBuilder.Add(BRepBuilderAPI_MakeEdge(p4, p1).Edge());
    return oreo::NamedShape(wireBuilder.Wire(), ctx.tags().nextTag());
}

oreo::NamedShape makeRectFace(oreo::KernelContext& ctx, double w, double h) {
    gp_Pnt p1(0, 0, 0), p2(w, 0, 0), p3(w, h, 0), p4(0, h, 0);
    BRepBuilderAPI_MakeWire wireBuilder;
    wireBuilder.Add(BRepBuilderAPI_MakeEdge(p1, p2).Edge());
    wireBuilder.Add(BRepBuilderAPI_MakeEdge(p2, p3).Edge());
    wireBuilder.Add(BRepBuilderAPI_MakeEdge(p3, p4).Edge());
    wireBuilder.Add(BRepBuilderAPI_MakeEdge(p4, p1).Edge());
    BRepBuilderAPI_MakeFace faceBuilder(wireBuilder.Wire());
    return oreo::NamedShape(faceBuilder.Face(), ctx.tags().nextTag());
}

} // anonymous namespace

// -- Extrude ----------------------------------------------------------

TEST(Geometry, ExtrudeRectangle) {
    auto ctx = oreo::KernelContext::create();
    auto face = makeRectFace(*ctx, 10, 20);
    auto resultR = oreo::extrude(*ctx, face, gp_Vec(0, 0, 30));
    ASSERT_TRUE(resultR.ok());
    auto result = resultR.value();

    auto boxR = oreo::aabb(*ctx, result);
    ASSERT_TRUE(boxR.ok());
    auto box = boxR.value();
    EXPECT_NEAR(box.xmax - box.xmin, 10.0, 0.01);
    EXPECT_NEAR(box.ymax - box.ymin, 20.0, 0.01);
    EXPECT_NEAR(box.zmax - box.zmin, 30.0, 0.01);
}

TEST(Geometry, ExtrudeNullShape) {
    auto ctx = oreo::KernelContext::create();
    oreo::NamedShape null;
    auto result = oreo::extrude(*ctx, null, gp_Vec(0, 0, 10));
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(ctx->diag().hasErrors());
}

TEST(Geometry, ExtrudeZeroVector) {
    auto ctx = oreo::KernelContext::create();
    auto face = makeRectFace(*ctx, 10, 20);
    auto result = oreo::extrude(*ctx, face, gp_Vec(0, 0, 0));
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(ctx->diag().hasErrors());
}

// -- Revolve ----------------------------------------------------------

TEST(Geometry, RevolveFullCircle) {
    auto ctx = oreo::KernelContext::create();
    auto face = makeRectFace(*ctx, 5, 10);
    gp_Ax1 axis(gp_Pnt(20, 0, 0), gp_Dir(0, 0, 1));
    auto resultR = oreo::revolve(*ctx, face, axis, 2.0 * M_PI);
    ASSERT_TRUE(resultR.ok());
    auto result = resultR.value();
    // Result should be a toroidal-like shape
    EXPECT_GT(result.countSubShapes(TopAbs_FACE), 0);
}

// -- Boolean Operations -----------------------------------------------

TEST(Geometry, BooleanUnionTwoBoxes) {
    auto ctx = oreo::KernelContext::create();
    auto aR = oreo::makeBox(*ctx, 10, 10, 10);
    ASSERT_TRUE(aR.ok());
    auto a = aR.value();
    // Second box offset by 5 in X -- overlapping
    TopoDS_Shape box2 = BRepPrimAPI_MakeBox(gp_Pnt(5, 0, 0), 10, 10, 10).Shape();
    oreo::NamedShape b(box2, ctx->tags().nextTag());

    auto resultR = oreo::booleanUnion(*ctx, a, b);
    ASSERT_TRUE(resultR.ok());
    auto result = resultR.value();

    auto bboxR = oreo::aabb(*ctx, result);
    ASSERT_TRUE(bboxR.ok());
    auto bbox = bboxR.value();
    EXPECT_NEAR(bbox.xmax - bbox.xmin, 15.0, 0.01);
    EXPECT_NEAR(bbox.ymax - bbox.ymin, 10.0, 0.01);
}

TEST(Geometry, BooleanSubtract) {
    auto ctx = oreo::KernelContext::create();
    auto aR = oreo::makeBox(*ctx, 10, 10, 10);
    ASSERT_TRUE(aR.ok());
    auto a = aR.value();
    TopoDS_Shape box2 = BRepPrimAPI_MakeBox(gp_Pnt(2, 2, -1), 6, 6, 12).Shape();
    oreo::NamedShape b(box2, ctx->tags().nextTag());

    auto resultR = oreo::booleanSubtract(*ctx, a, b);
    ASSERT_TRUE(resultR.ok());
    auto result = resultR.value();

    // Volume should be less than original
    auto origPropsR = oreo::massProperties(*ctx, a);
    ASSERT_TRUE(origPropsR.ok());
    auto origProps = origPropsR.value();
    auto resultPropsR = oreo::massProperties(*ctx, result);
    ASSERT_TRUE(resultPropsR.ok());
    auto resultProps = resultPropsR.value();
    EXPECT_LT(resultProps.volume, origProps.volume);
}

TEST(Geometry, BooleanIntersect) {
    auto ctx = oreo::KernelContext::create();
    auto aR = oreo::makeBox(*ctx, 10, 10, 10);
    ASSERT_TRUE(aR.ok());
    auto a = aR.value();
    TopoDS_Shape box2 = BRepPrimAPI_MakeBox(gp_Pnt(5, 5, 5), 10, 10, 10).Shape();
    oreo::NamedShape b(box2, ctx->tags().nextTag());

    auto resultR = oreo::booleanIntersect(*ctx, a, b);
    ASSERT_TRUE(resultR.ok());
    auto result = resultR.value();

    auto bboxR = oreo::aabb(*ctx, result);
    ASSERT_TRUE(bboxR.ok());
    auto bbox = bboxR.value();
    EXPECT_NEAR(bbox.xmax - bbox.xmin, 5.0, 0.01);
    EXPECT_NEAR(bbox.ymax - bbox.ymin, 5.0, 0.01);
    EXPECT_NEAR(bbox.zmax - bbox.zmin, 5.0, 0.01);
}

// -- Fillet ------------------------------------------------------------

TEST(Geometry, FilletBox) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 20, 20, 20).value();
    auto edgesR = oreo::getEdges(*ctx, box);
    ASSERT_TRUE(edgesR.ok());
    auto edges = edgesR.value();
    ASSERT_GT(edges.size(), 0u);

    // Fillet the first edge
    std::vector<oreo::NamedEdge> filletEdges = {edges[0]};
    auto resultR = oreo::fillet(*ctx, box, filletEdges, 2.0);
    ASSERT_TRUE(resultR.ok());
    auto result = resultR.value();

    // Filleted box should have more faces than original (6 -> 7)
    EXPECT_GT(result.countSubShapes(TopAbs_FACE), box.countSubShapes(TopAbs_FACE));
}

// -- Chamfer ----------------------------------------------------------

TEST(Geometry, ChamferBox) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 20, 20, 20).value();
    auto edgesR = oreo::getEdges(*ctx, box);
    ASSERT_TRUE(edgesR.ok());
    auto edges = edgesR.value();
    ASSERT_GT(edges.size(), 0u);

    std::vector<oreo::NamedEdge> chamferEdges = {edges[0]};
    auto resultR = oreo::chamfer(*ctx, box, chamferEdges, 3.0);
    ASSERT_TRUE(resultR.ok());
    auto result = resultR.value();
    EXPECT_GT(result.countSubShapes(TopAbs_FACE), box.countSubShapes(TopAbs_FACE));
}

// -- Mirror -----------------------------------------------------------

TEST(Geometry, MirrorBox) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 10, 10).value();
    gp_Ax2 plane(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0));  // YZ plane
    auto resultR = oreo::mirror(*ctx, box, plane);
    ASSERT_TRUE(resultR.ok());
    auto result = resultR.value();

    auto bboxR = oreo::aabb(*ctx, result);
    ASSERT_TRUE(bboxR.ok());
    auto bbox = bboxR.value();
    // Mirrored box should be on the negative X side
    EXPECT_LT(bbox.xmin, 0.0);
}

// -- Pattern ----------------------------------------------------------

TEST(Geometry, PatternLinear) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 5, 5, 5).value();
    auto resultR = oreo::patternLinear(*ctx, box, gp_Vec(1, 0, 0), 3, 10.0);
    ASSERT_TRUE(resultR.ok());
    auto result = resultR.value();

    auto bboxR = oreo::aabb(*ctx, result);
    ASSERT_TRUE(bboxR.ok());
    auto bbox = bboxR.value();
    // 3 boxes: 0-5, 10-15, 20-25
    EXPECT_NEAR(bbox.xmax - bbox.xmin, 25.0, 0.1);
}

TEST(Geometry, PatternCircular) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 5, 5, 5).value();
    gp_Ax1 axis(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1));
    auto resultR = oreo::patternCircular(*ctx, box, axis, 4, 2.0 * M_PI);
    ASSERT_TRUE(resultR.ok());
    auto result = resultR.value();
    EXPECT_GT(result.countSubShapes(TopAbs_FACE), box.countSubShapes(TopAbs_FACE));
}

// -- Queries ----------------------------------------------------------

TEST(Query, AABBBox) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 20, 30).value();
    auto bboxR = oreo::aabb(*ctx, box);
    ASSERT_TRUE(bboxR.ok());
    auto bbox = bboxR.value();
    EXPECT_NEAR(bbox.xmax - bbox.xmin, 10.0, 0.01);
    EXPECT_NEAR(bbox.ymax - bbox.ymin, 20.0, 0.01);
    EXPECT_NEAR(bbox.zmax - bbox.zmin, 30.0, 0.01);
}

TEST(Query, MassPropertiesBox) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 20, 30).value();
    auto propsR = oreo::massProperties(*ctx, box);
    ASSERT_TRUE(propsR.ok());
    auto props = propsR.value();
    EXPECT_NEAR(props.volume, 6000.0, 1.0);
    EXPECT_NEAR(props.centerOfMassX, 5.0, 0.1);
    EXPECT_NEAR(props.centerOfMassY, 10.0, 0.1);
    EXPECT_NEAR(props.centerOfMassZ, 15.0, 0.1);
}

TEST(Query, FaceEdgeCount) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 10, 10).value();
    EXPECT_EQ(box.countSubShapes(TopAbs_FACE), 6);
    EXPECT_EQ(box.countSubShapes(TopAbs_EDGE), 12);
    EXPECT_EQ(box.countSubShapes(TopAbs_VERTEX), 8);
}

TEST(Query, MeasureDistance) {
    auto ctx = oreo::KernelContext::create();
    auto a = oreo::makeBox(*ctx, 10, 10, 10).value();
    TopoDS_Shape box2 = BRepPrimAPI_MakeBox(gp_Pnt(20, 0, 0), 10, 10, 10).Shape();
    oreo::NamedShape b(box2, ctx->tags().nextTag());

    auto distR = oreo::measureDistance(*ctx, a, b);
    ASSERT_TRUE(distR.ok());
    double dist = distR.value();
    EXPECT_NEAR(dist, 10.0, 0.01);
}
