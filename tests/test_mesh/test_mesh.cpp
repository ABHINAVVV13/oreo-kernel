// test_mesh.cpp — Comprehensive tessellation tests.
//
// tessellate()/tessellateIncremental() return OperationResult<MeshResult>.
// Happy-path tests unwrap with .value(); error-path tests assert !.ok().

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

    auto meshR = oreo::tessellate(*ctx, box);
    ASSERT_TRUE(meshR.ok());
    auto mesh = std::move(meshR).value();
    EXPECT_FALSE(mesh.empty());

    // A box has 6 faces, each with 2 triangles = 12 triangles total
    EXPECT_EQ(mesh.totalTriangles, 12);
    EXPECT_EQ(mesh.faceGroups.size(), 6u);

    for (auto& group : mesh.faceGroups) {
        EXPECT_EQ(group.indexCount, 6u) << "Face " << group.faceId << " has wrong index count";
    }

    EXPECT_EQ(mesh.positions.size(), mesh.totalVertices * 3u);
    EXPECT_TRUE(mesh.hasNormals);
    EXPECT_EQ(mesh.normals.size(), mesh.positions.size());

    for (auto idx : mesh.indices) {
        EXPECT_LT(idx, static_cast<uint32_t>(mesh.totalVertices));
    }
}

TEST(Mesh, TessellateSphere) {
    auto ctx = oreo::KernelContext::create();
    auto sphere = oreo::makeSphere(*ctx, 10.0).value();
    ASSERT_FALSE(sphere.isNull());

    auto mesh = oreo::tessellate(*ctx, sphere).value();
    EXPECT_FALSE(mesh.empty());

    EXPECT_GT(mesh.totalTriangles, 12);
    EXPECT_GT(mesh.totalVertices, 8);
    EXPECT_GE(mesh.faceGroups.size(), 1u);
}

TEST(Mesh, TessellateCylinder) {
    auto ctx = oreo::KernelContext::create();
    auto cyl = oreo::makeCylinder(*ctx, 5.0, 20.0).value();
    ASSERT_FALSE(cyl.isNull());

    auto mesh = oreo::tessellate(*ctx, cyl).value();
    EXPECT_FALSE(mesh.empty());

    EXPECT_EQ(mesh.faceGroups.size(), 3u);
    EXPECT_GT(mesh.totalTriangles, 6);
}

// ═══════════════════════════════════════════════════════════════
// Face groups and selection
// ═══════════════════════════════════════════════════════════════

TEST(Mesh, FaceGroupsMatchFaceCount) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 10, 10).value();

    auto edgesR = oreo::getEdges(*ctx, box);
    ASSERT_TRUE(edgesR.ok());
    auto edges = edgesR.value();
    ASSERT_GE(edges.size(), 1u);
    auto filleted = oreo::fillet(*ctx, box, {edges[0]}, 2.0).value();
    ASSERT_FALSE(filleted.isNull());

    int expectedFaces = filleted.countSubShapes(TopAbs_FACE);
    auto mesh = oreo::tessellate(*ctx, filleted).value();
    EXPECT_FALSE(mesh.empty());
    EXPECT_EQ(static_cast<int>(mesh.faceGroups.size()), expectedFaces);
}

TEST(Mesh, FaceGroupNamesExist) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 10, 10).value();
    auto mesh = oreo::tessellate(*ctx, box).value();

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
    auto mesh = oreo::tessellate(*ctx, box).value();

    EXPECT_EQ(mesh.totalEdges, 12);
    EXPECT_EQ(mesh.edges.size(), 12u);

    for (auto& edge : mesh.edges) {
        EXPECT_GT(edge.edgeId, 0);
        EXPECT_FALSE(edge.edgeName.empty());
        EXPECT_GE(edge.vertices.size(), 6u);
    }
}

// ═══════════════════════════════════════════════════════════════
// LOD (Level of Detail)
// ═══════════════════════════════════════════════════════════════

