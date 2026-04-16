// test_mesh.cpp — Comprehensive tessellation tests.

#include <gtest/gtest.h>

#include "core/kernel_context.h"
#include "geometry/oreo_geometry.h"
#include "mesh/oreo_mesh.h"
#include "query/oreo_query.h"

#include <cmath>

namespace {
const double PI = 3.14159265358979323846;
}

// ═══════════════════════════════════════════════════════════════
// Basic tessellation
// ═══════════════════════════════════════════════════════════════

TEST(Mesh, TessellateBox) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 20, 30).value();
    ASSERT_FALSE(box.isNull());

    auto mesh = oreo::tessellate(*ctx, box);
    EXPECT_FALSE(mesh.empty());

    // A box has 6 faces, each with 2 triangles = 12 triangles total
    EXPECT_EQ(mesh.totalTriangles, 12);
    EXPECT_EQ(mesh.faceGroups.size(), 6u);

    // Each face group should have exactly 6 indices (2 triangles × 3 vertices)
    for (auto& group : mesh.faceGroups) {
        EXPECT_EQ(group.indexCount, 6u) << "Face " << group.faceId << " has wrong index count";
    }

    // Verify positions are not empty
    EXPECT_EQ(mesh.positions.size(), mesh.totalVertices * 3u);
    EXPECT_TRUE(mesh.hasNormals);
    EXPECT_EQ(mesh.normals.size(), mesh.positions.size());

    // Verify indices are within range
    for (auto idx : mesh.indices) {
        EXPECT_LT(idx, static_cast<uint32_t>(mesh.totalVertices));
    }
}

TEST(Mesh, TessellateSphere) {
    auto ctx = oreo::KernelContext::create();
    auto sphere = oreo::makeSphere(*ctx, 10.0).value();
    ASSERT_FALSE(sphere.isNull());

    auto mesh = oreo::tessellate(*ctx, sphere);
    EXPECT_FALSE(mesh.empty());

    // Sphere should have many triangles (curved surface)
    EXPECT_GT(mesh.totalTriangles, 12);
    EXPECT_GT(mesh.totalVertices, 8);

    // Sphere has 1 face in OCCT
    EXPECT_GE(mesh.faceGroups.size(), 1u);
}

TEST(Mesh, TessellateCylinder) {
    auto ctx = oreo::KernelContext::create();
    auto cyl = oreo::makeCylinder(*ctx, 5.0, 20.0).value();
    ASSERT_FALSE(cyl.isNull());

    auto mesh = oreo::tessellate(*ctx, cyl);
    EXPECT_FALSE(mesh.empty());

    // Cylinder has 3 faces (top, bottom, lateral)
    EXPECT_EQ(mesh.faceGroups.size(), 3u);
    EXPECT_GT(mesh.totalTriangles, 6);
}

// ═══════════════════════════════════════════════════════════════
// Face groups and selection
// ═══════════════════════════════════════════════════════════════

TEST(Mesh, FaceGroupsMatchFaceCount) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 10, 10).value();

    // Fillet one edge
    auto edgesR = oreo::getEdges(*ctx, box);
    ASSERT_TRUE(edgesR.ok());
    auto edges = edgesR.value();
    ASSERT_GE(edges.size(), 1u);
    auto filleted = oreo::fillet(*ctx, box, {edges[0]}, 2.0).value();
    ASSERT_FALSE(filleted.isNull());

    int expectedFaces = filleted.countSubShapes(TopAbs_FACE);
    auto mesh = oreo::tessellate(*ctx, filleted);
    EXPECT_FALSE(mesh.empty());

    // Number of face groups should match BRep face count
    EXPECT_EQ(static_cast<int>(mesh.faceGroups.size()), expectedFaces);
}

TEST(Mesh, FaceGroupNamesExist) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 10, 10).value();
    auto mesh = oreo::tessellate(*ctx, box);

    for (auto& group : mesh.faceGroups) {
        EXPECT_FALSE(group.faceName.empty())
            << "Face " << group.faceId << " has no name";
        EXPECT_GT(group.faceId, 0);
        EXPECT_GT(group.indexCount, 0u);
    }
}

// ═══════════════════════════════════════════════════════════════
// Edge wireframe
// ═══════════════════════════════════════════════════════════════

TEST(Mesh, EdgeWireframe) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 10, 10).value();
    auto mesh = oreo::tessellate(*ctx, box);

    // Box has 12 edges
    EXPECT_EQ(mesh.totalEdges, 12);
    EXPECT_EQ(mesh.edges.size(), 12u);

    for (auto& edge : mesh.edges) {
        EXPECT_GT(edge.edgeId, 0);
        EXPECT_FALSE(edge.edgeName.empty());
        // Each edge should have at least 2 points (start + end)
        EXPECT_GE(edge.vertices.size(), 6u);  // 2 points × 3 floats
    }
}

// ═══════════════════════════════════════════════════════════════
// LOD (Level of Detail)
// ═══════════════════════════════════════════════════════════════

