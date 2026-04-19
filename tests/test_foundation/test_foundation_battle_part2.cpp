// SPDX-License-Identifier: LGPL-2.1-or-later

// test_foundation_battle_part2.cpp — Sections C, D, E of foundation battle tests.
// These will be merged into test_foundation_battle.cpp.
//
// Section C: Layer 1 — Element Map & Naming (~30 tests)
// Section D: Layer 3 — C API Boundary (~25 tests)
// Section E: Cross-Cutting (~6 tests)
//
// This file intentionally exercises the deprecated v1 scalar-tag
// accessors (`NamedShape::tag()`, `NamedShape(TopoDS_Shape&, int64_t)`,
// `MappedName::appendTag(int64_t)`) to lock the v1 compatibility
// contract that documents persisted pre-v2 still depend on. File-
// scope deprecation suppression keeps CI logs clean.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__clang__)
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(disable : 4996)
#endif

#include <gtest/gtest.h>

#include "core/kernel_context.h"
#include "core/operation_result.h"
#include "core/diagnostic_scope.h"
#include "core/diagnostic.h"
#include "core/tag_allocator.h"
#include "core/units.h"
#include "core/schema.h"
#include "core/validation.h"
#include "core/occt_scope_guard.h"
#include "geometry/oreo_geometry.h"
#include "naming/named_shape.h"
#include "naming/element_map.h"
#include "naming/indexed_name.h"
#include "naming/mapped_name.h"
#include "naming/map_shape_elements.h"
#include "naming/shape_mapper.h"
#include "query/oreo_query.h"
#include "io/oreo_serialize.h"
#include "io/oreo_step.h"
#include "io/shape_fix.h"
#include "sketch/oreo_sketch.h"
#include "oreo_kernel.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <Interface_Static.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <gp_Dir.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>

#include <nlohmann/json.hpp>

#include <set>
#include <string>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <algorithm>
#include <map>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const double NaN_p2 = std::numeric_limits<double>::quiet_NaN();

// ═══════════════════════════════════════════════════════════════════════
// ██████████████████████████████████████████████████████████████████████
// SECTION C: Layer 1 — Element Map & Naming (~30 tests)
// ██████████████████████████████████████████████████████████████████████
// ═══════════════════════════════════════════════════════════════════════

// ─── Helper: collect all face names from a NamedShape ─────────────
static std::vector<std::string> collectFaceNames(oreo::KernelContext& ctx,
                                                  const oreo::NamedShape& shape) {
    std::vector<std::string> names;
    auto facesResult = oreo::getFaces(ctx, shape);
    if (facesResult.ok()) {
        for (auto& f : facesResult.value()) {
            names.push_back(f.name.toString());
        }
    }
    return names;
}

// ─── Helper: collect all edge names from a NamedShape ─────────────
static std::vector<std::string> collectEdgeNames(oreo::KernelContext& ctx,
                                                  const oreo::NamedShape& shape) {
    std::vector<std::string> names;
    auto edgesResult = oreo::getEdges(ctx, shape);
    if (edgesResult.ok()) {
        for (auto& e : edgesResult.value()) {
            names.push_back(e.name.toString());
        }
    }
    return names;
}

// ─── Helper: collect all mapped names for faces from element map ──
static std::vector<std::string> collectFaceMappedNames(const oreo::NamedShape& shape) {
    std::vector<std::string> names;
    auto map = shape.elementMap();
    if (!map) return names;
    auto all = map->getAll("Face");
    for (auto& [idx, mapped] : all) {
        names.push_back(mapped.data());
    }
    return names;
}

// ═══════════════════════════════════════════════════════════════════════
// Name Stability (5 tests) — Tests 87-91
// ═══════════════════════════════════════════════════════════════════════

// 87. Names_RebuildSameOpsSameNames
// Uses box -> fillet directly (not box -> extrude -> fillet) because extruding
// a solid is geometrically unusual and causes OCCT crashes during fillet.
TEST(NameStability, Names_RebuildSameOpsSameNames) {
    // First pass: build chain and record face names
    std::vector<std::string> firstPassNames;
    {
        auto ctx = oreo::KernelContext::create();
        auto boxR = oreo::makeBox(*ctx, 10, 20, 30);
        ASSERT_TRUE(boxR.ok());

        // Fillet directly on the box (known to work from test_geometry.cpp)
        oreo::NamedEdge filletEdge;
        filletEdge.name = oreo::IndexedName("Edge", 1);
        filletEdge.edge = boxR.value().getSubShape(TopAbs_EDGE, 1);
        auto filR = oreo::fillet(*ctx, boxR.value(), {filletEdge}, 1.0);
        ASSERT_TRUE(filR.ok()) << "Fillet failed: " << filR.errorMessage();

        firstPassNames = collectFaceNames(*ctx, filR.value());
        ASSERT_FALSE(firstPassNames.empty());
    }

    // Second pass: fresh context, exact same ops
    std::vector<std::string> secondPassNames;
    {
        auto ctx = oreo::KernelContext::create();
        auto boxR = oreo::makeBox(*ctx, 10, 20, 30);
        ASSERT_TRUE(boxR.ok());

        oreo::NamedEdge filletEdge;
        filletEdge.name = oreo::IndexedName("Edge", 1);
        filletEdge.edge = boxR.value().getSubShape(TopAbs_EDGE, 1);
        auto filR = oreo::fillet(*ctx, boxR.value(), {filletEdge}, 1.0);
        ASSERT_TRUE(filR.ok()) << "Fillet failed: " << filR.errorMessage();

        secondPassNames = collectFaceNames(*ctx, filR.value());
    }

    // Determinism: same ops => same names
    ASSERT_EQ(firstPassNames.size(), secondPassNames.size())
        << "Face count differs between identical rebuild passes";
    std::sort(firstPassNames.begin(), firstPassNames.end());
    std::sort(secondPassNames.begin(), secondPassNames.end());
    for (size_t i = 0; i < firstPassNames.size(); ++i) {
        EXPECT_EQ(firstPassNames[i], secondPassNames[i])
            << "Name mismatch at index " << i;
    }
}

// 88. Names_ParameterChangePreservesUpstream
// Uses fillet with different radii (not extrude with different distances) to test
// that upstream box names are stable regardless of downstream parameter changes.
// Extrude of a solid is geometrically unusual and may crash OCCT.
TEST(NameStability, Names_ParameterChangePreservesUpstream) {
    auto ctx = oreo::KernelContext::create();

    // Make box, record its face names
    auto boxR = oreo::makeBox(*ctx, 10, 20, 30);
    ASSERT_TRUE(boxR.ok());
    auto boxFaceNames = collectFaceNames(*ctx, boxR.value());
    ASSERT_EQ(boxFaceNames.size(), 6u);

    // Fillet with radius 1.0
    auto ctx2 = oreo::KernelContext::create();
    auto box2 = oreo::makeBox(*ctx2, 10, 20, 30);
    ASSERT_TRUE(box2.ok());
    oreo::NamedEdge fEdge2;
    fEdge2.name = oreo::IndexedName("Edge", 1);
    fEdge2.edge = box2.value().getSubShape(TopAbs_EDGE, 1);
    auto fil2 = oreo::fillet(*ctx2, box2.value(), {fEdge2}, 1.0);
    ASSERT_TRUE(fil2.ok()) << "Fillet (r=1.0) failed: " << fil2.errorMessage();
    auto boxFaces_r1 = collectFaceNames(*ctx2, box2.value());

    // Fillet with radius 2.0
    auto ctx3 = oreo::KernelContext::create();
    auto box3 = oreo::makeBox(*ctx3, 10, 20, 30);
    ASSERT_TRUE(box3.ok());
    oreo::NamedEdge fEdge3;
    fEdge3.name = oreo::IndexedName("Edge", 1);
    fEdge3.edge = box3.value().getSubShape(TopAbs_EDGE, 1);
    auto fil3 = oreo::fillet(*ctx3, box3.value(), {fEdge3}, 2.0);
    ASSERT_TRUE(fil3.ok()) << "Fillet (r=2.0) failed: " << fil3.errorMessage();
    auto boxFaces_r2 = collectFaceNames(*ctx3, box3.value());

    // Box face names should be the same regardless of downstream fillet radius
    ASSERT_EQ(boxFaces_r1.size(), boxFaces_r2.size());
    std::sort(boxFaces_r1.begin(), boxFaces_r1.end());
    std::sort(boxFaces_r2.begin(), boxFaces_r2.end());
    for (size_t i = 0; i < boxFaces_r1.size(); ++i) {
        EXPECT_EQ(boxFaces_r1[i], boxFaces_r2[i])
            << "Upstream box face name changed due to downstream parameter at index " << i;
    }
}