TEST(Mesh, LODCoarseVsFine) {
    auto ctx = oreo::KernelContext::create();
    auto sphere = oreo::makeSphere(*ctx, 10.0).value();
    ASSERT_FALSE(sphere.isNull());

    oreo::MeshParams coarse;
    coarse.linearDeflection = 2.0;
    coarse.angularDeflection = 40.0;
    auto meshCoarse = oreo::tessellate(*ctx, sphere, coarse).value();

    oreo::MeshParams fine;
    fine.linearDeflection = 0.01;
    fine.angularDeflection = 5.0;
    auto meshFine = oreo::tessellate(*ctx, sphere, fine).value();

    EXPECT_FALSE(meshCoarse.empty());
    EXPECT_FALSE(meshFine.empty());

    EXPECT_GT(meshFine.totalTriangles, meshCoarse.totalTriangles);
    EXPECT_GT(meshFine.totalVertices, meshCoarse.totalVertices);
}

// ═══════════════════════════════════════════════════════════════
// Normal vectors
// ═══════════════════════════════════════════════════════════════

TEST(Mesh, NormalsAreUnitLength) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 10, 10).value();
    auto mesh = oreo::tessellate(*ctx, box).value();

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
// Error handling (fail-closed via OperationResult)
// ═══════════════════════════════════════════════════════════════

TEST(Mesh, TessellateNullShape) {
    auto ctx = oreo::KernelContext::create();
    oreo::NamedShape null;
    auto meshR = oreo::tessellate(*ctx, null);
    EXPECT_FALSE(meshR.ok());
    EXPECT_TRUE(meshR.hasErrors());
    EXPECT_TRUE(ctx->diag().hasErrors());
}

TEST(Mesh, TessellateInvalidDeflection) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 10, 10).value();
    oreo::MeshParams bad;
    bad.linearDeflection = -1.0;
    auto meshR = oreo::tessellate(*ctx, box, bad);
    EXPECT_FALSE(meshR.ok());
    EXPECT_TRUE(meshR.hasErrors());
    EXPECT_FALSE(meshR.errorMessage().empty());
}

// ═══════════════════════════════════════════════════════════════
// No-normals mode
// ═══════════════════════════════════════════════════════════════

TEST(Mesh, WithoutNormals) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 10, 10).value();
    oreo::MeshParams params;
    params.computeNormals = false;
    auto mesh = oreo::tessellate(*ctx, box, params).value();
    EXPECT_FALSE(mesh.empty());
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
    auto mesh = oreo::tessellate(*ctx, box, params).value();
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
    auto mesh = oreo::tessellate(*ctx, box).value();

    EXPECT_EQ(mesh.diagnostics.size(), 6u);
    EXPECT_TRUE(mesh.allFacesOK());
    EXPECT_EQ(mesh.failedFaces, 0);
    EXPECT_EQ(mesh.degenerateTriangles, 0);

    for (auto& diag : mesh.diagnostics) {
        EXPECT_EQ(diag.status, oreo::FaceMeshStatus::OK);
        EXPECT_GT(diag.triangleCount, 0);
        EXPECT_EQ(diag.degenerateCount, 0);
    }
}

TEST(Mesh, DiagnosticsForSphere) {
    auto ctx = oreo::KernelContext::create();
    auto sphere = oreo::makeSphere(*ctx, 10.0).value();
    auto mesh = oreo::tessellate(*ctx, sphere).value();

    EXPECT_FALSE(mesh.diagnostics.empty());
    EXPECT_TRUE(mesh.allFacesOK());
}

// ═══════════════════════════════════════════════════════════════
// Incremental meshing
// ═══════════════════════════════════════════════════════════════

TEST(Mesh, IncrementalMeshCacheHit) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 10, 10).value();

    oreo::MeshCache cache;

    auto mesh1 = oreo::tessellateIncremental(*ctx, box, cache).value();
    EXPECT_FALSE(mesh1.empty());
    EXPECT_EQ(mesh1.totalTriangles, 12);
    EXPECT_EQ(cache.cachedFaceCount(), 6);

    auto mesh2 = oreo::tessellateIncremental(*ctx, box, cache).value();
    EXPECT_FALSE(mesh2.empty());
    EXPECT_EQ(mesh2.totalTriangles, 12);

    EXPECT_EQ(mesh1.totalVertices, mesh2.totalVertices);
    EXPECT_EQ(mesh1.totalTriangles, mesh2.totalTriangles);
    EXPECT_EQ(mesh1.faceGroups.size(), mesh2.faceGroups.size());
}

