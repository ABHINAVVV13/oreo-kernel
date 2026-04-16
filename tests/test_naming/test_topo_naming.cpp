// test_topo_naming.cpp — Topological naming regression tests.
// Verify that element names are stable across operations and
// that downstream references resolve correctly after upstream changes.

#include <gtest/gtest.h>

#include "core/oreo_error.h"
#include "geometry/oreo_geometry.h"
#include "naming/named_shape.h"
#include "naming/element_map.h"
#include "query/oreo_query.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>

namespace {

oreo::NamedShape makeBox(double x, double y, double z) {
    TopoDS_Shape box = BRepPrimAPI_MakeBox(x, y, z).Shape();
    return oreo::NamedShape(box, oreo::NamedShape::nextTag());
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

// ── Element Map Basics ───────────────────────────────────────

TEST(TopoNaming, ElementMapBasicSetGet) {
    oreo::ElementMap map;
    oreo::IndexedName idx("Face", 1);
    oreo::MappedName name("Face1;:H1;:M");

    auto result = map.setElementName(idx, name);
    EXPECT_FALSE(result.empty());
    EXPECT_EQ(map.getMappedName(idx).data(), "Face1;:H1;:M");
    EXPECT_EQ(map.getIndexedName(name).index(), 1);
    EXPECT_EQ(map.getIndexedName(name).type(), "Face");
}

TEST(TopoNaming, ElementMapDuplicateDisambiguates) {
    oreo::ElementMap map;
    oreo::IndexedName idx1("Face", 1);
    oreo::IndexedName idx2("Face", 2);
    oreo::MappedName name("SameName");

    auto r1 = map.setElementName(idx1, name);
    auto r2 = map.setElementName(idx2, name);
    // Second should get a disambiguated name (";D1" suffix)
    EXPECT_FALSE(r1.empty());
    EXPECT_FALSE(r2.empty());
    EXPECT_NE(r1.data(), r2.data());  // Different names
}

TEST(TopoNaming, ElementMapCount) {
    oreo::ElementMap map;
    map.setElementName(oreo::IndexedName("Face", 1), oreo::MappedName("F1"));
    map.setElementName(oreo::IndexedName("Face", 2), oreo::MappedName("F2"));
    map.setElementName(oreo::IndexedName("Edge", 1), oreo::MappedName("E1"));

    EXPECT_EQ(map.count(), 3);
    EXPECT_EQ(map.count("Face"), 2);
    EXPECT_EQ(map.count("Edge"), 1);
    EXPECT_EQ(map.count("Vertex"), 0);
}

// ── Name Encoding ────────────────────────────────────────────

TEST(TopoNaming, EncodeElementName) {
    oreo::MappedName input("Face1");
    auto encoded = oreo::ElementMap::encodeElementName("Face", input, 0x1a, ";:M");
    EXPECT_NE(encoded.data().find(";:H"), std::string::npos);
    EXPECT_NE(encoded.data().find(";:M"), std::string::npos);
    EXPECT_NE(encoded.data().find("1a"), std::string::npos);
}

// ── Serialization Round-trip ─────────────────────────────────

TEST(TopoNaming, ElementMapSerializeRoundTrip) {
    oreo::ElementMap map;
    map.setElementName(oreo::IndexedName("Face", 1), oreo::MappedName("F1;:Ha;:M"));
    map.setElementName(oreo::IndexedName("Face", 2), oreo::MappedName("F2;:Ha;:G"));
    map.setElementName(oreo::IndexedName("Edge", 1), oreo::MappedName("E1;:Hb;:M"));

    auto buf = map.serialize();
    ASSERT_FALSE(buf.empty());

    auto restored = oreo::ElementMap::deserialize(buf.data(), buf.size());
    ASSERT_NE(restored, nullptr);

    EXPECT_EQ(restored->count(), 3);
    EXPECT_EQ(restored->getMappedName(oreo::IndexedName("Face", 1)).data(), "F1;:Ha;:M");
    EXPECT_EQ(restored->getMappedName(oreo::IndexedName("Edge", 1)).data(), "E1;:Hb;:M");
}

// ── Operation Produces Element Map ───────────────────────────

TEST(TopoNaming, ExtrudeProducesElementMap) {
    auto face = makeRectFace(10, 20);
    auto result = oreo::extrude(face, gp_Vec(0, 0, 30));
    ASSERT_FALSE(result.isNull());
    ASSERT_NE(result.elementMap(), nullptr);

    // Extruded shape should have named faces
    int faceCount = result.countSubShapes(TopAbs_FACE);
    EXPECT_GT(faceCount, 0);

    // Every face should have a mapping
    for (int i = 1; i <= faceCount; ++i) {
        oreo::IndexedName idx("Face", i);
        auto name = result.elementMap()->getMappedName(idx);
        EXPECT_FALSE(name.empty()) << "Face" << i << " has no mapped name";
    }
}

TEST(TopoNaming, BooleanPreservesNames) {
    auto a = makeBox(10, 10, 10);
    auto b = makeBox(10, 10, 10);
    auto result = oreo::booleanUnion(a, b);
    ASSERT_FALSE(result.isNull());
    ASSERT_NE(result.elementMap(), nullptr);

    // Result should have named faces
    int faceCount = result.countSubShapes(TopAbs_FACE);
    EXPECT_GT(faceCount, 0);
    EXPECT_GT(result.elementMap()->count("Face"), 0);
}

TEST(TopoNaming, FilletPreservesBaseNames) {
    auto box = makeBox(20, 20, 20);
    auto edges = oreo::getEdges(box);
    ASSERT_GT(edges.size(), 0u);

    std::vector<oreo::NamedEdge> filletEdges = {edges[0]};
    auto result = oreo::fillet(box, filletEdges, 2.0);
    ASSERT_FALSE(result.isNull());
    ASSERT_NE(result.elementMap(), nullptr);

    // The fillet should have produced new faces
    EXPECT_GT(result.countSubShapes(TopAbs_FACE), box.countSubShapes(TopAbs_FACE));
    // And all faces should be named
    EXPECT_EQ(result.elementMap()->count("Face"), result.countSubShapes(TopAbs_FACE));
}

// ── Name Stability Across Rebuilds ───────────────────────────

TEST(TopoNaming, NameStabilityExtrudeThenFillet) {
    // Build: face → extrude(30) → fillet(edge[0], 2)
    auto face1 = makeRectFace(10, 20);
    auto ext1 = oreo::extrude(face1, gp_Vec(0, 0, 30));
    ASSERT_FALSE(ext1.isNull());

    auto edges1 = oreo::getEdges(ext1);
    ASSERT_GT(edges1.size(), 0u);
    std::vector<oreo::NamedEdge> fEdges1 = {edges1[0]};
    auto fillet1 = oreo::fillet(ext1, fEdges1, 2.0);
    ASSERT_FALSE(fillet1.isNull());

    int faceCount1 = fillet1.countSubShapes(TopAbs_FACE);

    // Rebuild with different extrude height: face → extrude(50) → fillet(edge[0], 2)
    auto face2 = makeRectFace(10, 20);
    auto ext2 = oreo::extrude(face2, gp_Vec(0, 0, 50));
    ASSERT_FALSE(ext2.isNull());

    auto edges2 = oreo::getEdges(ext2);
    ASSERT_GT(edges2.size(), 0u);
    std::vector<oreo::NamedEdge> fEdges2 = {edges2[0]};
    auto fillet2 = oreo::fillet(ext2, fEdges2, 2.0);
    ASSERT_FALSE(fillet2.isNull());

    int faceCount2 = fillet2.countSubShapes(TopAbs_FACE);

    // Same topology → same face count
    EXPECT_EQ(faceCount1, faceCount2);
}