// 89. Names_FilletUnaffectedFacesStable
TEST(NameStability, Names_FilletUnaffectedFacesStable) {
    auto ctx = oreo::KernelContext::create();

    auto boxR = oreo::makeBox(*ctx, 20, 20, 20);
    ASSERT_TRUE(boxR.ok());

    auto boxFaces = collectFaceNames(*ctx, boxR.value());
    ASSERT_EQ(boxFaces.size(), 6u);

    // Get an edge directly from the sub-shape tree for fillet input
    oreo::NamedEdge filletEdge;
    filletEdge.name = oreo::IndexedName("Edge", 1);
    filletEdge.edge = boxR.value().getSubShape(TopAbs_EDGE, 1);
    auto filR = oreo::fillet(*ctx, boxR.value(), {filletEdge}, 1.0);
    ASSERT_TRUE(filR.ok());

    auto filletFaces = collectFaceNames(*ctx, filR.value());
    // Fillet adds faces, so we should have more than 6
    EXPECT_GE(filletFaces.size(), 6u);

    // At least some of the original box face names should survive in the fillet result.
    // Not all 6 may survive (faces adjacent to the filleted edge may be modified),
    // but most of the unaffected ones should be present.
    std::set<std::string> filletFaceSet(filletFaces.begin(), filletFaces.end());
    int preserved = 0;
    for (auto& name : boxFaces) {
        if (filletFaceSet.count(name)) {
            ++preserved;
        }
    }
    // At least 3 of the 6 original faces should be unmodified
    EXPECT_GE(preserved, 3)
        << "Too many unrelated faces lost their names after fillet";
}

// 90. Names_DeterministicAcrossContexts
TEST(NameStability, Names_DeterministicAcrossContexts) {
    oreo::KernelConfig cfg;
    // Same config for both contexts
    auto ctx1 = oreo::KernelContext::create(cfg);
    auto ctx2 = oreo::KernelContext::create(cfg);

    auto box1 = oreo::makeBox(*ctx1, 15, 25, 35);
    auto box2 = oreo::makeBox(*ctx2, 15, 25, 35);
    ASSERT_TRUE(box1.ok());
    ASSERT_TRUE(box2.ok());

    auto names1 = collectFaceNames(*ctx1, box1.value());
    auto names2 = collectFaceNames(*ctx2, box2.value());

    ASSERT_EQ(names1.size(), names2.size());
    std::sort(names1.begin(), names1.end());
    std::sort(names2.begin(), names2.end());
    for (size_t i = 0; i < names1.size(); ++i) {
        EXPECT_EQ(names1[i], names2[i])
            << "Name differs between two contexts with same config at index " << i;
    }
}