TEST(Mesh, IncrementalMeshInvalidateOnParamChange) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 10, 10).value();

    oreo::MeshCache cache;

    oreo::MeshParams coarse;
    coarse.linearDeflection = 2.0;
    auto mesh1 = oreo::tessellateIncremental(*ctx, box, cache, coarse).value();
    EXPECT_EQ(cache.cachedFaceCount(), 6);

    oreo::MeshParams fine;
    fine.linearDeflection = 0.01;
    auto mesh2 = oreo::tessellateIncremental(*ctx, box, cache, fine).value();
    EXPECT_EQ(cache.cachedFaceCount(), 6);
}

TEST(Mesh, IncrementalMeshInvalidateSingleFace) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 10, 10).value();

    oreo::MeshCache cache;

    auto mesh1 = oreo::tessellateIncremental(*ctx, box, cache).value();
    EXPECT_EQ(cache.cachedFaceCount(), 6);

    cache.invalidateFace(3);
    EXPECT_EQ(cache.cachedFaceCount(), 5);

    auto mesh2 = oreo::tessellateIncremental(*ctx, box, cache).value();
    EXPECT_EQ(cache.cachedFaceCount(), 6);
}

// ═══════════════════════════════════════════════════════════════
// Deterministic output
// ═══════════════════════════════════════════════════════════════

TEST(Mesh, DeterministicOutput) {
    std::vector<float> firstPositions;
    std::vector<float> firstNormals;
    std::vector<uint32_t> firstIndices;

    for (int iter = 0; iter < 10; ++iter) {
        auto ctx = oreo::KernelContext::create();
        auto box = oreo::makeBox(*ctx, 15, 25, 35).value();
        auto mesh = oreo::tessellate(*ctx, box).value();
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

            for (size_t i = 0; i < mesh.positions.size(); ++i) {
                EXPECT_EQ(mesh.positions[i], firstPositions[i])
                    << "Position differs at index " << i << " on iteration " << iter;
            }

            for (size_t i = 0; i < mesh.indices.size(); ++i) {
                EXPECT_EQ(mesh.indices[i], firstIndices[i])
                    << "Index differs at index " << i << " on iteration " << iter;
            }
        }
    }
}

TEST(Mesh, DeterministicSphereOutput) {
    int firstTriangles = -1;
    int firstVertices = -1;

    for (int iter = 0; iter < 5; ++iter) {
        auto ctx = oreo::KernelContext::create();
        auto sphere = oreo::makeSphere(*ctx, 10.0).value();
        auto mesh = oreo::tessellate(*ctx, sphere).value();
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

// ═══════════════════════════════════════════════════════════════
// Crease preservation / normal quality
// ═══════════════════════════════════════════════════════════════

// A cube has 12 triangles, and at every corner three faces meet at 90°.
// Because face vertices are not shared across faces (verified by the assembly
// logic), every corner position should appear in the positions array at least
// 3 times — once per incident face — and at each of those copies the vertex
// normal must point along that face's outward axis (±X, ±Y, or ±Z), not the
// averaged corner direction.
TEST(Mesh, CubeCornersHaveSharpCreases) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 10, 10).value();
    auto mesh = oreo::tessellate(*ctx, box).value();

    ASSERT_EQ(mesh.totalTriangles, 12);
    ASSERT_TRUE(mesh.hasNormals);

    // Minimum vertex count: 6 faces × 4 corners = 24 unique (face, corner) pairs.
    // If normals were averaged across faces, the implementation would collapse
    // shared positions into 8 vertices — so anything ≥ 24 means faces keep
    // their own vertices.
    EXPECT_GE(mesh.totalVertices, 24)
        << "Box has fewer than 24 vertices; faces likely share verts, which "
           "would destroy sharp edges.";

    // Every normal must be axis-aligned (±X, ±Y, or ±Z) for a cube.
    // An averaged corner normal would point along (±1,±1,±1)/√3 — far from
    // any axis. If we see that, creases are being averaged.
    for (size_t v = 0; v < mesh.normals.size(); v += 3) {
        float nx = mesh.normals[v];
        float ny = mesh.normals[v + 1];
        float nz = mesh.normals[v + 2];
        float maxComp = std::max({std::abs(nx), std::abs(ny), std::abs(nz)});
        // Axis-aligned normal: exactly one component = 1, others = 0.
        // Allow small float slop (1e-3).
        EXPECT_NEAR(maxComp, 1.0f, 1e-3f)
            << "Vertex " << (v / 3) << " normal (" << nx << "," << ny << "," << nz
            << ") is not axis-aligned — creases are being averaged across faces.";
    }
}

