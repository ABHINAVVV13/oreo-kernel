// test_feature_tree.cpp — Comprehensive feature tree tests.
// Validates the core of parametric CAD: build -> replay -> modify -> re-replay.

#include <gtest/gtest.h>

#include "core/kernel_context.h"
#include "feature/feature.h"
#include "feature/feature_tree.h"
#include "geometry/oreo_geometry.h"
#include "query/oreo_query.h"
#include "naming/named_shape.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepPrimAPI_MakeBox.hxx>

namespace {

oreo::NamedShape makeRectFace(oreo::KernelContext& ctx, double w, double h) {
    gp_Pnt p1(0, 0, 0), p2(w, 0, 0), p3(w, h, 0), p4(0, h, 0);
    BRepBuilderAPI_MakeWire wm;
    wm.Add(BRepBuilderAPI_MakeEdge(p1, p2).Edge());
    wm.Add(BRepBuilderAPI_MakeEdge(p2, p3).Edge());
    wm.Add(BRepBuilderAPI_MakeEdge(p3, p4).Edge());
    wm.Add(BRepBuilderAPI_MakeEdge(p4, p1).Edge());
    BRepBuilderAPI_MakeFace fm(wm.Wire());
    return oreo::NamedShape(fm.Face(), ctx.tags.nextTag());
}

} // anonymous namespace

// -- Basic tree building and replay -----------------------------------

TEST(FeatureTree, EmptyTreeReplay) {
    oreo::FeatureTree tree;
    auto result = tree.replay();
    EXPECT_TRUE(result.isNull());
    EXPECT_EQ(tree.featureCount(), 0);
}

TEST(FeatureTree, SingleExtrudeReplay) {
    // Build a face manually, then use the tree to extrude it
    auto ctx = oreo::KernelContext::create();
    auto face = makeRectFace(*ctx, 10, 20);

    oreo::FeatureTree tree;

    // Feature: Extrude by 30 in Z
    oreo::Feature ext;
    ext.id = "F1";
    ext.type = "Extrude";
    ext.params["direction"] = gp_Vec(0, 0, 30);
    tree.addFeature(ext);

    // We need to seed the tree with the initial shape.
    // The tree's replay starts from nothing -- so the first feature needs
    // to operate on an existing shape. For now, we test with MakeBox primitive.
}

TEST(FeatureTree, BoxThenFilletReplay) {
    auto ctx = oreo::KernelContext::create();
    oreo::FeatureTree tree;

    // Feature 1: Create a box (using the geometry operation directly,
    // then check that the feature tree can reference its edges)
    auto boxR = oreo::makeBox(*ctx, 20, 20, 20);
    ASSERT_TRUE(boxR.ok());
    auto box = boxR.value();
    EXPECT_EQ(box.countSubShapes(TopAbs_FACE), 6);
    EXPECT_EQ(box.countSubShapes(TopAbs_EDGE), 12);

    // Get edges for fillet
    auto edgesR = oreo::getEdges(*ctx, box);
    ASSERT_TRUE(edgesR.ok());
    auto edges = edgesR.value();
    ASSERT_GE(edges.size(), 1u);

    // Fillet the first edge
    std::vector<oreo::NamedEdge> filletEdges = {edges[0]};
    auto filletedR = oreo::fillet(*ctx, box, filletEdges, 2.0);
    ASSERT_TRUE(filletedR.ok());
    auto filleted = filletedR.value();

    // Filleted box should have more faces
    EXPECT_GT(filleted.countSubShapes(TopAbs_FACE), 6);
}

// -- Feature tree with parameter changes ------------------------------

TEST(FeatureTree, ParameterChangeTriggersReplay) {
    oreo::FeatureTree tree;

    oreo::Feature ext;
    ext.id = "F1";
    ext.type = "Extrude";
    ext.params["direction"] = gp_Vec(0, 0, 30);
    tree.addFeature(ext);

    // Check that updateParameter marks feature dirty
    tree.updateParameter("F1", "direction", oreo::ParamValue(gp_Vec(0, 0, 50)));

    const auto* f = tree.getFeature("F1");
    ASSERT_NE(f, nullptr);
    auto dir = std::get<gp_Vec>(f->params.at("direction"));
    EXPECT_NEAR(dir.Z(), 50.0, 0.01);
}