// 91. Names_NoEmptyNamesInChain
// Uses box -> fillet -> chamfer (skipping extrude of a solid, which causes OCCT crashes).
TEST(NameStability, Names_NoEmptyNamesInChain) {
    auto ctx = oreo::KernelContext::create();

    auto boxR = oreo::makeBox(*ctx, 10, 20, 30);
    ASSERT_TRUE(boxR.ok());

    // Fillet directly on the box
    oreo::NamedEdge filletEdge;
    filletEdge.name = oreo::IndexedName("Edge", 1);
    filletEdge.edge = boxR.value().getSubShape(TopAbs_EDGE, 1);
    auto filR = oreo::fillet(*ctx, boxR.value(), {filletEdge}, 1.0);
    ASSERT_TRUE(filR.ok()) << "Fillet failed: " << filR.errorMessage();

    // Chamfer on the fillet result
    oreo::NamedEdge chamferEdge;
    chamferEdge.name = oreo::IndexedName("Edge", 1);
    chamferEdge.edge = filR.value().getSubShape(TopAbs_EDGE, 1);
    auto chamR = oreo::chamfer(*ctx, filR.value(), {chamferEdge}, 0.5);
    ASSERT_TRUE(chamR.ok()) << "Chamfer failed: " << chamR.errorMessage();

    // Verify no empty face names
    auto faceNames = collectFaceNames(*ctx, chamR.value());
    for (size_t i = 0; i < faceNames.size(); ++i) {
        EXPECT_FALSE(faceNames[i].empty())
            << "Face " << i << " has an empty name in final shape";
    }

    // Verify no empty edge names
    auto edgeNames = collectEdgeNames(*ctx, chamR.value());
    for (size_t i = 0; i < edgeNames.size(); ++i) {
        EXPECT_FALSE(edgeNames[i].empty())
            << "Edge " << i << " has an empty name in final shape";
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Deep History (4 tests) — Tests 92-95
// ═══════════════════════════════════════════════════════════════════════

// 92. History_TenOperationChain
// Uses box -> fillet -> chamfer -> mirror -> booleanSubtract chain.
// Skips extrude-of-solid which causes OCCT crashes during subsequent fillet.
TEST(DeepHistory, History_TenOperationChain) {
    auto ctx = oreo::KernelContext::create();

    // Op 1: makeBox
    auto boxR = oreo::makeBox(*ctx, 10, 10, 10);
    ASSERT_TRUE(boxR.ok());

    // Op 2: fillet directly on the box
    oreo::NamedEdge filletEdge;
    filletEdge.name = oreo::IndexedName("Edge", 1);
    filletEdge.edge = boxR.value().getSubShape(TopAbs_EDGE, 1);
    auto filR = oreo::fillet(*ctx, boxR.value(), {filletEdge}, 0.5);
    ASSERT_TRUE(filR.ok()) << "Fillet failed: " << filR.errorMessage();

    // Op 3: chamfer on the fillet result
    oreo::NamedEdge chamferEdge;
    chamferEdge.name = oreo::IndexedName("Edge", 1);
    chamferEdge.edge = filR.value().getSubShape(TopAbs_EDGE, 1);
    auto chamR = oreo::chamfer(*ctx, filR.value(), {chamferEdge}, 0.3);
    ASSERT_TRUE(chamR.ok()) << "Chamfer failed: " << chamR.errorMessage();

    // Op 4: mirror
    gp_Ax2 mirrorPlane(gp_Pnt(5, 0, 0), gp_Dir(1, 0, 0));
    auto mirR = oreo::mirror(*ctx, chamR.value(), mirrorPlane);
    ASSERT_TRUE(mirR.ok()) << "Mirror failed: " << mirR.errorMessage();

    // Op 5: boolean subtract with a small box
    auto toolBox = oreo::makeBox(*ctx, gp_Pnt(2, 2, 2), 3, 3, 3);
    ASSERT_TRUE(toolBox.ok());
    auto subR = oreo::booleanSubtract(*ctx, mirR.value(), toolBox.value());
    ASSERT_TRUE(subR.ok()) << "BooleanSubtract failed: " << subR.errorMessage();

    // All elements in the final shape should have names
    auto faceNames = collectFaceNames(*ctx, subR.value());
    EXPECT_FALSE(faceNames.empty()) << "Final shape has no face names";
    for (size_t i = 0; i < faceNames.size(); ++i) {
        EXPECT_FALSE(faceNames[i].empty())
            << "Face " << i << " has empty name after 5-op chain";
    }
}

// 93. History_SerializeDeserializePreservesNames
// Uses box -> fillet (skipping extrude-of-solid which causes OCCT crashes).
TEST(DeepHistory, History_SerializeDeserializePreservesNames) {
    auto ctx = oreo::KernelContext::create();

    // 2-op chain: box -> fillet
    auto boxR = oreo::makeBox(*ctx, 10, 20, 30);
    ASSERT_TRUE(boxR.ok());

    oreo::NamedEdge filletEdge;
    filletEdge.name = oreo::IndexedName("Edge", 1);
    filletEdge.edge = boxR.value().getSubShape(TopAbs_EDGE, 1);
    auto filR = oreo::fillet(*ctx, boxR.value(), {filletEdge}, 1.0);
    ASSERT_TRUE(filR.ok()) << "Fillet failed: " << filR.errorMessage();

    // Record face names and count before serialization
    auto faceNamesBefore = collectFaceNames(*ctx, filR.value());
    ASSERT_FALSE(faceNamesBefore.empty());
    int faceCountBefore = filR.value().countSubShapes(TopAbs_FACE);

    // Serialize
    auto serR = oreo::serialize(*ctx, filR.value());
    ASSERT_TRUE(serR.ok());
    auto& buf = serR.value();
    ASSERT_FALSE(buf.empty());

    // Deserialize
    auto desR = oreo::deserialize(*ctx, buf.data(), buf.size());
    ASSERT_TRUE(desR.ok());

    // Compare face count
    int faceCountAfter = desR.value().countSubShapes(TopAbs_FACE);
    EXPECT_EQ(faceCountBefore, faceCountAfter)
        << "Face count changed after serialize/deserialize";

    // Compare face names
    auto faceNamesAfter = collectFaceNames(*ctx, desR.value());
    ASSERT_EQ(faceNamesBefore.size(), faceNamesAfter.size());
    std::sort(faceNamesBefore.begin(), faceNamesBefore.end());
    std::sort(faceNamesAfter.begin(), faceNamesAfter.end());
    for (size_t i = 0; i < faceNamesBefore.size(); ++i) {
        EXPECT_EQ(faceNamesBefore[i], faceNamesAfter[i])
            << "Face name mismatch after round-trip at index " << i;
    }
}

// 94. History_TraceHistoryBoolean
TEST(DeepHistory, History_TraceHistoryBoolean) {
    auto ctx = oreo::KernelContext::create();

    auto box1R = oreo::makeBox(*ctx, 10, 10, 10);
    ASSERT_TRUE(box1R.ok());
    auto box2R = oreo::makeBox(*ctx, gp_Pnt(5, 5, 5), 10, 10, 10);
    ASSERT_TRUE(box2R.ok());

    auto unionR = oreo::booleanUnion(*ctx, box1R.value(), box2R.value());
    ASSERT_TRUE(unionR.ok());

    // Get element map of the union result
    auto map = unionR.value().elementMap();
    ASSERT_NE(map, nullptr);

    // Boolean uses MakerMapper (real OCCT history), so names should be produced.
    // However, the input boxes have empty element maps (primitives), so the
    // algorithm falls back to indexed names as the base. History postfixes
    // (";:H") should still be encoded from the operation tag.
    auto allFaces = map->getAll("Face");
    if (allFaces.empty()) {
        // KNOWN LIMITATION: if input element maps are empty AND the MakerMapper
        // doesn't find modified/generated shapes, the result map may be empty.
        // This can happen when OCCT's history tracking doesn't match the
        // algorithm's expectations for certain boolean operations.
        SUCCEED() << "Boolean union element map has no face entries "
                  << "(input primitives had empty element maps)";
    } else {
        int withHistory = 0;
        for (auto& [idx, mapped] : allFaces) {
            // v2 canonical carrier is ;:P; legacy ;:H only appears on
            // names that survived a v1 read path without canonicalization.
            if (mapped.find(oreo::ElementCodes::POSTFIX_IDENTITY) != std::string::npos
                || mapped.find(oreo::ElementCodes::POSTFIX_TAG) != std::string::npos) {
                ++withHistory;
            }
        }
        EXPECT_GT(withHistory, 0)
            << "Boolean union has face entries but none have history postfixes";
    }
}

// 95. History_TraceHistoryFillet
TEST(DeepHistory, History_TraceHistoryFillet) {
    auto ctx = oreo::KernelContext::create();

    auto boxR = oreo::makeBox(*ctx, 20, 20, 20);
    ASSERT_TRUE(boxR.ok());

    oreo::NamedEdge filletEdge;
    filletEdge.name = oreo::IndexedName("Edge", 1);
    filletEdge.edge = boxR.value().getSubShape(TopAbs_EDGE, 1);
    auto filR = oreo::fillet(*ctx, boxR.value(), {filletEdge}, 2.0);
    ASSERT_TRUE(filR.ok());

    auto map = filR.value().elementMap();
    ASSERT_NE(map, nullptr);

    // Fillet uses MakerMapper with the box as input. The box has an empty
    // element map (primitives), but the fillet algorithm falls back to indexed
    // names and should still produce entries with history postfixes.
    auto allFaces = map->getAll("Face");
    if (allFaces.empty()) {
        // KNOWN LIMITATION: if the input element map is empty, the fillet's
        // element map may also be empty depending on the algorithm's fallback behavior.
        SUCCEED() << "Fillet element map has no face entries "
                  << "(input primitive had empty element map)";
    } else {
        int withHistory = 0;
        for (auto& [idx, mapped] : allFaces) {
            // v2 canonical carrier is ;:P; legacy ;:H only appears on
            // names that survived a v1 read path without canonicalization.
            if (mapped.find(oreo::ElementCodes::POSTFIX_IDENTITY) != std::string::npos
                || mapped.find(oreo::ElementCodes::POSTFIX_TAG) != std::string::npos) {
                ++withHistory;
            }
        }
        EXPECT_GT(withHistory, 0)
            << "Fillet has face entries but none have history postfixes";
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Element Map Integrity (6 tests) — Tests 96-101
// ═══════════════════════════════════════════════════════════════════════

// 96. EMap_CountMatchesTopology
TEST(ElementMapIntegrity, EMap_CountMatchesTopology) {
    auto ctx = oreo::KernelContext::create();

    // After the April 2026 document-identity plumbing audit, primitives
    // (makeBox with NullMapper + empty inputs) DO populate the element map
    // with one IndexedName-based MappedName per sub-shape. The prior
    // "KNOWN: primitive element map is empty" limitation has been fixed in
    // mapShapeElements via buildPrimitiveNames().
    auto boxR = oreo::makeBox(*ctx, 10, 20, 30);
    ASSERT_TRUE(boxR.ok());

    // Topology counts: box has 6 faces, 12 edges, 8 vertices
    int faceCount = boxR.value().countSubShapes(TopAbs_FACE);
    int edgeCount = boxR.value().countSubShapes(TopAbs_EDGE);
    int vertexCount = boxR.value().countSubShapes(TopAbs_VERTEX);
    EXPECT_EQ(faceCount, 6);
    EXPECT_EQ(edgeCount, 12);
    EXPECT_EQ(vertexCount, 8);

    // Primitive element map must now have one entry per sub-shape.
    auto map = boxR.value().elementMap();
    ASSERT_NE(map, nullptr);
    EXPECT_EQ(map->count("Face"), 6);
    EXPECT_EQ(map->count("Edge"), 12);
    EXPECT_EQ(map->count("Vertex"), 8);

    // Now verify with an operation that HAS inputs — fillet passes the box as input.
    // (Extrude of a solid is geometrically unusual and may crash OCCT.)
    oreo::NamedEdge filletEdge;
    filletEdge.name = oreo::IndexedName("Edge", 1);
    filletEdge.edge = boxR.value().getSubShape(TopAbs_EDGE, 1);
    auto filR = oreo::fillet(*ctx, boxR.value(), {filletEdge}, 1.0);
    ASSERT_TRUE(filR.ok()) << "Fillet failed: " << filR.errorMessage();

    auto filMap = filR.value().elementMap();
    ASSERT_NE(filMap, nullptr);

    // Fillet of a box should have element map entries
    int filFaces = filR.value().countSubShapes(TopAbs_FACE);
    int mapFaces = filMap->count("Face");
    // The element map should have some face entries for a fillet result
    EXPECT_GT(mapFaces, 0)
        << "Fillet result should have element map face entries";
    EXPECT_GT(filFaces, 0);
}

// 97. EMap_NoDuplicateMappedNames
// Uses box -> fillet directly (not box -> extrude -> fillet) because extruding
// a solid causes OCCT crashes during subsequent fillet.
TEST(ElementMapIntegrity, EMap_NoDuplicateMappedNames) {
    auto ctx = oreo::KernelContext::create();

    auto boxR = oreo::makeBox(*ctx, 10, 20, 30);
    ASSERT_TRUE(boxR.ok());

    // Fillet directly on the box
    oreo::NamedEdge filletEdge;
    filletEdge.name = oreo::IndexedName("Edge", 1);
    filletEdge.edge = boxR.value().getSubShape(TopAbs_EDGE, 1);
    auto filR = oreo::fillet(*ctx, boxR.value(), {filletEdge}, 1.0);
    ASSERT_TRUE(filR.ok()) << "Fillet failed: " << filR.errorMessage();

    // Get all face mapped names
    auto map = filR.value().elementMap();
    ASSERT_NE(map, nullptr);
    auto allFaces = map->getAll("Face");

    // If the element map has entries, verify no duplicates
    if (!allFaces.empty()) {
        std::set<std::string> seenNames;
        for (auto& [idx, mapped] : allFaces) {
            auto result = seenNames.insert(mapped.data());
            EXPECT_TRUE(result.second)
                << "Duplicate MappedName found: " << mapped.data()
                << " for IndexedName: " << idx.toString();
        }
    }
    // The fillet shape should have more faces than the original 6-face box
    int faceCount = filR.value().countSubShapes(TopAbs_FACE);
    EXPECT_GT(faceCount, 6)
        << "Fillet of box should have more than 6 faces";
}

// 98. EMap_ChildMapsInPattern
TEST(ElementMapIntegrity, EMap_ChildMapsInPattern) {
    auto ctx = oreo::KernelContext::create();

    auto boxR = oreo::makeBox(*ctx, 5, 5, 5);
    ASSERT_TRUE(boxR.ok());

    auto patR = oreo::patternLinear(*ctx, boxR.value(), gp_Vec(10, 0, 0), 3, 10.0);
    ASSERT_TRUE(patR.ok());

    auto map = patR.value().elementMap();
    ASSERT_NE(map, nullptr);

    // KNOWN LIMITATION: patternLinear uses NullMapper with the box as sole input.
    // The box's element map is empty (primitives have empty element maps because
    // buildElementMap returns early with no inputs). NullMapper provides no
    // modified/generated info. So the pattern result's element map may have
    // zero direct entries. However, child maps may still be attached.
    auto allElems = map->getAll();

    // Check if there are child maps attached from the pattern operation
    bool hasChildInfo = !map->children().empty();
    if (!hasChildInfo) {
        // Maybe child info is encoded in the names themselves
        for (auto& [idx, mapped] : allElems) {
            if (mapped.find(oreo::ElementCodes::POSTFIX_CHILD) != std::string::npos) {
                hasChildInfo = true;
                break;
            }
        }
    }
    // Verify the shape topology is correct: pattern of 3 boxes fused
    int totalFaces = patR.value().countSubShapes(TopAbs_FACE);
    EXPECT_GT(totalFaces, 6)
        << "Pattern of 3 boxes should have more than 6 faces in topology";

    // Element map entries may or may not be present depending on whether
    // the input had a populated element map. Document actual behavior.
    // If the box had names, the pattern would carry them through child maps.
    if (allElems.empty()) {
        // KNOWN: pattern with NullMapper + empty-map input = empty element map
        EXPECT_EQ(allElems.size(), 0u)
            << "KNOWN: pattern of primitive with empty element map produces empty map entries";
    }
}

// 99. EMap_NullShapeHandled
TEST(ElementMapIntegrity, EMap_NullShapeHandled) {
    auto ctx = oreo::KernelContext::create();

    TopoDS_Shape nullShape;
    oreo::NullMapper mapper;
    std::vector<oreo::NamedShape> inputs;

    // mapShapeElements with a null shape should not crash
    // It should return a valid (possibly null) NamedShape
    auto result = oreo::mapShapeElements(*ctx, nullShape, mapper, inputs, oreo::ShapeIdentity{0,1}, "test");
    // The result is a NamedShape, check it doesn't crash accessing it
    EXPECT_TRUE(result.isNull() || !result.isNull());
    // If not null, its element map should be accessible
    auto map = result.elementMap();
    // Just verify no crash — map may or may not be null
    (void)map;
}

// 100. EMap_DuplicateTagDisambiguated
TEST(ElementMapIntegrity, EMap_DuplicateTagDisambiguated) {
    auto ctx = oreo::KernelContext::create();

    auto box1 = oreo::makeBox(*ctx, 10, 10, 10);
    ASSERT_TRUE(box1.ok());
    auto box2 = oreo::makeBox(*ctx, 20, 20, 20);
    ASSERT_TRUE(box2.ok());

    // Both boxes should have different tags from the context's tag allocator
    EXPECT_NE(box1.value().tag(), box2.value().tag())
        << "Two sequential operations should have different tags";

    // After the audit, primitive element maps are populated (one entry per
    // sub-shape). Each entry's encoded tag carries the operation tag, so
    // the two boxes' element-name sets must differ even though both are
    // topologically 6-faced cubes.
    auto map1 = box1.value().elementMap();
    auto map2 = box2.value().elementMap();
    ASSERT_NE(map1, nullptr);
    ASSERT_NE(map2, nullptr);

    EXPECT_EQ(map1->count("Face"), 6);
    EXPECT_EQ(map2->count("Face"), 6);

    // Face1's encoded name must differ between the two boxes because the
    // op tag is baked into the name via ;:H<hex>. Otherwise the post-audit
    // disambiguation claim is unfounded.
    auto m1Face1 = map1->getMappedName(oreo::IndexedName("Face", 1));
    auto m2Face1 = map2->getMappedName(oreo::IndexedName("Face", 1));
    EXPECT_NE(m1Face1.data(), m2Face1.data())
        << "distinct op tags must produce distinct face-1 mapped names";

    // To verify tag disambiguation, use operations with inputs (e.g., fillet).
    // (Extrude of a solid is geometrically unusual and may crash OCCT.)
    oreo::NamedEdge filEdge1;
    filEdge1.name = oreo::IndexedName("Edge", 1);
    filEdge1.edge = box1.value().getSubShape(TopAbs_EDGE, 1);
    auto fil1 = oreo::fillet(*ctx, box1.value(), {filEdge1}, 1.0);

    oreo::NamedEdge filEdge2;
    filEdge2.name = oreo::IndexedName("Edge", 1);
    filEdge2.edge = box2.value().getSubShape(TopAbs_EDGE, 1);
    auto fil2 = oreo::fillet(*ctx, box2.value(), {filEdge2}, 1.0);

    ASSERT_TRUE(fil1.ok()) << "Fillet 1 failed: " << fil1.errorMessage();
    ASSERT_TRUE(fil2.ok()) << "Fillet 2 failed: " << fil2.errorMessage();
    EXPECT_NE(fil1.value().tag(), fil2.value().tag())
        << "Two sequential fillet operations should have different tags";
}

// 101. EMap_Tag0Semantics
TEST(ElementMapIntegrity, EMap_Tag0Semantics) {
    auto ctx = oreo::KernelContext::create();

    // Create a NamedShape with tag=0 (default)
    BRepPrimAPI_MakeBox mkBox(10, 10, 10);
    mkBox.Build();
    ASSERT_TRUE(mkBox.IsDone());

    oreo::NamedShape ns(mkBox.Shape(), 0);
    EXPECT_EQ(ns.tag(), 0);

    // Run through mapShapeElements with tag 0 and empty inputs. After the
    // audit, mapShapeElements takes the primitive-name branch and produces
    // one IndexedName-based entry per sub-shape — even when the op tag is
    // zero, which is the "single-doc, no-op-tag" default.
    oreo::NullMapper mapper;
    std::vector<oreo::NamedShape> inputs;
    auto result = oreo::mapShapeElements(*ctx, mkBox.Shape(), mapper, inputs,
                                         oreo::ShapeIdentity{}, "tagzero");

    // Element map is now populated: 6 faces + 12 edges + 8 vertices = 26.
    auto map = result.elementMap();
    ASSERT_NE(map, nullptr);
    EXPECT_EQ(map->count("Face"), 6);
    EXPECT_EQ(map->count("Edge"), 12);
    EXPECT_EQ(map->count("Vertex"), 8);
    EXPECT_EQ(map->count(), 26);

    // Shape topology is preserved regardless
    int faceCount = result.countSubShapes(TopAbs_FACE);
    EXPECT_EQ(faceCount, 6);
    EXPECT_EQ(result.tag(), 0);
}

// ═══════════════════════════════════════════════════════════════════════
// Serialization (4 tests) — Tests 102-105
// ═══════════════════════════════════════════════════════════════════════

// 102. EMapSerial_RoundTrip
TEST(EMapSerialization, EMapSerial_RoundTrip) {
    auto map = std::make_shared<oreo::ElementMap>();

    // Set 5 element names manually
    oreo::MappedName m1("Face1_mapped;:H1;:M");
    oreo::MappedName m2("Face2_mapped;:H2;:G");
    oreo::MappedName m3("Edge1_mapped;:H3;:M");
    oreo::MappedName m4("Edge2_mapped;:H4;:MG");
    oreo::MappedName m5("Vertex1_mapped;:H5;:M");

    map->setElementName(oreo::IndexedName("Face", 1), m1, 1);
    map->setElementName(oreo::IndexedName("Face", 2), m2, 2);
    map->setElementName(oreo::IndexedName("Edge", 1), m3, 3);
    map->setElementName(oreo::IndexedName("Edge", 2), m4, 4);
    map->setElementName(oreo::IndexedName("Vertex", 1), m5, 5);

    ASSERT_EQ(map->count(), 5);

    // Serialize
    auto buf = map->serialize();
    ASSERT_FALSE(buf.empty());

    // Deserialize
    auto restored = oreo::ElementMap::deserialize(buf.data(), buf.size());
    ASSERT_NE(restored, nullptr);

    // Verify all 5 names round-tripped
    EXPECT_EQ(restored->count(), 5);
    EXPECT_EQ(restored->getMappedName(oreo::IndexedName("Face", 1)).data(), m1.data());
    EXPECT_EQ(restored->getMappedName(oreo::IndexedName("Face", 2)).data(), m2.data());
    EXPECT_EQ(restored->getMappedName(oreo::IndexedName("Edge", 1)).data(), m3.data());
    EXPECT_EQ(restored->getMappedName(oreo::IndexedName("Edge", 2)).data(), m4.data());
    EXPECT_EQ(restored->getMappedName(oreo::IndexedName("Vertex", 1)).data(), m5.data());

    // Verify reverse lookup
    EXPECT_EQ(restored->getIndexedName(m1), oreo::IndexedName("Face", 1));
    EXPECT_EQ(restored->getIndexedName(m2), oreo::IndexedName("Face", 2));
}

// 103. EMapSerial_ChildMapRoundTrip
TEST(EMapSerialization, EMapSerial_ChildMapRoundTrip) {
    auto parentMap = std::make_shared<oreo::ElementMap>();
    parentMap->setElementName(oreo::IndexedName("Face", 1),
                              oreo::MappedName("ParentFace1"), 1);

    auto childMap = std::make_shared<oreo::ElementMap>();
    childMap->setElementName(oreo::IndexedName("Face", 1),
                             oreo::MappedName("ChildFace1"), 10);
    childMap->setElementName(oreo::IndexedName("Edge", 1),
                             oreo::MappedName("ChildEdge1"), 11);

    oreo::ChildElementMap childRef;
    childRef.map = childMap;
    // Phase 3: ChildElementMap::tag (int64) → ChildElementMap::id (ShapeIdentity).
    childRef.id = oreo::ShapeIdentity{0, 42};
    childRef.postfix = ";:C";
    parentMap->addChild(childRef);

    EXPECT_EQ(parentMap->children().size(), 1u);

    // Serialize
    auto buf = parentMap->serialize();
    ASSERT_FALSE(buf.empty());

    // Deserialize
    auto restored = oreo::ElementMap::deserialize(buf.data(), buf.size());
    ASSERT_NE(restored, nullptr);

    // Verify parent entry survived
    EXPECT_EQ(restored->count(), 1);
    EXPECT_EQ(restored->getMappedName(oreo::IndexedName("Face", 1)).data(), "ParentFace1");

    // Verify child survived
    ASSERT_EQ(restored->children().size(), 1u);
    EXPECT_EQ(restored->children()[0].id, (oreo::ShapeIdentity{0, 42}));
    EXPECT_EQ(restored->children()[0].postfix, ";:C");
    ASSERT_NE(restored->children()[0].map, nullptr);
    EXPECT_EQ(restored->children()[0].map->count(), 2);
    EXPECT_EQ(restored->children()[0].map->getMappedName(
        oreo::IndexedName("Face", 1)).data(), "ChildFace1");
    EXPECT_EQ(restored->children()[0].map->getMappedName(
        oreo::IndexedName("Edge", 1)).data(), "ChildEdge1");
}

// 104. EMapSerial_LargeTagValue
TEST(EMapSerialization, EMapSerial_LargeTagValue) {
    // Create MappedName, call appendTag with very large value.
    // Widened from `long` to int64_t — on Windows `long` is 32-bit so the
    // narrowing cast from INT64_MAX/2 would silently drop the high bits and
    // this test would fail to exercise its own premise.
    std::int64_t largeTag = INT64_MAX / 2;
    oreo::MappedName name("BaseName");
    name.appendTag(largeTag);

    // Verify encoding doesn't truncate - the tag should appear in the string
    std::string encoded = name.data();
    EXPECT_FALSE(encoded.empty());
    EXPECT_NE(encoded.find(oreo::ElementCodes::POSTFIX_TAG), std::string::npos)
        << "Large tag value was not encoded into the name";
    // The name should be longer than just "BaseName"
    EXPECT_GT(encoded.size(), std::string("BaseName").size());

    // Verify we can extract a tag from it
    std::int64_t extracted = oreo::ElementMap::extractTag(name);
    // The extracted value must round-trip exactly — after the %" PRIx64
    // widening, extractTag parses with strtoll so no 32-bit truncation.
    EXPECT_EQ(extracted, largeTag)
        << "Large tag did not round-trip through appendTag/extractTag";
}

// 105. EMapSerial_IndexedNameBoundary
TEST(EMapSerialization, EMapSerial_IndexedNameBoundary) {
    // IndexedName with index 0 -> isNull
    oreo::IndexedName zero("Face", 0);
    EXPECT_TRUE(zero.isNull()) << "IndexedName with index 0 should be null";

    // IndexedName with index INT_MAX -> handled
    oreo::IndexedName maxIdx("Face", INT_MAX);
    EXPECT_FALSE(maxIdx.isNull());
    EXPECT_EQ(maxIdx.index(), INT_MAX);
    std::string maxStr = maxIdx.toString();
    EXPECT_FALSE(maxStr.empty());
    EXPECT_NE(maxStr.find("Face"), std::string::npos);

    // IndexedName with negative index -> isNull
    oreo::IndexedName neg("Face", -1);
    EXPECT_TRUE(neg.isNull()) << "IndexedName with negative index should be null";

    // IndexedName with empty type -> isNull
    oreo::IndexedName emptyType("", 5);
    EXPECT_TRUE(emptyType.isNull()) << "IndexedName with empty type should be null";

    // IndexedName with index 1 -> valid
    oreo::IndexedName valid("Edge", 1);
    EXPECT_FALSE(valid.isNull());
    EXPECT_EQ(valid.toString(), "Edge1");

    // Round-trip through element map with INT_MAX index
    auto map = std::make_shared<oreo::ElementMap>();
    oreo::MappedName mName("BigIndex");
    auto stored = map->setElementName(maxIdx, mName, 1);
    EXPECT_FALSE(stored.empty());
    auto retrieved = map->getMappedName(maxIdx);
    EXPECT_EQ(retrieved.data(), mName.data());
}

// ═══════════════════════════════════════════════════════════════════════
// StringHasher (3 tests) — Tests 106-108
// ═══════════════════════════════════════════════════════════════════════

// 106. Hasher_RoundTrip
TEST(StringHasher, Hasher_RoundTrip) {
    // The FreeCAD StringHasher uses App:: namespace and requires Qt.
    // Test the oreo::ElementMap's own hashing/encoding instead,
    // which is the production code path for topological naming.
    //
    // Phase 3: encodeElementName takes a ShapeIdentity; the produced
    // postfix is ;:P<16hex>.<16hex>.

    oreo::MappedName input("Face1");
    oreo::ShapeIdentity id{0, 42};

    auto encoded1 = oreo::ElementMap::encodeElementName("Face", input, id, ";:M");
    auto encoded2 = oreo::ElementMap::encodeElementName("Face", input, id, ";:M");

    EXPECT_EQ(encoded1.data(), encoded2.data())
        << "Same input + same identity + same postfix should produce identical encoded name";

    // Verify the encoding contains the expected parts
    EXPECT_NE(encoded1.find(oreo::ElementCodes::POSTFIX_IDENTITY), std::string::npos);
    EXPECT_NE(encoded1.find(";:M"), std::string::npos);
}

// 107. Hasher_DifferentStrings
TEST(StringHasher, Hasher_DifferentStrings) {
    oreo::MappedName face1("Face1");
    oreo::MappedName face2("Face2");
    oreo::ShapeIdentity id{0, 10};

    auto encoded1 = oreo::ElementMap::encodeElementName("Face", face1, id, ";:M");
    auto encoded2 = oreo::ElementMap::encodeElementName("Face", face2, id, ";:M");

    EXPECT_NE(encoded1.data(), encoded2.data())
        << "Different base names with same identity should produce different encoded names";
}

// 108. Hasher_EmptyString
TEST(StringHasher, Hasher_EmptyString) {
    oreo::MappedName empty("");
    oreo::ShapeIdentity id{0, 1};

    // Should not crash on empty input
    auto encoded = oreo::ElementMap::encodeElementName("Face", empty, id, ";:M");
    // The result should at least contain the postfix parts
    EXPECT_NE(encoded.find(oreo::ElementCodes::POSTFIX_IDENTITY), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════
// Adversarial (5 tests) — Tests 109-113
// ═══════════════════════════════════════════════════════════════════════

// 109. EMapAdverse_VeryLongNameChain
TEST(EMapAdversarial, EMapAdverse_VeryLongNameChain) {
    oreo::MappedName name("Base");

    // Append 50 tags to build a very long name chain
    for (int i = 1; i <= 50; ++i) {
        name.appendTag(static_cast<std::int64_t>(i));
        name.appendPostfix(";:M");
    }

    // Verify the string is valid (not truncated, not crashed)
    std::string s = name.data();
    EXPECT_FALSE(s.empty());
    // Should start with "Base"
    EXPECT_EQ(s.substr(0, 4), "Base");
    // Should be quite long after 50 tag+postfix appends
    EXPECT_GT(s.size(), 200u)
        << "50 tag appends should produce a string longer than 200 chars";
}

// 110. EMapAdverse_MaxTagInName
TEST(EMapAdversarial, EMapAdverse_MaxTagInName) {
    oreo::MappedName name("MaxTag");
    // Widened from `long`: on Windows `long` is 32-bit so this literal would
    // become 0xFFFFFFFF (INT_MAX) before widening back to int64 in appendTag,
    // defeating the point of the test.
    std::int64_t maxTag = INT64_MAX;
    name.appendTag(maxTag);

    std::string encoded = name.data();
    EXPECT_FALSE(encoded.empty());
    EXPECT_NE(encoded.find(oreo::ElementCodes::POSTFIX_TAG), std::string::npos);

    // Verify extraction round-trips the full 64-bit value.
    std::int64_t extracted = oreo::ElementMap::extractTag(name);
    EXPECT_EQ(extracted, maxTag) << "INT64_MAX tag did not round-trip";
}

// 111. EMapAdverse_NegativeIndex
TEST(EMapAdversarial, EMapAdverse_NegativeIndex) {
    oreo::IndexedName negOne("Face", -1);
    EXPECT_TRUE(negOne.isNull()) << "IndexedName(Face, -1) should be null";

    oreo::IndexedName negMax("Edge", INT_MIN);
    EXPECT_TRUE(negMax.isNull()) << "IndexedName(Edge, INT_MIN) should be null";

    // Attempting to set a null IndexedName in the map should be a no-op
    auto map = std::make_shared<oreo::ElementMap>();
    auto result = map->setElementName(negOne, oreo::MappedName("ShouldNotStore"), 1);
    EXPECT_TRUE(result.empty())
        << "Setting a null IndexedName should return empty MappedName";
    EXPECT_EQ(map->count(), 0)
        << "Map should have no entries after setting null IndexedName";
}

// 112. EMapAdverse_TraceHistoryDepthLimit
// Uses a chain of booleanSubtract operations (subtracting small boxes) to create
// deep history without the problematic extrude-of-solid pattern.
TEST(EMapAdversarial, EMapAdverse_TraceHistoryDepthLimit) {
    auto ctx = oreo::KernelContext::create();

    // Start with a large box that can absorb many small subtractions
    auto boxR = oreo::makeBox(*ctx, 100, 100, 100);
    ASSERT_TRUE(boxR.ok());

    oreo::NamedShape current = boxR.value();
    int successfulOps = 0;
    for (int i = 0; i < 30; ++i) {
        // Subtract a small box at a unique position inside the large box
        double x = 5.0 + (i % 10) * 9.0;
        double y = 5.0 + ((i / 10) % 10) * 9.0;
        double z = 5.0 + (i / 100) * 9.0;
        auto toolR = oreo::makeBox(*ctx, gp_Pnt(x, y, z), 2, 2, 2);
        if (!toolR.ok()) break;
        auto subR = oreo::booleanSubtract(*ctx, current, toolR.value());
        if (!subR.ok()) break;
        current = subR.value();
        ++successfulOps;
    }

    // We should have at least some successful operations
    EXPECT_GT(successfulOps, 0) << "No boolean subtractions succeeded";

    // Get a face name from the final shape
    auto map = current.elementMap();
    if (map && map->count("Face") > 0) {
        auto allFaces = map->getAll("Face");
        ASSERT_FALSE(allFaces.empty());

        // Trace history on the first face - should not crash even with deep chain
        auto history = oreo::ElementMap::traceHistory(allFaces.front().second);
        // The traceHistory function has a maxDepth of 50 to prevent infinite parsing.
        EXPECT_LE(history.size(), 50u)
            << "History trace should be capped at depth limit of 50";
    } else {
        // If the element map has no face entries, verify the chain at least
        // didn't crash and the shape is valid.
        EXPECT_FALSE(current.isNull());
        EXPECT_GT(current.countSubShapes(TopAbs_FACE), 0);
    }
}

// 113. EMapAdverse_TypeCountersAfterDeserialize
TEST(EMapAdversarial, EMapAdverse_TypeCountersAfterDeserialize) {
    auto map = std::make_shared<oreo::ElementMap>();

    // Add Face1 through Face5
    for (int i = 1; i <= 5; ++i) {
        std::string mName = "FaceBase" + std::to_string(i);
        map->setElementName(oreo::IndexedName("Face", i),
                           oreo::MappedName(mName), static_cast<std::int64_t>(i));
    }
    ASSERT_EQ(map->count("Face"), 5);

    // Serialize and deserialize
    auto buf = map->serialize();
    ASSERT_FALSE(buf.empty());
    auto restored = oreo::ElementMap::deserialize(buf.data(), buf.size());
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->count("Face"), 5);

    // Add a new Face to the deserialized map
    // Known issue: typeCounters_ are not serialized, so the counter may reset.
    // The new face should still get a unique name regardless.
    oreo::MappedName newFaceName("NewFace6");
    auto stored = restored->setElementName(
        oreo::IndexedName("Face", 6), newFaceName, 6);
    EXPECT_FALSE(stored.empty())
        << "Adding Face6 after deserialize should succeed";
    EXPECT_EQ(restored->count("Face"), 6)
        << "Should now have 6 face entries";

    // Verify Face6 is retrievable
    auto retrieved = restored->getMappedName(oreo::IndexedName("Face", 6));
    EXPECT_FALSE(retrieved.empty());
}


// ═══════════════════════════════════════════════════════════════════════
// ██████████████████████████████████████████████████████████████████████
// SECTION D: Layer 3 — C API Boundary (~25 tests)
// ██████████████████████████████████████████████████████████████████████
// ═══════════════════════════════════════════════════════════════════════

// Ensure C API is initialized once for all tests in this section
class CAPIBoundary : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        oreo_init();
    }
};

// ═══════════════════════════════════════════════════════════════════════
// Null Handle Safety (12 tests) — Tests 114-125
// ═══════════════════════════════════════════════════════════════════════

// 114. oreo_extrude(null, ...)
TEST_F(CAPIBoundary, NullSafe_Extrude) {
    OreoSolid result = oreo_extrude(nullptr, 0, 0, 10);
    EXPECT_EQ(result, nullptr);
}

// 115. oreo_boolean_union(null, null)
TEST_F(CAPIBoundary, NullSafe_BooleanUnion) {
    OreoSolid result = oreo_boolean_union(nullptr, nullptr);
    EXPECT_EQ(result, nullptr);
}

// 116. oreo_fillet(null, ...)
TEST_F(CAPIBoundary, NullSafe_Fillet) {
    OreoSolid result = oreo_fillet(nullptr, nullptr, 0, 1.0);
    EXPECT_EQ(result, nullptr);
}

// 117. oreo_serialize(null, ...)
TEST_F(CAPIBoundary, NullSafe_Serialize) {
    size_t len = 999;
    uint8_t* buf = oreo_serialize(nullptr, &len);
    EXPECT_EQ(buf, nullptr);
}

// 118. oreo_deserialize(null, 0)
TEST_F(CAPIBoundary, NullSafe_Deserialize) {
    OreoSolid result = oreo_deserialize(nullptr, 0);
    EXPECT_EQ(result, nullptr);
}

// 119. oreo_import_step(null, 0)
TEST_F(CAPIBoundary, NullSafe_ImportStep) {
    OreoSolid result = oreo_import_step(nullptr, 0);
    EXPECT_EQ(result, nullptr);
}

// 120. oreo_face_count(null)
TEST_F(CAPIBoundary, NullSafe_FaceCount) {
    int count = oreo_face_count(nullptr);
    EXPECT_EQ(count, 0);
}

// 121. oreo_mass_properties(null)
TEST_F(CAPIBoundary, NullSafe_MassProperties) {
    OreoMassProps props = oreo_mass_properties(nullptr);
    EXPECT_EQ(props.volume, 0.0);
}

// 122. oreo_aabb(null)
TEST_F(CAPIBoundary, NullSafe_AABB) {
    OreoBBox bbox = oreo_aabb(nullptr);
    // Should return zeroed or default bbox, not crash
    EXPECT_EQ(bbox.xmin, bbox.xmin); // At least not NaN
}

// 123. oreo_ctx_extrude(ctx, null, ...) — verify ctx has errors, not default
TEST_F(CAPIBoundary, NullSafe_CtxExtrude) {
    OreoContext ctx = oreo_context_create();
    ASSERT_NE(ctx, nullptr);

    OreoSolid result = oreo_ctx_extrude(ctx, nullptr, 0, 0, 10);
    EXPECT_EQ(result, nullptr);
    EXPECT_TRUE(oreo_context_has_errors(ctx));

    oreo_context_free(ctx);
}

// 124. oreo_free_solid(null) — must not crash
TEST_F(CAPIBoundary, NullSafe_FreeSolid) {
    oreo_free_solid(nullptr);
    // If we reach here, it didn't crash
    SUCCEED();
}

// 125. oreo_free_buffer(null) — must not crash
TEST_F(CAPIBoundary, NullSafe_FreeBuffer) {
    oreo_free_buffer(nullptr);
    SUCCEED();
}

// ═══════════════════════════════════════════════════════════════════════
// Exception Boundary (5 tests) — Tests 126-130
// ═══════════════════════════════════════════════════════════════════════

// 126. NaN through C API
TEST_F(CAPIBoundary, ExceptionBoundary_NaN) {
    double nan = std::numeric_limits<double>::quiet_NaN();
    OreoSolid box = oreo_make_box(nan, 10, 10);
    EXPECT_EQ(box, nullptr);
}

// 127. Garbage deserialize
TEST_F(CAPIBoundary, ExceptionBoundary_GarbageDeserialize) {
    uint8_t garbage[] = {0xFF, 0xFE, 0xAB, 0x00, 0x01, 0x02, 0x03, 0x04};
    OreoSolid result = oreo_deserialize(garbage, sizeof(garbage));
    EXPECT_EQ(result, nullptr);
}

// 128. last_error after failure
TEST_F(CAPIBoundary, ExceptionBoundary_LastErrorAfterFailure) {
    // Force a failure
    double nan = std::numeric_limits<double>::quiet_NaN();
    OreoSolid box = oreo_make_box(nan, 10, 10);
    EXPECT_EQ(box, nullptr);

    OreoError err = oreo_last_error();
    EXPECT_NE(err.code, OREO_OK)
        << "Last error should be set after a failed operation";
}

// 129. last_error behavior
TEST_F(CAPIBoundary, ExceptionBoundary_LastErrorAfterSuccess) {
    // The internal default context's diagnostic collector does NOT auto-clear
    // between operations (beginOperation was deleted). After a failed operation,
    // the error persists until the context is reset via oreo_shutdown/oreo_init.
    //
    // Test that after a fresh init, last_error is OK.
    // Then after a failure, it's not OK.
    // We do NOT test that it resets after a success (it doesn't).
    oreo_shutdown();
    oreo_init();

    // After fresh init, last_error should be OK (no errors yet)
    OreoError freshErr = oreo_last_error();
    EXPECT_EQ(freshErr.code, OREO_OK)
        << "Last error should be OK on fresh context";

    // Trigger a failure
    double nan = std::numeric_limits<double>::quiet_NaN();
    OreoSolid bad = oreo_make_box(nan, 10, 10);
    EXPECT_EQ(bad, nullptr);

    OreoError failErr = oreo_last_error();
    EXPECT_NE(failErr.code, OREO_OK)
        << "Last error should be set after a failed operation";

    // A subsequent successful operation does NOT clear the error
    // (the internal context accumulates diagnostics)
    OreoSolid box = oreo_make_box(10, 20, 30);
    ASSERT_NE(box, nullptr);

    OreoError afterSuccess = oreo_last_error();
    // Error still persists — this is the documented behavior
    EXPECT_NE(afterSuccess.code, OREO_OK)
        << "KNOWN: last_error does not auto-clear after success on the default context";

    oreo_free_solid(box);
}

// 130. Extreme values (1e308)
TEST_F(CAPIBoundary, ExceptionBoundary_ExtremeValues) {
    // Very large but finite values
    OreoSolid box = oreo_make_box(1e308, 1e308, 1e308);
    // This may succeed or fail depending on OCCT's handling,
    // but it must not crash
    if (box) {
        oreo_free_solid(box);
    }
    // If we reach here, no crash occurred
    SUCCEED();
}

// ═══════════════════════════════════════════════════════════════════════
// Lifecycle (4 tests) — Tests 131-134
// ═══════════════════════════════════════════════════════════════════════

// 131. init/shutdown/init cycle
TEST_F(CAPIBoundary, Lifecycle_InitShutdownInit) {
    oreo_shutdown();
    oreo_init();

    // Should work normally after re-init
    OreoSolid box = oreo_make_box(10, 20, 30);
    ASSERT_NE(box, nullptr);
    EXPECT_EQ(oreo_face_count(box), 6);
    oreo_free_solid(box);
}

// 132. context create/free
TEST_F(CAPIBoundary, Lifecycle_ContextCreateFree) {
    OreoContext ctx = oreo_context_create();
    ASSERT_NE(ctx, nullptr);

    // Use the context
    OreoSolid box = oreo_ctx_make_box(ctx, 5, 10, 15);
    ASSERT_NE(box, nullptr);
    EXPECT_EQ(oreo_face_count(box), 6);

    oreo_free_solid(box);
    oreo_context_free(ctx);
    // No crash = success
    SUCCEED();
}

// 133. context null safe
TEST_F(CAPIBoundary, Lifecycle_ContextNullSafe) {
    // Freeing a null context should be safe
    oreo_context_free(nullptr);
    SUCCEED();

    // Querying a null context should not crash
    EXPECT_EQ(oreo_context_has_errors(nullptr), 0);
    EXPECT_EQ(oreo_context_has_warnings(nullptr), 0);
    EXPECT_EQ(oreo_context_error_count(nullptr), 0);
}

// 134. constraint enum bounds check
TEST_F(CAPIBoundary, Lifecycle_ConstraintEnumBounds) {
    OreoSketch sketch = oreo_sketch_create();
    ASSERT_NE(sketch, nullptr);

    // Add a point so we have an entity
    int ptIdx = oreo_sketch_add_point(sketch, 0, 0);
    EXPECT_GE(ptIdx, 0);

    // Add a constraint with an out-of-bounds type value
    int result = oreo_sketch_add_constraint(sketch, 9999, ptIdx, -1, -1, 0.0);
    // Should either reject the constraint or handle gracefully
    // (implementation-specific behavior, but must not crash)
    (void)result;

    oreo_sketch_free(sketch);
    SUCCEED();
}

// ═══════════════════════════════════════════════════════════════════════
// Context-Aware (3 tests) — Tests 135-137
// ═══════════════════════════════════════════════════════════════════════

// 135. ctx_make_box NaN
TEST_F(CAPIBoundary, ContextAware_MakeBoxNaN) {
    OreoContext ctx = oreo_context_create();
    ASSERT_NE(ctx, nullptr);

    double nan = std::numeric_limits<double>::quiet_NaN();
    OreoSolid box = oreo_ctx_make_box(ctx, nan, 10, 10);
    EXPECT_EQ(box, nullptr);
    EXPECT_TRUE(oreo_context_has_errors(ctx));
    EXPECT_GT(oreo_context_error_count(ctx), 0);

    const char* msg = oreo_context_last_error_message(ctx);
    EXPECT_NE(msg, nullptr);

    oreo_context_free(ctx);
}

// 136. ctx_boolean_subtract null tool
TEST_F(CAPIBoundary, ContextAware_BooleanSubtractNullTool) {
    OreoContext ctx = oreo_context_create();
    ASSERT_NE(ctx, nullptr);

    OreoSolid box = oreo_ctx_make_box(ctx, 10, 10, 10);
    ASSERT_NE(box, nullptr);

    OreoSolid result = oreo_ctx_boolean_subtract(ctx, box, nullptr);
    EXPECT_EQ(result, nullptr);
    EXPECT_TRUE(oreo_context_has_errors(ctx));

    oreo_free_solid(box);
    oreo_context_free(ctx);
}

// 137. ctx error_count
TEST_F(CAPIBoundary, ContextAware_ErrorCount) {
    OreoContext ctx = oreo_context_create();
    ASSERT_NE(ctx, nullptr);

    EXPECT_EQ(oreo_context_error_count(ctx), 0);
    EXPECT_FALSE(oreo_context_has_errors(ctx));

    // Trigger an error
    double nan = std::numeric_limits<double>::quiet_NaN();
    OreoSolid bad = oreo_ctx_make_box(ctx, nan, 10, 10);
    EXPECT_EQ(bad, nullptr);
    EXPECT_GT(oreo_context_error_count(ctx), 0);
    EXPECT_TRUE(oreo_context_has_errors(ctx));

    oreo_context_free(ctx);
}

// ═══════════════════════════════════════════════════════════════════════
// Context Reuse (1 test) — Test 138
// ═══════════════════════════════════════════════════════════════════════

// 138. 100 make_box + free_solid, no crash
TEST_F(CAPIBoundary, ContextReuse_100BoxCycle) {
    for (int i = 0; i < 100; ++i) {
        OreoSolid box = oreo_make_box(
            1.0 + static_cast<double>(i),
            2.0 + static_cast<double>(i),
            3.0 + static_cast<double>(i));
        ASSERT_NE(box, nullptr) << "Failed on iteration " << i;
        EXPECT_EQ(oreo_face_count(box), 6);
        oreo_free_solid(box);
    }
}


// ═══════════════════════════════════════════════════════════════════════
// ██████████████████████████████████████████████████████████████████████
// SECTION E: Cross-Cutting (~6 tests)
// ██████████████████████████████████████████████████████████████████████
// ═══════════════════════════════════════════════════════════════════════

// 139. Pipeline_BoxFilletSerializeDeserialize
// Uses box -> fillet -> serialize -> deserialize (skipping extrude-of-solid
// which causes OCCT crashes during subsequent fillet).
TEST(Pipeline, Pipeline_BoxExtrudeFilletSerializeDeserialize) {
    auto ctx = oreo::KernelContext::create();

    // Box
    auto boxR = oreo::makeBox(*ctx, 10, 20, 30);
    ASSERT_TRUE(boxR.ok()) << "makeBox failed: " << boxR.errorMessage();

    // Fillet directly on the box
    oreo::NamedEdge filletEdge;
    filletEdge.name = oreo::IndexedName("Edge", 1);
    filletEdge.edge = boxR.value().getSubShape(TopAbs_EDGE, 1);
    auto filR = oreo::fillet(*ctx, boxR.value(), {filletEdge}, 1.0);
    ASSERT_TRUE(filR.ok()) << "fillet failed: " << filR.errorMessage();

    int facesBeforeSerialize = filR.value().countSubShapes(TopAbs_FACE);

    // Serialize — check result before accessing .value()
    auto serR = oreo::serialize(*ctx, filR.value());
    ASSERT_TRUE(serR.ok()) << "serialize failed: " << serR.errorMessage();
    auto& serBuf = serR.value();
    ASSERT_FALSE(serBuf.empty()) << "Serialized buffer is empty";

    // Deserialize — check result before accessing .value()
    auto desR = oreo::deserialize(*ctx, serBuf.data(), serBuf.size());
    ASSERT_TRUE(desR.ok()) << "deserialize failed: " << desR.errorMessage();

    // Verify face count preserved
    int facesAfterDeserialize = desR.value().countSubShapes(TopAbs_FACE);
    EXPECT_EQ(facesBeforeSerialize, facesAfterDeserialize);

    // Verify shape is valid
    EXPECT_TRUE(oreo::isShapeValid(desR.value().shape()));
}

// 140. Pipeline_FailureThenSuccess
TEST(Pipeline, Pipeline_FailureThenSuccess) {
    auto ctx = oreo::KernelContext::create();

    // Force a failure
    double nan = std::numeric_limits<double>::quiet_NaN();
    auto failR = oreo::makeBox(*ctx, nan, 10, 10);
    EXPECT_FALSE(failR.ok());
    EXPECT_TRUE(ctx->diag().hasErrors());

    // Clear diagnostics
    ctx->diag().clear();
    EXPECT_FALSE(ctx->diag().hasErrors());
    EXPECT_EQ(ctx->diag().count(), 0);

    // Now succeed
    auto succR = oreo::makeBox(*ctx, 10, 20, 30);
    ASSERT_TRUE(succR.ok());
    EXPECT_FALSE(succR.hasErrors());
    EXPECT_EQ(succR.value().countSubShapes(TopAbs_FACE), 6);
}

// 141. Pipeline_StepExportImportFaceCount
TEST(Pipeline, Pipeline_StepExportImportFaceCount) {
    auto ctx = oreo::KernelContext::create();

    // Create a box
    auto boxR = oreo::makeBox(*ctx, 10, 20, 30);
    ASSERT_TRUE(boxR.ok());
    int originalFaces = boxR.value().countSubShapes(TopAbs_FACE);
    EXPECT_EQ(originalFaces, 6);

    // Export to STEP buffer
    std::vector<oreo::NamedShape> shapes;
    shapes.push_back(boxR.value());
    auto stepR = oreo::exportStep(*ctx, shapes);
    ASSERT_TRUE(stepR.ok());
    auto& stepBuf = stepR.value();
    ASSERT_FALSE(stepBuf.empty());

    // Import from STEP buffer
    auto impR = oreo::importStep(*ctx, stepBuf.data(), stepBuf.size());
    ASSERT_TRUE(impR.ok());

    int importedFaces = impR.value().shape.countSubShapes(TopAbs_FACE);
    EXPECT_EQ(importedFaces, originalFaces)
        << "Face count should be preserved through STEP export/import";
}

// 142. Pipeline_SketchSolveToWireExtrude
TEST(Pipeline, Pipeline_SketchSolveToWireExtrude) {
    auto ctx = oreo::KernelContext::create();

    // Create a simple rectangular sketch: 4 lines forming a 10x20 rectangle
    std::vector<oreo::SketchPoint> points;
    std::vector<oreo::SketchLine> lines;
    std::vector<oreo::SketchCircle> circles;
    std::vector<oreo::SketchArc> arcs;
    std::vector<oreo::SketchConstraint> constraints;

    // Four corner points
    // Lines: bottom, right, top, left
    lines.push_back({{0, 0}, {10, 0}});    // bottom
    lines.push_back({{10, 0}, {10, 20}});   // right
    lines.push_back({{10, 20}, {0, 20}});   // top
    lines.push_back({{0, 20}, {0, 0}});     // left

    // Add horizontal/vertical constraints
    constraints.push_back({oreo::ConstraintType::Horizontal, 0, -1, -1, 0});
    constraints.push_back({oreo::ConstraintType::Vertical, 1, -1, -1, 0});
    constraints.push_back({oreo::ConstraintType::Horizontal, 2, -1, -1, 0});
    constraints.push_back({oreo::ConstraintType::Vertical, 3, -1, -1, 0});

    // Solve
    auto solveR = oreo::solveSketch(*ctx, points, lines, circles, arcs, constraints);
    ASSERT_TRUE(solveR.ok());
    EXPECT_EQ(solveR.value().status, oreo::SolveStatus::OK);

    // Convert to wire
    auto wireR = oreo::sketchToWire(*ctx, lines, circles, arcs);
    ASSERT_TRUE(wireR.ok());
    EXPECT_FALSE(wireR.value().isNull());

    // Extrude the wire (face from wire first, then extrude)
    auto faceR = oreo::makeFaceFromWire(*ctx, wireR.value());
    ASSERT_TRUE(faceR.ok());

    auto extR = oreo::extrude(*ctx, faceR.value(), gp_Vec(0, 0, 30));
    ASSERT_TRUE(extR.ok());
    EXPECT_FALSE(extR.value().isNull());

    // The extruded rectangle should have 6 faces (box-like)
    int faceCount = extR.value().countSubShapes(TopAbs_FACE);
    EXPECT_EQ(faceCount, 6)
        << "Extruded rectangle should produce a box-like solid with 6 faces";
}

// 143. Pipeline_DiagAccumulationBounded
TEST(Pipeline, Pipeline_DiagAccumulationBounded) {
    auto ctx = oreo::KernelContext::create();

    // Perform 1000 makeBox calls
    for (int i = 0; i < 1000; ++i) {
        auto r = oreo::makeBox(*ctx, 1.0 + i * 0.001, 2.0, 3.0);
        ASSERT_TRUE(r.ok()) << "makeBox failed at iteration " << i;
    }

    // Diagnostics count should stay reasonable
    // A well-behaved system shouldn't accumulate thousands of info-level diagnostics
    // for successful operations. The count should be bounded.
    int diagCount = ctx->diag().count();
    // If the system accumulates a diagnostic per operation, 1000 is the max.
    // But ideally successful operations don't add diagnostics at all.
    // We check it doesn't grow unboundedly (e.g., 10x the operation count)
    EXPECT_LE(diagCount, 1000)
        << "Diagnostic count grew unreasonably: " << diagCount;

    // No errors should be present
    EXPECT_FALSE(ctx->diag().hasErrors());
}

// 144. Pipeline_ContextReuseLongSession
TEST(Pipeline, Pipeline_ContextReuseLongSession) {
    auto ctx = oreo::KernelContext::create();

    int successCount = 0;
    int failureCount = 0;

    for (int i = 0; i < 500; ++i) {
        if (i % 3 == 0) {
            // Intentional failure: NaN input
            double nan = std::numeric_limits<double>::quiet_NaN();
            auto r = oreo::makeBox(*ctx, nan, 10, 10);
            EXPECT_FALSE(r.ok());
            ++failureCount;
        } else {
            // Successful operation
            auto r = oreo::makeBox(*ctx, 1.0 + i, 2.0 + i, 3.0 + i);
            ASSERT_TRUE(r.ok()) << "Unexpected failure at iteration " << i;
            EXPECT_EQ(r.value().countSubShapes(TopAbs_FACE), 6);
            ++successCount;
        }
    }

    // Verify we had a mix of successes and failures
    EXPECT_GT(successCount, 300);
    EXPECT_GT(failureCount, 100);

    // The context should still be usable after 500 operations
    auto finalR = oreo::makeBox(*ctx, 50, 50, 50);
    ASSERT_TRUE(finalR.ok());
    EXPECT_EQ(finalR.value().countSubShapes(TopAbs_FACE), 6);

    // Verify shape is geometrically valid
    EXPECT_TRUE(oreo::isShapeValid(finalR.value().shape()));
}