// ═══════════════════════════════════════════════════════════════
// Adaptive deflection
// ═══════════════════════════════════════════════════════════════

// In Auto mode, a 1 mm cube and a 10 m cube should produce roughly similar
// triangle counts — the tessellation adapts to shape size. With the old
// Absolute-only default (linearDeflection = 0.1 mm), a 1 mm cube would be
// heavily over-tessellated and a 10 m cube heavily under-tessellated.
TEST(Mesh, AutoDeflectionScalesWithShape) {
    auto ctx = oreo::KernelContext::create();
    auto smallBox = oreo::makeBox(*ctx, 1.0, 1.0, 1.0).value();
    auto largeBox = oreo::makeBox(*ctx, 10000.0, 10000.0, 10000.0).value();

    oreo::MeshParams params;
    params.deflectionMode = oreo::DeflectionMode::Auto;

    auto smallMesh = oreo::tessellate(*ctx, smallBox, params).value();
    auto largeMesh = oreo::tessellate(*ctx, largeBox, params).value();

    // Both boxes have 6 flat faces × 2 triangles = 12 triangles minimum.
    // In Auto mode, neither should balloon. Allow up to 4× spread for
    // discretisation differences but prohibit 10× or worse.
    EXPECT_GE(smallMesh.totalTriangles, 12);
    EXPECT_GE(largeMesh.totalTriangles, 12);

    double ratio = static_cast<double>(std::max(smallMesh.totalTriangles,
                                                largeMesh.totalTriangles)) /
                   static_cast<double>(std::min(smallMesh.totalTriangles,
                                                largeMesh.totalTriangles));
    EXPECT_LT(ratio, 4.0)
        << "Adaptive deflection off: small=" << smallMesh.totalTriangles
        << " vs large=" << largeMesh.totalTriangles
        << " (ratio " << ratio << "x)";
}

// Relative mode uses linearDeflection as a fraction of the bounding-box
// diagonal. A smaller fraction should produce more triangles.
TEST(Mesh, RelativeDeflectionRespondsToFraction) {
    auto ctx = oreo::KernelContext::create();
    auto sphere = oreo::makeSphere(*ctx, 50.0).value();

    oreo::MeshParams coarse;
    coarse.deflectionMode = oreo::DeflectionMode::Relative;
    coarse.linearDeflection = 0.02; // 2% of diagonal
    auto coarseMesh = oreo::tessellate(*ctx, sphere, coarse).value();

    oreo::MeshParams fine;
    fine.deflectionMode = oreo::DeflectionMode::Relative;
    fine.linearDeflection = 0.001; // 0.1% of diagonal
    auto fineMesh = oreo::tessellate(*ctx, sphere, fine).value();

    EXPECT_GT(fineMesh.totalTriangles, coarseMesh.totalTriangles * 2)
        << "Fine relative deflection should produce many more triangles than coarse";
}

TEST(Mesh, AbsoluteDeflectionIsBackwardsCompatible) {
    // Default params (deflectionMode = Absolute, linearDeflection = 0.1) must
    // continue to match the pre-adaptive behaviour so existing callers don't
    // see a silent triangle-count change.
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 20, 30).value();
    auto mesh = oreo::tessellate(*ctx, box).value();
    EXPECT_EQ(mesh.totalTriangles, 12);
    EXPECT_EQ(mesh.faceGroups.size(), 6u);
}