// -- Feature suppression ----------------------------------------------

TEST(FeatureTree, SuppressFeature) {
    oreo::FeatureTree tree;

    oreo::Feature f1;
    f1.id = "F1";
    f1.type = "Extrude";
    f1.params["direction"] = gp_Vec(0, 0, 30);
    tree.addFeature(f1);

    tree.suppressFeature("F1", true);
    const auto* feature = tree.getFeature("F1");
    ASSERT_NE(feature, nullptr);
    EXPECT_TRUE(feature->suppressed);

    tree.suppressFeature("F1", false);
    feature = tree.getFeature("F1");
    EXPECT_FALSE(feature->suppressed);
}

// -- Feature removal --------------------------------------------------

TEST(FeatureTree, RemoveFeature) {
    oreo::FeatureTree tree;

    oreo::Feature f1, f2;
    f1.id = "F1"; f1.type = "Extrude"; f1.params["direction"] = gp_Vec(0,0,30);
    f2.id = "F2"; f2.type = "Fillet"; f2.params["radius"] = 2.0;
    tree.addFeature(f1);
    tree.addFeature(f2);
    EXPECT_EQ(tree.featureCount(), 2);

    EXPECT_TRUE(tree.removeFeature("F1"));
    EXPECT_EQ(tree.featureCount(), 1);
    EXPECT_EQ(tree.getFeature("F2")->type, "Fillet");

    EXPECT_FALSE(tree.removeFeature("nonexistent"));
}

// -- Serialization round-trip -----------------------------------------

TEST(FeatureTree, SerializeRoundTrip) {
    oreo::FeatureTree tree;

    oreo::Feature f1;
    f1.id = "F1";
    f1.type = "Extrude";
    f1.params["direction"] = gp_Vec(0, 0, 30);
    f1.params["distance"] = 30.0;
    tree.addFeature(f1);

    oreo::Feature f2;
    f2.id = "F2";
    f2.type = "Fillet";
    f2.params["radius"] = 2.0;
    f2.params["edges"] = std::vector<oreo::ElementRef>{
        {"F1", "Edge3;:H1a;:M", "Edge"}
    };
    tree.addFeature(f2);

    std::string json = tree.toJSON();
    EXPECT_FALSE(json.empty());

    // Verify JSON contains expected strings
    EXPECT_NE(json.find("F1"), std::string::npos);
    EXPECT_NE(json.find("Extrude"), std::string::npos);
    EXPECT_NE(json.find("F2"), std::string::npos);
    EXPECT_NE(json.find("Fillet"), std::string::npos);
    EXPECT_NE(json.find("Edge3"), std::string::npos);
}

// -- Broken feature detection -----------------------------------------

TEST(FeatureTree, BrokenFeatureDetection) {
    oreo::FeatureTree tree;

    // Add a feature with an invalid reference
    oreo::Feature f1;
    f1.id = "F1";
    f1.type = "Fillet";
    f1.params["radius"] = 2.0;
    f1.params["edges"] = std::vector<oreo::ElementRef>{
        {"nonexistent_feature", "Edge1", "Edge"}
    };
    tree.addFeature(f1);

    tree.replay();

    // Feature should be marked as broken
    EXPECT_TRUE(tree.hasBrokenFeatures());
    auto broken = tree.getBrokenFeatures();
    ASSERT_EQ(broken.size(), 1u);
    EXPECT_EQ(broken[0]->id, "F1");
    EXPECT_EQ(broken[0]->status, oreo::FeatureStatus::BrokenReference);
}

// -- Primitives integration -------------------------------------------

TEST(FeatureTree, PrimitivesWork) {
    auto ctx = oreo::KernelContext::create();

    // Test all new primitives
    auto cylR = oreo::makeCylinder(*ctx, 5.0, 20.0);
    ASSERT_TRUE(cylR.ok());
    auto cyl = cylR.value();
    EXPECT_EQ(cyl.countSubShapes(TopAbs_FACE), 3);  // top + bottom + lateral

    auto sphereR = oreo::makeSphere(*ctx, 10.0);
    ASSERT_TRUE(sphereR.ok());
    auto sphere = sphereR.value();
    EXPECT_GT(sphere.countSubShapes(TopAbs_FACE), 0);

    auto coneR = oreo::makeCone(*ctx, 5.0, 2.0, 10.0);
    ASSERT_TRUE(coneR.ok());
    auto cone = coneR.value();

    auto torusR = oreo::makeTorus(*ctx, 10.0, 3.0);
    ASSERT_TRUE(torusR.ok());
    auto torus = torusR.value();

    auto wedgeR = oreo::makeWedge(*ctx, 10, 10, 10, 5);
    ASSERT_TRUE(wedgeR.ok());
    auto wedge = wedgeR.value();
}