TEST(Mesh, LODCoarseVsFine) {
    auto ctx = oreo::KernelContext::create();
    auto sphere = oreo::makeSphere(*ctx, 10.0).value();
    ASSERT_FALSE(sphere.isNull());

    // Coarse mesh
    oreo::MeshParams coarse;
    coarse.linearDeflection = 2.0;
    coarse.angularDeflection = 40.0;
    auto meshCoarse = oreo::tessellate(*ctx, sphere, coarse);

    // Fine mesh
    oreo::MeshParams fine;
    fine.linearDeflection = 0.01;
    fine.angularDeflection = 5.0;
    auto meshFine = oreo::tessellate(*ctx, sphere, fine);

    EXPECT_FALSE(meshCoarse.empty());
    EXPECT_FALSE(meshFine.empty());

    // Fine mesh should have significantly more triangles
    EXPECT_GT(meshFine.totalTriangles, meshCoarse.totalTriangles);
    EXPECT_GT(meshFine.totalVertices, meshCoarse.totalVertices);
}

// ═══════════════════════════════════════════════════════════════
// Normal vectors
// ═══════════════════════════════════════════════════════════════

TEST(Mesh, NormalsAreUnitLength) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 10, 10).value();
    auto mesh = oreo::tessellate(*ctx, box);

    ASSERT_TRUE(mesh.hasNormals);
    ASSERT_EQ(mesh.normals.size(), mesh.positions.size());

    for (size_t i = 0; i + 2 < mesh.normals.size(); i += 3) {
        float nx = mesh.normals[i];
        float ny = mesh.normals[i+1];
        float nz = mesh.normals[i+2];
        float len = std::sqrt(nx*nx + ny*ny + nz*nz);
        EXPECT_NEAR(len, 1.0f, 0.01f) << "Normal at vertex " << (i/3) << " is not unit length";
    }
}

// ═══════════════════════════════════════════════════════════════
// Error handling
// ═══════════════════════════════════════════════════════════════

TEST(Mesh, TessellateNullShape) {
    auto ctx = oreo::KernelContext::create();
    oreo::NamedShape null;
    auto mesh = oreo::tessellate(*ctx, null);
    EXPECT_TRUE(mesh.empty());
    EXPECT_TRUE(ctx->diag.hasErrors());
}

TEST(Mesh, TessellateInvalidDeflection) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 10, 10).value();
    oreo::MeshParams bad;
    bad.linearDeflection = -1.0;
    auto mesh = oreo::tessellate(*ctx, box, bad);
    EXPECT_TRUE(mesh.empty());
    EXPECT_TRUE(ctx->diag.hasErrors());
}

// ═══════════════════════════════════════════════════════════════
// No-normals mode
// ═══════════════════════════════════════════════════════════════

TEST(Mesh, WithoutNormals) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 10, 10).value();
    oreo::MeshParams params;
    params.computeNormals = false;
    auto mesh = oreo::tessellate(*ctx, box, params);
    EXPECT_FALSE(mesh.empty());
    // Normals array should still be populated (with defaults) but hasNormals = false
    EXPECT_EQ(mesh.totalTriangles, 12);
}

// ═══════════════════════════════════════════════════════════════
// No-edges mode
// ═══════════════════════════════════════════════════════════════

TEST(Mesh, WithoutEdges) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 10, 10).value();
    oreo::MeshParams params;
    params.extractEdges = false;
    auto mesh = oreo::tessellate(*ctx, box, params);
    EXPECT_FALSE(mesh.empty());
    EXPECT_EQ(mesh.edges.size(), 0u);
    EXPECT_EQ(mesh.totalEdges, 0);
}

// ═══════════════════════════════════════════════════════════════
// Mesh diagnostics
// ═══════════════════════════════════════════════════════════════

TEST(Mesh, DiagnosticsForBox) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 10, 10).value();
    auto mesh = oreo::tessellate(*ctx, box);

    // Box should have diagnostics for all 6 faces
    EXPECT_EQ(mesh.diagnostics.size(), 6u);
    EXPECT_TRUE(mesh.allFacesOK());
    EXPECT_EQ(mesh.failedFaces, 0);
    EXPECT_EQ(mesh.degenerateTriangles, 0);

    // Every face should be OK
    for (auto& diag : mesh.diagnostics) {
        EXPECT_EQ(diag.status, oreo::FaceMeshStatus::OK);
        EXPECT_GT(diag.triangleCount, 0);
        EXPECT_EQ(diag.degenerateCount, 0);
    }
}

TEST(Mesh, DiagnosticsForSphere) {
    auto ctx = oreo::KernelContext::create();
    auto sphere = oreo::makeSphere(*ctx, 10.0).value();
    auto mesh = oreo::tessellate(*ctx, sphere);

    EXPECT_FALSE(mesh.diagnostics.empty());
    // Sphere should tessellate fine
    EXPECT_TRUE(mesh.allFacesOK());
}

// ═══════════════════════════════════════════════════════════════
// Incremental meshing
// ═══════════════════════════════════════════════════════════════