TEST(Mesh, ResolveLinearDeflectionMatchesModes) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 100, 100, 100).value();
    double diag = std::sqrt(3.0) * 100.0; // cube diagonal ≈ 173.2 mm

    oreo::MeshParams abs;
    abs.deflectionMode = oreo::DeflectionMode::Absolute;
    abs.linearDeflection = 0.25;
    EXPECT_NEAR(oreo::resolveLinearDeflection(box, abs), 0.25, 1e-9);

    oreo::MeshParams rel;
    rel.deflectionMode = oreo::DeflectionMode::Relative;
    rel.linearDeflection = 0.01;
    EXPECT_NEAR(oreo::resolveLinearDeflection(box, rel), diag * 0.01, 1e-6);

    oreo::MeshParams auto_;
    auto_.deflectionMode = oreo::DeflectionMode::Auto;
    EXPECT_NEAR(oreo::resolveLinearDeflection(box, auto_), diag * 0.005, 1e-6);
}

// ═══════════════════════════════════════════════════════════════
// Selection: triangle → face ID
// ═══════════════════════════════════════════════════════════════

TEST(Mesh, FaceIdForTriangleMatchesGroups) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 20, 30).value();
    auto mesh = oreo::tessellate(*ctx, box).value();

    ASSERT_GT(mesh.totalTriangles, 0);

    // Walk every triangle and make sure faceIdForTriangle agrees with the
    // FaceGroup ranges. This is the invariant renderer picking relies on.
    for (int t = 0; t < mesh.totalTriangles; ++t) {
        uint32_t idxPos = static_cast<uint32_t>(t) * 3u;
        int expected = -1;
        for (auto& g : mesh.faceGroups) {
            if (idxPos >= g.indexStart && idxPos < g.indexStart + g.indexCount) {
                expected = g.faceId;
                break;
            }
        }
        EXPECT_EQ(mesh.faceIdForTriangle(t), expected)
            << "Triangle " << t << " maps to wrong face ID";
    }
}

TEST(Mesh, FaceIdForTriangleOutOfRange) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 10, 10).value();
    auto mesh = oreo::tessellate(*ctx, box).value();

    EXPECT_EQ(mesh.faceIdForTriangle(static_cast<uint32_t>(mesh.totalTriangles)), -1);
    EXPECT_EQ(mesh.faceIdForTriangle(static_cast<uint32_t>(mesh.totalTriangles + 5)), -1);

    // Empty mesh: no face groups, always -1.
    oreo::MeshResult empty;
    EXPECT_EQ(empty.faceIdForTriangle(0), -1);
}

// Tessellating a sphere should produce smooth normals, not facet-per-triangle.
// We verify by checking that vertex normals roughly point radially outward
// from the sphere center.
TEST(Mesh, SphereHasSmoothNormals) {
    auto ctx = oreo::KernelContext::create();
    const double R = 10.0;
    auto sphere = oreo::makeSphere(*ctx, R).value();
    auto mesh = oreo::tessellate(*ctx, sphere).value();

    ASSERT_TRUE(mesh.hasNormals);
    ASSERT_GT(mesh.totalVertices, 0);

    int misaligned = 0;
    for (size_t i = 0, n = 0; i < mesh.positions.size(); i += 3, n += 3) {
        float px = mesh.positions[i];
        float py = mesh.positions[i + 1];
        float pz = mesh.positions[i + 2];
        float len = std::sqrt(px * px + py * py + pz * pz);
        if (len < 1e-4f) continue; // skip pole cap vertex
        // Radial direction from sphere center (which is at origin).
        float rx = px / len, ry = py / len, rz = pz / len;
        float nx = mesh.normals[n];
        float ny = mesh.normals[n + 1];
        float nz = mesh.normals[n + 2];
        float dot = rx * nx + ry * ny + rz * nz;
        // A smooth-shaded sphere normal should be within ~3° of the radial
        // direction (cos 3° ≈ 0.9986). Facet normals would drift to ~15°
        // for typical tessellation, giving dot ≈ 0.97.
        if (dot < 0.995f) ++misaligned;
    }
    // Allow a handful of pole/seam vertices to drift, but the vast majority
    // must be close to radial.
    EXPECT_LT(misaligned, static_cast<int>(mesh.totalVertices) / 20)
        << misaligned << " of " << mesh.totalVertices
        << " sphere normals deviate more than 3° from radial — "
           "smooth shading is degraded.";
}