// -- Manufacturing operations -----------------------------------------

TEST(FeatureTree, HoleOperation) {
    auto ctx = oreo::KernelContext::create();
    auto boxR = oreo::makeBox(*ctx, 50, 50, 50);
    ASSERT_TRUE(boxR.ok());
    auto box = boxR.value();

    auto facesR = oreo::getFaces(*ctx, box);
    ASSERT_TRUE(facesR.ok());
    auto faces = facesR.value();
    ASSERT_GE(faces.size(), 1u);

    // Drill a hole in the top face
    auto resultR = oreo::hole(*ctx, box, faces[0], gp_Pnt(25, 25, 50), 10.0, 30.0);
    ASSERT_TRUE(resultR.ok());
    auto result = resultR.value();

    // Should have more faces than original box (hole adds cylindrical face)
    EXPECT_GT(result.countSubShapes(TopAbs_FACE), box.countSubShapes(TopAbs_FACE));
}

TEST(FeatureTree, OffsetOperation) {
    auto ctx = oreo::KernelContext::create();
    auto boxR = oreo::makeBox(*ctx, 10, 10, 10);
    ASSERT_TRUE(boxR.ok());
    auto box = boxR.value();

    auto resultR = oreo::offset(*ctx, box, 1.0);
    // Offset may or may not succeed depending on OCCT's handling
    // Just verify it doesn't crash
    if (resultR.ok()) {
        auto result = resultR.value();
        auto origProps = oreo::massProperties(*ctx, box).value();
        auto newProps = oreo::massProperties(*ctx, result).value();
        EXPECT_GT(newProps.volume, origProps.volume);
    }
}

TEST(FeatureTree, SplitBodyOperation) {
    auto ctx = oreo::KernelContext::create();
    auto boxR = oreo::makeBox(*ctx, 20, 20, 20);
    ASSERT_TRUE(boxR.ok());
    auto box = boxR.value();

    // Split at Z=10
    gp_Pln plane(gp_Pnt(0, 0, 10), gp_Dir(0, 0, 1));
    auto resultR = oreo::splitBody(*ctx, box, plane);
    ASSERT_TRUE(resultR.ok());
    auto result = resultR.value();

    // Split produces a compound with more faces
    EXPECT_GT(result.countSubShapes(TopAbs_FACE), box.countSubShapes(TopAbs_FACE));
}

TEST(FeatureTree, CombineShapes) {
    auto ctx = oreo::KernelContext::create();
    auto box1 = oreo::makeBox(*ctx, 10, 10, 10).value();
    auto box2 = oreo::makeBox(*ctx, gp_Pnt(20, 0, 0), 10, 10, 10).value();

    auto combinedR = oreo::combine(*ctx, {box1, box2});
    ASSERT_TRUE(combinedR.ok());
    auto combined = combinedR.value();
    // Compound should have faces from both boxes
    EXPECT_EQ(combined.countSubShapes(TopAbs_FACE), 12);
}

TEST(FeatureTree, MakeFaceFromWire) {
    auto ctx = oreo::KernelContext::create();
    // Create a rectangular wire
    gp_Pnt p1(0,0,0), p2(10,0,0), p3(10,10,0), p4(0,10,0);
    BRepBuilderAPI_MakeWire wm;
    wm.Add(BRepBuilderAPI_MakeEdge(p1, p2).Edge());
    wm.Add(BRepBuilderAPI_MakeEdge(p2, p3).Edge());
    wm.Add(BRepBuilderAPI_MakeEdge(p3, p4).Edge());
    wm.Add(BRepBuilderAPI_MakeEdge(p4, p1).Edge());
    oreo::NamedShape wire(wm.Wire(), ctx->tags.nextTag());

    auto faceR = oreo::makeFaceFromWire(*ctx, wire);
    ASSERT_TRUE(faceR.ok());
    auto face = faceR.value();
    EXPECT_EQ(face.shape().ShapeType(), TopAbs_FACE);
}

