// oreo_mesh.h — Production-grade tessellation API.
//
// Features:
//   1. Tessellation API — BRepMesh → indexed triangles ready for WebGL
//   2. Face/edge IDs — stable element-map names for selection
//   3. LOD controls — linear + angular deflection
//   4. Incremental meshing — cache per-face, remesh only changed faces
//   5. Selection data — faceId/edgeId for pick correlation
//   6. Visual attributes — RGBA per face group
//   7. Deterministic output — same shape → identical vertex/index ordering
//   8. Mesh diagnostics — report failed faces, degenerate triangles

#ifndef OREO_MESH_H
#define OREO_MESH_H

#include "core/kernel_context.h"
#include "naming/named_shape.h"

#include <TopoDS_Face.hxx>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace oreo {

// ─── Mesh quality parameters ─────────────────────────────────

struct MeshParams {
    double linearDeflection = 0.1;    // Chord tolerance (mm) — lower = more triangles
    double angularDeflection = 20.0;  // Angular tolerance (degrees) — lower = smoother curves
    bool computeNormals = true;       // Compute per-vertex normals for smooth shading
    bool extractEdges = true;         // Extract edge wireframe line strips
    bool parallel = true;             // Use parallel meshing (OCCT InParallel)
};

// ─── Per-face group in the mesh ──────────────────────────────

struct FaceGroup {
    int faceId;              // 1-based face index (matches element map)
    std::string faceName;    // MappedName from element map (for stable selection)
    uint32_t indexStart;     // Offset into the indices array
    uint32_t indexCount;     // Number of indices for this face (multiple of 3)
    float r, g, b, a;       // Face color (default: 0.7, 0.7, 0.7, 1.0)
};

// ─── Per-edge wireframe segment ──────────────────────────────

struct EdgeSegment {
    int edgeId;              // 1-based edge index (matches element map)
    std::string edgeName;    // MappedName from element map
    std::vector<float> vertices;  // Line strip [x,y,z, x,y,z, ...] — flat
};

// ─── Per-face diagnostic ─────────────────────────────────────

enum class FaceMeshStatus {
    OK,                      // Tessellated successfully
    NoTriangulation,         // BRep_Tool::Triangulation returned null
    DegenerateTriangles,     // Some triangles have zero or near-zero area
    ZeroNormals,             // Some vertex normals could not be computed
};

struct FaceMeshDiagnostic {
    int faceId;
    FaceMeshStatus status;
    int triangleCount;       // Number of triangles for this face
    int degenerateCount;     // Number of degenerate triangles (zero-area)
    std::string detail;      // Human-readable detail
};

// ─── Tessellation result ─────────────────────────────────────

struct MeshResult {
    // Geometry buffers (flat arrays, ready for GPU upload)
    std::vector<float> positions;      // 3 floats per vertex: [x,y,z, ...]
    std::vector<float> normals;        // 3 floats per vertex: [nx,ny,nz, ...]
    std::vector<uint32_t> indices;     // 3 per triangle: [i,j,k, ...]

    // Per-face grouping (for selection and per-face colors)
    std::vector<FaceGroup> faceGroups;

    // Edge wireframe (for outline display)
    std::vector<EdgeSegment> edges;

    // Diagnostics (per-face tessellation status)
    std::vector<FaceMeshDiagnostic> diagnostics;

    // Summary
    int totalTriangles = 0;
    int totalVertices = 0;
    int totalEdges = 0;
    int failedFaces = 0;        // Faces that could not be tessellated
    int degenerateTriangles = 0; // Total degenerate triangles across all faces
    bool hasNormals = false;

    // Check if mesh was generated
    bool empty() const { return positions.empty(); }

    // Check if all faces tessellated successfully
    bool allFacesOK() const { return failedFaces == 0; }
};

// ─── Mesh Cache (for incremental meshing) ────────────────────

// Stores per-face tessellation results so that when only some faces change,
// only those faces need to be retessellated.
class MeshCache {
public:
    MeshCache() = default;

    // Check if a face has a cached mesh
    bool hasCachedFace(int faceId) const;

    // Get cached face mesh data
    struct CachedFace {
        std::vector<float> positions;
        std::vector<float> normals;
        std::vector<uint32_t> indices;  // 0-based within this face
        int triangleCount;
        FaceMeshDiagnostic diagnostic;
        uint64_t geometryFingerprint = 0;
    };

    const CachedFace* getCachedFace(int faceId) const;

    // Store a face mesh in the cache
    void cacheFace(int faceId, CachedFace&& data);

    // Invalidate a specific face (marks it for retessellation)
    void invalidateFace(int faceId);

    // Invalidate all faces
    void invalidateAll();

    // Get the params used for cached meshes (to detect LOD changes)
    const MeshParams& cachedParams() const { return params_; }
    void setCachedParams(const MeshParams& p) { params_ = p; }

    // Check if params match (if not, all caches are invalid)
    bool paramsMatch(const MeshParams& p) const;

    // Check if cached face exists AND its geometry fingerprint matches
    bool isCacheValid(int faceId, uint64_t currentFingerprint) const;

    int cachedFaceCount() const { return static_cast<int>(cache_.size()); }

private:
    std::map<int, CachedFace> cache_;
    MeshParams params_;
};

// ─── Geometry fingerprinting ────────────────────────────────

// Compute a hash fingerprint of face geometry (bounding box, surface type, UV bounds,
// tolerance, orientation). Used by incremental meshing to detect geometry changes.
uint64_t computeFaceFingerprint(const TopoDS_Face& face);

// ─── Tessellation API ────────────────────────────────────────

// Full tessellation (no cache).
MeshResult tessellate(KernelContext& ctx,
                      const NamedShape& shape,
                      const MeshParams& params = {});

// Incremental tessellation (uses cache, remeshes only changed/invalidated faces).
// Pass the same cache across calls. If params change, cache is automatically invalidated.
MeshResult tessellateIncremental(KernelContext& ctx,
                                 const NamedShape& shape,
                                 MeshCache& cache,
                                 const MeshParams& params = {});

} // namespace oreo

#endif // OREO_MESH_H