TEST(Mesh, IncrementalMeshCacheHit) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 10, 10).value();

    oreo::MeshCache cache;

    // First tessellation — all cache misses
    auto mesh1 = oreo::tessellateIncremental(*ctx, box, cache);
    EXPECT_FALSE(mesh1.empty());
    EXPECT_EQ(mesh1.totalTriangles, 12);
    EXPECT_EQ(cache.cachedFaceCount(), 6);

    // Second tessellation — all cache hits (same shape, same params)
    auto mesh2 = oreo::tessellateIncremental(*ctx, box, cache);
    EXPECT_FALSE(mesh2.empty());
    EXPECT_EQ(mesh2.totalTriangles, 12);

    // Results should be identical
    EXPECT_EQ(mesh1.totalVertices, mesh2.totalVertices);
    EXPECT_EQ(mesh1.totalTriangles, mesh2.totalTriangles);
    EXPECT_EQ(mesh1.faceGroups.size(), mesh2.faceGroups.size());
}

TEST(Mesh, IncrementalMeshInvalidateOnParamChange) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 10, 10).value();

    oreo::MeshCache cache;

    // First pass with coarse params
    oreo::MeshParams coarse;
    coarse.linearDeflection = 2.0;
    auto mesh1 = oreo::tessellateIncremental(*ctx, box, cache, coarse);
    EXPECT_EQ(cache.cachedFaceCount(), 6);

    // Second pass with different params — cache should be invalidated
    oreo::MeshParams fine;
    fine.linearDeflection = 0.01;
    auto mesh2 = oreo::tessellateIncremental(*ctx, box, cache, fine);
    EXPECT_EQ(cache.cachedFaceCount(), 6);  // Re-cached with new params
}

TEST(Mesh, IncrementalMeshInvalidateSingleFace) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 10, 10).value();

    oreo::MeshCache cache;

    // First pass — fills cache
    auto mesh1 = oreo::tessellateIncremental(*ctx, box, cache);
    EXPECT_EQ(cache.cachedFaceCount(), 6);

    // Invalidate one face
    cache.invalidateFace(3);
    EXPECT_EQ(cache.cachedFaceCount(), 5);

    // Second pass — should retessellate only face 3
    auto mesh2 = oreo::tessellateIncremental(*ctx, box, cache);
    EXPECT_EQ(cache.cachedFaceCount(), 6);  // Face 3 re-cached
}

// ═══════════════════════════════════════════════════════════════
// Deterministic output
// ═══════════════════════════════════════════════════════════════

TEST(Mesh, DeterministicOutput) {
    // Tessellate the same shape 10 times, verify identical output
    std::vector<float> firstPositions;
    std::vector<float> firstNormals;
    std::vector<uint32_t> firstIndices;

    for (int iter = 0; iter < 10; ++iter) {
        auto ctx = oreo::KernelContext::create();
        auto box = oreo::makeBox(*ctx, 15, 25, 35).value();
        auto mesh = oreo::tessellate(*ctx, box);
        ASSERT_FALSE(mesh.empty());

        if (iter == 0) {
            firstPositions = mesh.positions;
            firstNormals = mesh.normals;
            firstIndices = mesh.indices;
        } else {
            ASSERT_EQ(mesh.positions.size(), firstPositions.size())
                << "Position count differs on iteration " << iter;
            ASSERT_EQ(mesh.normals.size(), firstNormals.size())
                << "Normal count differs on iteration " << iter;
            ASSERT_EQ(mesh.indices.size(), firstIndices.size())
                << "Index count differs on iteration " << iter;

            // Verify byte-identical positions
            for (size_t i = 0; i < mesh.positions.size(); ++i) {
                EXPECT_EQ(mesh.positions[i], firstPositions[i])
                    << "Position differs at index " << i << " on iteration " << iter;
            }

            // Verify byte-identical indices
            for (size_t i = 0; i < mesh.indices.size(); ++i) {
                EXPECT_EQ(mesh.indices[i], firstIndices[i])
                    << "Index differs at index " << i << " on iteration " << iter;
            }
        }
    }
}

TEST(Mesh, DeterministicSphereOutput) {
    // Sphere has more complex tessellation — verify determinism
    int firstTriangles = -1;
    int firstVertices = -1;

    for (int iter = 0; iter < 5; ++iter) {
        auto ctx = oreo::KernelContext::create();
        auto sphere = oreo::makeSphere(*ctx, 10.0).value();
        auto mesh = oreo::tessellate(*ctx, sphere);
        ASSERT_FALSE(mesh.empty());

        if (iter == 0) {
            firstTriangles = mesh.totalTriangles;
            firstVertices = mesh.totalVertices;
        } else {
            EXPECT_EQ(mesh.totalTriangles, firstTriangles)
                << "Triangle count differs on iteration " << iter;
            EXPECT_EQ(mesh.totalVertices, firstVertices)
                << "Vertex count differs on iteration " << iter;
        }
    }
}