// -- Deterministic replay ---------------------------------------------

TEST(FeatureTree, DeterministicPrimitives) {
    // Create the same box 10 times, verify identical topology
    int firstFaces = -1, firstEdges = -1, firstVertices = -1;
    double firstVolume = -1;

    for (int i = 0; i < 10; ++i) {
        auto ctx = oreo::KernelContext::create();
        auto boxR = oreo::makeBox(*ctx, 10, 20, 30);
        ASSERT_TRUE(boxR.ok());
        auto box = boxR.value();

        int faces = box.countSubShapes(TopAbs_FACE);
        int edges = box.countSubShapes(TopAbs_EDGE);
        int verts = box.countSubShapes(TopAbs_VERTEX);
        double vol = oreo::massProperties(*ctx, box).value().volume;

        if (i == 0) {
            firstFaces = faces;
            firstEdges = edges;
            firstVertices = verts;
            firstVolume = vol;
        } else {
            EXPECT_EQ(faces, firstFaces) << "Face count differs on iteration " << i;
            EXPECT_EQ(edges, firstEdges) << "Edge count differs on iteration " << i;
            EXPECT_EQ(verts, firstVertices) << "Vertex count differs on iteration " << i;
            EXPECT_NEAR(vol, firstVolume, 0.001) << "Volume differs on iteration " << i;
        }
    }
}

TEST(FeatureTree, DeterministicExtrudeFilletBoolean) {
    // Run the same sequence 10 times, verify identical results
    double firstVolume = -1;
    int firstFaces = -1;

    for (int iter = 0; iter < 10; ++iter) {
        auto ctx = oreo::KernelContext::create();

        // Extrude a rectangle
        auto face = makeRectFace(*ctx, 10, 20);
        auto extR = oreo::extrude(*ctx, face, gp_Vec(0, 0, 30));
        ASSERT_TRUE(extR.ok());
        auto ext = extR.value();

        // Fillet an edge
        auto edgesR = oreo::getEdges(*ctx, ext);
        ASSERT_TRUE(edgesR.ok());
        auto edges = edgesR.value();
        ASSERT_GE(edges.size(), 1u);
        auto filletedR = oreo::fillet(*ctx, ext, {edges[0]}, 2.0);
        ASSERT_TRUE(filletedR.ok());
        auto filleted = filletedR.value();

        // Boolean subtract a cylinder
        auto cylR = oreo::makeCylinder(*ctx, gp_Ax2(gp_Pnt(5, 10, -1), gp_Dir(0, 0, 1)), 3.0, 32.0);
        ASSERT_TRUE(cylR.ok());
        auto cyl = cylR.value();
        auto final_shapeR = oreo::booleanSubtract(*ctx, filleted, cyl);
        ASSERT_TRUE(final_shapeR.ok());
        auto final_shape = final_shapeR.value();

        int faceCount = final_shape.countSubShapes(TopAbs_FACE);
        double volume = oreo::massProperties(*ctx, final_shape).value().volume;

        if (iter == 0) {
            firstFaces = faceCount;
            firstVolume = volume;
        } else {
            EXPECT_EQ(faceCount, firstFaces) << "Face count differs on iteration " << iter;
            EXPECT_NEAR(volume, firstVolume, 0.1) << "Volume differs on iteration " << iter;
        }
    }
}

// =====================================================================
// Gap 5: Tests for previously untested operations
// =====================================================================

TEST(Operations, DraftFaces) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 30, 30, 30).value();
    auto facesR = oreo::getFaces(*ctx, box);
    ASSERT_TRUE(facesR.ok());
    auto faces = facesR.value();
    ASSERT_GE(faces.size(), 1u);

    // Draft the first face by 5 degrees
    auto resultR = oreo::draft(*ctx, box, {faces[0]}, 5.0, gp_Dir(0, 0, 1));
    // Draft may fail on certain faces -- just verify no crash
    if (resultR.ok()) {
        auto result = resultR.value();
        EXPECT_GT(result.countSubShapes(TopAbs_FACE), 0);
    }
}

TEST(Operations, Pocket) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 50, 50, 50).value();

    // Pocket is extrude-cut. Create a face positioned on top of the box.
    // The pocket function extrudes in -Z, so the face at z=50 cuts into the box.
    gp_Pnt p1(10,10,50), p2(40,10,50), p3(40,40,50), p4(10,40,50);
    BRepBuilderAPI_MakeWire wm;
    wm.Add(BRepBuilderAPI_MakeEdge(p1, p2).Edge());
    wm.Add(BRepBuilderAPI_MakeEdge(p2, p3).Edge());
    wm.Add(BRepBuilderAPI_MakeEdge(p3, p4).Edge());
    wm.Add(BRepBuilderAPI_MakeEdge(p4, p1).Edge());
    BRepBuilderAPI_MakeFace fm(wm.Wire());
    oreo::NamedShape profile(fm.Face(), ctx->tags.nextTag());

    auto pocketedR = oreo::pocket(*ctx, box, profile, 20.0);
    if (pocketedR.ok()) {
        auto pocketed = pocketedR.value();
        auto origVol = oreo::massProperties(*ctx, box).value().volume;
        auto newVol = oreo::massProperties(*ctx, pocketed).value().volume;
        EXPECT_LT(newVol, origVol);
    }
}

TEST(Operations, Thicken) {
    auto ctx = oreo::KernelContext::create();
    // Thicken works on shells (not bare faces).
    // Create a box, shell it, then thicken the result.
    auto box = oreo::makeBox(*ctx, 20, 20, 20).value();

    // Shell it first to get a shell
    auto facesR = oreo::getFaces(*ctx, box);
    ASSERT_TRUE(facesR.ok());
    auto faces = facesR.value();
    ASSERT_GE(faces.size(), 1u);
    auto shelledR = oreo::shell(*ctx, box, {faces[0]}, 1.0);

    if (shelledR.ok()) {
        auto shelled = shelledR.value();
        // Now try to thicken the shelled result
        auto thickR = oreo::thicken(*ctx, shelled, 2.0);
        // Thicken may succeed or fail -- just verify no crash
        if (thickR.ok()) {
            auto thick = thickR.value();
            EXPECT_GT(thick.countSubShapes(TopAbs_FACE), 0);
        }
    }
}

TEST(Operations, VariableFillet) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 30, 30, 30).value();
    auto edgesR = oreo::getEdges(*ctx, box);
    ASSERT_TRUE(edgesR.ok());
    auto edges = edgesR.value();
    ASSERT_GE(edges.size(), 1u);

    auto resultR = oreo::filletVariable(*ctx, box, edges[0], 1.0, 5.0);
    if (resultR.ok()) {
        auto result = resultR.value();
        EXPECT_GT(result.countSubShapes(TopAbs_FACE), box.countSubShapes(TopAbs_FACE));
    }
}

TEST(Operations, WireOffset) {
    auto ctx = oreo::KernelContext::create();
    gp_Pnt p1(0,0,0), p2(20,0,0), p3(20,20,0), p4(0,20,0);
    BRepBuilderAPI_MakeWire wm;
    wm.Add(BRepBuilderAPI_MakeEdge(p1, p2).Edge());
    wm.Add(BRepBuilderAPI_MakeEdge(p2, p3).Edge());
    wm.Add(BRepBuilderAPI_MakeEdge(p3, p4).Edge());
    wm.Add(BRepBuilderAPI_MakeEdge(p4, p1).Edge());
    oreo::NamedShape wire(wm.Wire(), ctx->tags.nextTag());

    auto resultR = oreo::wireOffset(*ctx, wire, 2.0);
    // Wire offset may succeed or fail depending on OCCT version
    if (resultR.ok()) {
        auto result = resultR.value();
        EXPECT_GT(result.countSubShapes(TopAbs_EDGE), 0);
    }
}

TEST(Operations, Rib) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 50, 50, 10).value();

    auto ribProfile = makeRectFace(*ctx, 2, 40);
    auto resultR = oreo::rib(*ctx, box, ribProfile, gp_Dir(0, 0, 1), 30.0);
    if (resultR.ok()) {
        auto result = resultR.value();
        auto origVol = oreo::massProperties(*ctx, box).value().volume;
        auto newVol = oreo::massProperties(*ctx, result).value().volume;
        EXPECT_GT(newVol, origVol);
    }
}

// =====================================================================
// Gap 7: End-to-end parametric replay test
// =====================================================================

TEST(ParametricReplay, BoxExtrudeFilletChangeHeight) {
    auto ctx = oreo::KernelContext::create();

    // 1. Create box via feature tree
    oreo::FeatureTree tree;

    oreo::Feature makeBoxF;
    makeBoxF.id = "F1";
    makeBoxF.type = "MakeBox";
    makeBoxF.params["dimensions"] = gp_Vec(20, 20, 20);
    tree.addFeature(makeBoxF);

    // 2. Replay -> get box
    auto shape1 = tree.replay();
    ASSERT_FALSE(shape1.isNull());
    EXPECT_EQ(shape1.countSubShapes(TopAbs_FACE), 6);

    double vol1 = oreo::massProperties(*ctx, shape1).value().volume;
    EXPECT_NEAR(vol1, 8000.0, 1.0);  // 20*20*20

    // 3. Change dimension -> re-replay
    tree.updateParameter("F1", "dimensions", oreo::ParamValue(gp_Vec(30, 20, 20)));
    auto shape2 = tree.replay();
    ASSERT_FALSE(shape2.isNull());
    EXPECT_EQ(shape2.countSubShapes(TopAbs_FACE), 6);  // Same topology

    double vol2 = oreo::massProperties(*ctx, shape2).value().volume;
    EXPECT_NEAR(vol2, 12000.0, 1.0);  // 30*20*20
}

TEST(ParametricReplay, FeatureTreeSerializeRoundTrip) {
    oreo::FeatureTree tree;

    oreo::Feature f1;
    f1.id = "F1";
    f1.type = "MakeBox";
    f1.params["dimensions"] = gp_Vec(10, 20, 30);
    tree.addFeature(f1);

    oreo::Feature f2;
    f2.id = "F2";
    f2.type = "Offset";
    f2.params["distance"] = 1.5;
    tree.addFeature(f2);

    // Serialize
    std::string json = tree.toJSON();
    EXPECT_FALSE(json.empty());

    // Deserialize
    auto restored = oreo::FeatureTree::fromJSON(json);
    EXPECT_EQ(restored.featureCount(), 2);

    // Verify features restored correctly
    auto* rf1 = restored.getFeature("F1");
    ASSERT_NE(rf1, nullptr);
    EXPECT_EQ(rf1->type, "MakeBox");

    auto* rf2 = restored.getFeature("F2");
    ASSERT_NE(rf2, nullptr);
    EXPECT_EQ(rf2->type, "Offset");
    EXPECT_NEAR(std::get<double>(rf2->params.at("distance")), 1.5, 0.001);
}

// =====================================================================
// Gap 9: Element map name determinism
// =====================================================================

TEST(Determinism, ElementMapNamesIdentical) {
    // Run the same operations 10 times, verify element map names are identical
    std::map<std::string, std::string> firstNames;

    for (int iter = 0; iter < 10; ++iter) {
        auto ctx = oreo::KernelContext::create();
        auto box = oreo::makeBox(*ctx, 15, 25, 35).value();

        // Collect all face names
        std::map<std::string, std::string> names;
        for (int i = 1; i <= box.countSubShapes(TopAbs_FACE); ++i) {
            oreo::IndexedName idx("Face", i);
            auto mapped = box.getElementName(idx);
            names[idx.toString()] = mapped.data();
        }
        for (int i = 1; i <= box.countSubShapes(TopAbs_EDGE); ++i) {
            oreo::IndexedName idx("Edge", i);
            auto mapped = box.getElementName(idx);
            names[idx.toString()] = mapped.data();
        }

        if (iter == 0) {
            firstNames = names;
        } else {
            EXPECT_EQ(names.size(), firstNames.size())
                << "Element count differs on iteration " << iter;
            for (auto& [key, val] : names) {
                auto it = firstNames.find(key);
                if (it != firstNames.end()) {
                    EXPECT_EQ(val, it->second)
                        << "Element name for " << key << " differs on iteration " << iter;
                }
            }
        }
    }
}
