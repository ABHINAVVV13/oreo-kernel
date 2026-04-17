// oreo_mesh.cpp — Production-grade tessellation using FreeCAD's extraction patterns.
//
// Core extraction functions adapted from FreeCAD 1.0's Part::Tools (LGPL-2.1+):
//   - getTriangulation() — per-face triangle extraction with orientation
//   - getPolygonOnTriangulation() — edge polyline extraction
//   - getPointNormals() — smooth normals via triangle adjacency averaging,
//     with surface-based refinement for curved faces
//
// Pipeline:
//   1. BRepMesh_IncrementalMesh → tessellate all faces
//   2. For each face → getTriangulation() → vertices + triangles
//   3. getPointNormals() → smooth vertex normals
//   4. For each edge → getPolygonOnTriangulation() → line strips
//   5. Map face/edge indices to element map names

#include "oreo_mesh.h"
#include "core/diagnostic_scope.h"
#include "core/validation.h"
#include "query/oreo_query.h"

#include <BRep_Tool.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <Bnd_Box.hxx>
#include <Geom_Surface.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <GeomLProp_SLProps.hxx>
#include <Poly_Connect.hxx>
#include <Poly_Polygon3D.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangle.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <gp.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace oreo {

// ─── FreeCAD extraction functions (adapted from Part::Tools) ─────────

namespace mesh_tools {

// Extract triangulation from a face. Vertices are transformed to global space.
// Triangle indices are 0-based. Winding order is corrected for face orientation.
// From FreeCAD Part::Tools::getTriangulation (LGPL-2.1+)
bool getTriangulation(const TopoDS_Face& face,
                      std::vector<gp_Pnt>& points,
                      std::vector<Poly_Triangle>& facets)
{
    TopLoc_Location loc;
    Handle(Poly_Triangulation) hTria = BRep_Tool::Triangulation(face, loc);
    if (hTria.IsNull()) return false;

    gp_Trsf transf;
    bool identity = true;
    if (!loc.IsIdentity()) {
        identity = false;
        transf = loc.Transformation();
    }

    TopAbs_Orientation orient = face.Orientation();

    Standard_Integer nbNodes = hTria->NbNodes();
    Standard_Integer nbTriangles = hTria->NbTriangles();

    points.reserve(nbNodes);
    facets.reserve(nbTriangles);

    for (int i = 1; i <= nbNodes; i++) {
        gp_Pnt p = hTria->Node(i);
        if (!identity) p.Transform(transf);
        points.push_back(p);
    }

    for (int i = 1; i <= nbTriangles; i++) {
        Standard_Integer n1, n2, n3;
        hTria->Triangle(i).Get(n1, n2, n3);
        --n1; --n2; --n3;  // Convert to 0-based

        // Correct winding for reversed faces
        if (orient != TopAbs_FORWARD) {
            std::swap(n1, n2);
        }
        facets.emplace_back(n1, n2, n3);
    }

    return true;
}

// Extract edge polyline from face triangulation.
// From FreeCAD Part::Tools::getPolygonOnTriangulation (LGPL-2.1+)
bool getPolygonOnTriangulation(const TopoDS_Edge& edge,
                               const TopoDS_Face& face,
                               std::vector<gp_Pnt>& points)
{
    TopLoc_Location loc;
    Handle(Poly_Triangulation) hTria = BRep_Tool::Triangulation(face, loc);
    if (hTria.IsNull()) return false;

    Handle(Poly_PolygonOnTriangulation) hPoly =
        BRep_Tool::PolygonOnTriangulation(edge, hTria, loc);
    if (hPoly.IsNull()) return false;

    gp_Trsf transf;
    bool identity = true;
    if (!loc.IsIdentity()) {
        identity = false;
        transf = loc.Transformation();
    }

    Standard_Integer nbNodes = hPoly->NbNodes();
    points.reserve(nbNodes);
    const TColStd_Array1OfInteger& indices = hPoly->Nodes();

    for (Standard_Integer i = indices.Lower(); i <= indices.Upper(); i++) {
        gp_Pnt p = hTria->Node(indices(i));
        if (!identity) p.Transform(transf);
        points.push_back(p);
    }

    return true;
}

// Extract 3D edge polygon (for free edges not on any face).
// From FreeCAD Part::Tools::getPolygon3D (LGPL-2.1+)
bool getPolygon3D(const TopoDS_Edge& edge, std::vector<gp_Pnt>& points)
{
    TopLoc_Location loc;
    Handle(Poly_Polygon3D) hPoly = BRep_Tool::Polygon3D(edge, loc);
    if (hPoly.IsNull()) return false;

    gp_Trsf transf;
    bool identity = true;
    if (!loc.IsIdentity()) {
        identity = false;
        transf = loc.Transformation();
    }

    Standard_Integer nbNodes = hPoly->NbNodes();
    points.reserve(nbNodes);
    const TColgp_Array1OfPnt& nodes = hPoly->Nodes();

    for (int i = 1; i <= nbNodes; i++) {
        gp_Pnt p = nodes(i);
        if (!identity) p.Transform(transf);
        points.push_back(p);
    }

    return true;
}

// Compute smooth vertex normals by averaging adjacent triangle normals.
// From FreeCAD Part::Tools::getPointNormals (LGPL-2.1+)
void getPointNormals(const std::vector<gp_Pnt>& points,
                     const std::vector<Poly_Triangle>& facets,
                     std::vector<gp_Vec>& vertexnormals)
{
    vertexnormals.resize(points.size());

    for (const auto& it : facets) {
        Standard_Integer n1, n2, n3;
        it.Get(n1, n2, n3);

        gp_Vec v1(points[n1].XYZ());
        gp_Vec v2(points[n2].XYZ());
        gp_Vec v3(points[n3].XYZ());
        gp_Vec n = (v2 - v1) ^ (v3 - v1);

        vertexnormals[n1] += n;
        vertexnormals[n2] += n;
        vertexnormals[n3] += n;
    }

    for (auto& it : vertexnormals) {
        if (it.Magnitude() > gp::Resolution()) {
            it.Normalize();
        } else {
            it = gp_Vec(0, 0, 1);
        }
    }
}

// Refine vertex normals using surface UV projection for curved faces.
// More accurate than triangle-averaging for curved geometry.
// From FreeCAD Part::Tools::getPointNormals (surface variant, LGPL-2.1+)
void refineNormalsFromSurface(const std::vector<gp_Pnt>& points,
                              const TopoDS_Face& face,
                              std::vector<gp_Vec>& vertexnormals)
{
    if (points.size() != vertexnormals.size()) return;

    Handle(Geom_Surface) hSurface = BRep_Tool::Surface(face);
    if (hSurface.IsNull()) return;

    for (std::size_t i = 0; i < points.size(); i++) {
        try {
            GeomAPI_ProjectPointOnSurf proj(points[i], hSurface);
            if (proj.NbPoints() == 0) continue;

            Standard_Real u, v;
            proj.Parameters(1, u, v);

            GeomLProp_SLProps props(hSurface, u, v, 2, gp::Resolution());
            if (!props.IsNormalDefined()) continue;

            gp_Dir normal = props.Normal();
            gp_Vec temp(normal);

            if (temp * vertexnormals[i] < 0.0) temp = -temp;
            vertexnormals[i] = temp;
        } catch (...) {
            // Surface normal refinement failed — keep averaged normals.
            // This is non-fatal; averaged normals from getPointNormals() are
            // an acceptable fallback. UV projection can fail for degenerate
            // surface patches or singular points without affecting mesh quality.
        }
    }
}

// Apply face orientation to normals.
void applyFaceOrientation(const TopoDS_Face& face, std::vector<gp_Vec>& normals) {
    if (face.Orientation() == TopAbs_REVERSED) {
        for (auto& n : normals) n.Reverse();
    }
}

// Apply location transformation to normals.
void applyTransformationOnNormals(const TopLoc_Location& loc, std::vector<gp_Vec>& normals) {
    if (loc.IsIdentity()) return;
    gp_Trsf transf = loc.Transformation();
    for (auto& n : normals) n.Transform(transf);
}

// Count degenerate triangles (zero or near-zero area).
int countDegenerateTriangles(const std::vector<gp_Pnt>& points,
                             const std::vector<Poly_Triangle>& facets,
                             double tolerance = 1e-10)
{
    int count = 0;
    for (const auto& tri : facets) {
        Standard_Integer n1, n2, n3;
        tri.Get(n1, n2, n3);
        if (n1 < 0 || n1 >= (int)points.size() ||
            n2 < 0 || n2 >= (int)points.size() ||
            n3 < 0 || n3 >= (int)points.size()) {
            ++count;
            continue;
        }
        gp_Vec v1(points[n1], points[n2]);
        gp_Vec v2(points[n1], points[n3]);
        gp_Vec cross = v1 ^ v2;
        double area = cross.Magnitude() * 0.5;
        if (area < tolerance) ++count;
    }
    return count;
}

// Count normals that are zero or couldn't be computed.
int countZeroNormals(const std::vector<gp_Vec>& normals) {
    int count = 0;
    for (auto& n : normals) {
        if (n.Magnitude() < 1e-10) ++count;
    }
    return count;
}

} // namespace mesh_tools

// ─── MeshCache implementation ────────────────────────────────────────

bool MeshCache::hasCachedFace(int faceId) const {
    return cache_.count(faceId) > 0;
}

const MeshCache::CachedFace* MeshCache::getCachedFace(int faceId) const {
    auto it = cache_.find(faceId);
    return (it != cache_.end()) ? &it->second : nullptr;
}

void MeshCache::cacheFace(int faceId, CachedFace&& data) {
    cache_[faceId] = std::move(data);
}

void MeshCache::invalidateFace(int faceId) {
    cache_.erase(faceId);
}

void MeshCache::invalidateAll() {
    cache_.clear();
}

bool MeshCache::paramsMatch(const MeshParams& p) const {
    return std::abs(params_.linearDeflection - p.linearDeflection) < 1e-12
        && std::abs(params_.angularDeflection - p.angularDeflection) < 1e-12
        && params_.computeNormals == p.computeNormals;
}

bool MeshCache::isCacheValid(int faceId, uint64_t currentFingerprint) const {
    auto it = cache_.find(faceId);
    if (it == cache_.end()) return false;
    return it->second.geometryFingerprint == currentFingerprint;
}

// ─── Geometry fingerprinting ─────────────────────────────────────────

uint64_t computeFaceFingerprint(const TopoDS_Face& face) {
    uint64_t hash = 14695981039346656037ULL; // FNV-1a offset basis
    auto mix = [&](const void* data, size_t len) {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len; ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ULL; // FNV-1a prime
        }
    };

    // Bounding box
    Bnd_Box box;
    BRepBndLib::Add(face, box);
    if (!box.IsVoid()) {
        double vals[6];
        box.Get(vals[0], vals[1], vals[2], vals[3], vals[4], vals[5]);
        mix(vals, sizeof(vals));
    }

    // Surface type + UV bounds
    BRepAdaptor_Surface adapt(face);
    int surfType = static_cast<int>(adapt.GetType());
    mix(&surfType, sizeof(int));
    double umin = adapt.FirstUParameter(), umax = adapt.LastUParameter();
    double vmin = adapt.FirstVParameter(), vmax = adapt.LastVParameter();
    mix(&umin, sizeof(double)); mix(&umax, sizeof(double));
    mix(&vmin, sizeof(double)); mix(&vmax, sizeof(double));

    // Tolerance + orientation
    double tol = BRep_Tool::Tolerance(face);
    mix(&tol, sizeof(double));
    int orient = static_cast<int>(face.Orientation());
    mix(&orient, sizeof(int));

    return hash;
}

// ─── Shared face tessellation logic ──────────────────────────────────

namespace {

// Tessellate a single face and produce positions, normals, indices, and diagnostics.
// Used by both tessellate() and tessellateIncremental().
MeshCache::CachedFace tessellateSingleFace(
    KernelContext& ctx,
    const TopoDS_Face& face,
    int faceIdx,
    const MeshParams& params)
{
    MeshCache::CachedFace cached;
    cached.triangleCount = 0;
    cached.diagnostic.faceId = faceIdx;
    cached.diagnostic.triangleCount = 0;
    cached.diagnostic.degenerateCount = 0;

    std::vector<gp_Pnt> points;
    std::vector<Poly_Triangle> facets;
    if (!mesh_tools::getTriangulation(face, points, facets)) {
        cached.diagnostic.status = FaceMeshStatus::NoTriangulation;
        cached.diagnostic.detail = "Face " + std::to_string(faceIdx) + " has no triangulation";
        return cached;
    }

    // Compute normals
    std::vector<gp_Vec> normals;
    bool hasNormalIssues = false;
    if (params.computeNormals) {
        mesh_tools::getPointNormals(points, facets, normals);
        mesh_tools::refineNormalsFromSurface(points, face, normals);
        mesh_tools::applyFaceOrientation(face, normals);
        TopLoc_Location loc;
        BRep_Tool::Triangulation(face, loc);
        mesh_tools::applyTransformationOnNormals(loc, normals);

        int zeroNormals = mesh_tools::countZeroNormals(normals);
        if (zeroNormals > 0) hasNormalIssues = true;
    }

    // Check for degenerate triangles
    int degenCount = mesh_tools::countDegenerateTriangles(points, facets);

    // Write positions
    for (size_t i = 0; i < points.size(); ++i) {
        cached.positions.push_back(static_cast<float>(points[i].X()));
        cached.positions.push_back(static_cast<float>(points[i].Y()));
        cached.positions.push_back(static_cast<float>(points[i].Z()));

        if (params.computeNormals && i < normals.size()) {
            cached.normals.push_back(static_cast<float>(normals[i].X()));
            cached.normals.push_back(static_cast<float>(normals[i].Y()));
            cached.normals.push_back(static_cast<float>(normals[i].Z()));
        } else {
            cached.normals.push_back(0.0f);
            cached.normals.push_back(0.0f);
            cached.normals.push_back(1.0f);
        }
    }

    // Write indices (0-based within this face)
    for (auto& tri : facets) {
        Standard_Integer n1, n2, n3;
        tri.Get(n1, n2, n3);
        cached.indices.push_back(static_cast<uint32_t>(n1));
        cached.indices.push_back(static_cast<uint32_t>(n2));
        cached.indices.push_back(static_cast<uint32_t>(n3));
    }

    cached.triangleCount = static_cast<int>(facets.size());

    // Set diagnostic
    cached.diagnostic.triangleCount = cached.triangleCount;
    cached.diagnostic.degenerateCount = degenCount;
    if (degenCount > 0) {
        cached.diagnostic.status = FaceMeshStatus::DegenerateTriangles;
        cached.diagnostic.detail = "Face " + std::to_string(faceIdx) + " has "
                                 + std::to_string(degenCount) + " degenerate triangle(s)";
    } else if (hasNormalIssues) {
        cached.diagnostic.status = FaceMeshStatus::ZeroNormals;
        cached.diagnostic.detail = "Face " + std::to_string(faceIdx) + " has zero-length normals";
    } else {
        cached.diagnostic.status = FaceMeshStatus::OK;
    }

    return cached;
}

// Assemble a MeshResult from cached per-face data.
MeshResult assembleMeshFromFaces(
    KernelContext& ctx,
    const NamedShape& shape,
    const std::map<int, MeshCache::CachedFace>& faces,
    int faceCount,
    const MeshParams& params)
{
    MeshResult result;
    uint32_t globalVertexOffset = 0;

    for (int faceIdx = 1; faceIdx <= faceCount; ++faceIdx) {
        auto it = faces.find(faceIdx);
        if (it == faces.end()) continue;

        const auto& cached = it->second;

        // Record diagnostic
        result.diagnostics.push_back(cached.diagnostic);
        if (cached.diagnostic.status == FaceMeshStatus::NoTriangulation) {
            result.failedFaces++;
            continue;
        }
        result.degenerateTriangles += cached.diagnostic.degenerateCount;

        // Build face group
        FaceGroup group;
        group.faceId = faceIdx;
        group.indexStart = static_cast<uint32_t>(result.indices.size());
        group.r = 0.7f; group.g = 0.7f; group.b = 0.7f; group.a = 1.0f;

        if (shape.elementMap()) {
            auto mapped = shape.elementMap()->getMappedName(IndexedName("Face", faceIdx));
            group.faceName = mapped.data();
        }
        if (group.faceName.empty()) {
            group.faceName = "Face" + std::to_string(faceIdx);
        }

        // Append positions + normals
        result.positions.insert(result.positions.end(),
                                cached.positions.begin(), cached.positions.end());
        result.normals.insert(result.normals.end(),
                              cached.normals.begin(), cached.normals.end());
        if (params.computeNormals && !cached.normals.empty()) {
            result.hasNormals = true;
        }

        // Append indices (offset by global vertex count)
        for (auto idx : cached.indices) {
            result.indices.push_back(globalVertexOffset + idx);
        }

        group.indexCount = static_cast<uint32_t>(result.indices.size()) - group.indexStart;
        result.faceGroups.push_back(group);

        globalVertexOffset += static_cast<uint32_t>(cached.positions.size() / 3);
        result.totalTriangles += cached.triangleCount;
    }

    result.totalVertices = static_cast<int>(result.positions.size() / 3);
    return result;
}

} // anonymous namespace

// ─── Edge extraction helper ──────────────────────────────────────────

namespace {

void extractEdges(const NamedShape& shape,
                  const TopTools_IndexedMapOfShape& faceMap,
                  MeshResult& result)
{
    TopTools_IndexedMapOfShape edgeMap;
    TopExp::MapShapes(shape.shape(), TopAbs_EDGE, edgeMap);

    for (int edgeIdx = 1; edgeIdx <= edgeMap.Extent(); ++edgeIdx) {
        TopoDS_Edge edge = TopoDS::Edge(edgeMap(edgeIdx));

        EdgeSegment seg;
        seg.edgeId = edgeIdx;
        if (shape.elementMap()) {
            auto mapped = shape.elementMap()->getMappedName(IndexedName("Edge", edgeIdx));
            seg.edgeName = mapped.data();
        }
        if (seg.edgeName.empty()) {
            seg.edgeName = "Edge" + std::to_string(edgeIdx);
        }

        bool extracted = false;
        for (int fi = 1; fi <= faceMap.Extent() && !extracted; ++fi) {
            std::vector<gp_Pnt> edgePoints;
            if (mesh_tools::getPolygonOnTriangulation(edge, TopoDS::Face(faceMap(fi)), edgePoints)) {
                for (auto& p : edgePoints) {
                    seg.vertices.push_back(static_cast<float>(p.X()));
                    seg.vertices.push_back(static_cast<float>(p.Y()));
                    seg.vertices.push_back(static_cast<float>(p.Z()));
                }
                extracted = true;
            }
        }

        if (!extracted) {
            std::vector<gp_Pnt> edgePoints;
            if (mesh_tools::getPolygon3D(edge, edgePoints)) {
                for (auto& p : edgePoints) {
                    seg.vertices.push_back(static_cast<float>(p.X()));
                    seg.vertices.push_back(static_cast<float>(p.Y()));
                    seg.vertices.push_back(static_cast<float>(p.Z()));
                }
                extracted = true;
            }
        }

        if (extracted && !seg.vertices.empty()) {
            result.edges.push_back(std::move(seg));
        }
    }
    result.totalEdges = static_cast<int>(result.edges.size());
}

} // anonymous namespace

// ─── Deflection resolution ──────────────────────────────────────────

double resolveLinearDeflection(const NamedShape& shape, const MeshParams& params) {
    if (params.deflectionMode == DeflectionMode::Absolute) {
        return params.linearDeflection;
    }

    // Compute the shape's bounding box diagonal. Used as the reference length
    // for both Relative (user-specified fraction) and Auto (fixed 0.5%) modes.
    double diagonal = 0.0;
    if (!shape.isNull()) {
        Bnd_Box box;
        BRepBndLib::Add(shape.shape(), box);
        if (!box.IsVoid()) {
            double xmin, ymin, zmin, xmax, ymax, zmax;
            box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
            double dx = xmax - xmin, dy = ymax - ymin, dz = zmax - zmin;
            diagonal = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
    }

    // If we couldn't get a usable diagonal, fall back to the absolute value.
    // This also catches degenerate zero-volume shapes.
    if (!(diagonal > 0.0) || !std::isfinite(diagonal)) {
        return params.linearDeflection > 0.0 ? params.linearDeflection : 0.1;
    }

    double fraction = (params.deflectionMode == DeflectionMode::Auto)
        ? 0.005                       // 0.5% — typical CAD viewer default
        : params.linearDeflection;    // Relative: user picks the fraction

    double d = diagonal * fraction;

    // Floor at 0.001 mm. Without this, microscopic shapes (e.g., 0.01 mm
    // bolts) would request sub-micron deflection and the mesher would
    // explode in triangle count. 1 µm is below any reasonable visual
    // resolution and prevents pathological over-tessellation.
    constexpr double kMinDeflection = 0.001;
    if (d < kMinDeflection) d = kMinDeflection;

    return d;
}

// ─── Full tessellation ───────────────────────────────────────────────

OperationResult<MeshResult> tessellate(KernelContext& ctx,
                                       const NamedShape& shape,
                                       const MeshParams& params)
{
    DiagnosticScope scope(ctx);
    MeshResult result;

    if (shape.isNull()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT, "Cannot tessellate null shape");
        return scope.makeFailure<MeshResult>();
    }
    if (params.deflectionMode == DeflectionMode::Absolute && params.linearDeflection <= 0) {
        ctx.diag().error(ErrorCode::INVALID_INPUT, "Linear deflection must be positive");
        return scope.makeFailure<MeshResult>();
    }
    if (params.deflectionMode == DeflectionMode::Relative && params.linearDeflection <= 0) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
            "Relative deflection fraction must be positive");
        return scope.makeFailure<MeshResult>();
    }

    const double effectiveLinear = resolveLinearDeflection(shape, params);

    // ── Step 1: Tessellate ───────────────────────────────────
    double angularRad = params.angularDeflection * M_PI / 180.0;
    BRepMesh_IncrementalMesh mesher(shape.shape(), effectiveLinear,
                                    Standard_False, angularRad,
                                    params.parallel ? Standard_True : Standard_False);
    mesher.Perform();

    if (!mesher.IsDone()) {
        ctx.diag().error(ErrorCode::OCCT_FAILURE, "BRepMesh_IncrementalMesh failed");
        return scope.makeFailure<MeshResult>();
    }

    // ── Step 2: Extract per-face meshes using shared logic ───
    TopTools_IndexedMapOfShape faceMap;
    TopExp::MapShapes(shape.shape(), TopAbs_FACE, faceMap);

    std::map<int, MeshCache::CachedFace> faces;
    for (int faceIdx = 1; faceIdx <= faceMap.Extent(); ++faceIdx) {
        TopoDS_Face face = TopoDS::Face(faceMap(faceIdx));
        faces[faceIdx] = tessellateSingleFace(ctx, face, faceIdx, params);
    }

    // ── Step 3: Assemble result ──────────────────────────────
    result = assembleMeshFromFaces(ctx, shape, faces, faceMap.Extent(), params);

    // ── Step 4: Extract edge wireframe ───────────────────────
    if (params.extractEdges) {
        extractEdges(shape, faceMap, result);
    }

    // Report diagnostics summary (warnings only — not fatal)
    if (result.failedFaces > 0) {
        ctx.diag().warning(ErrorCode::OCCT_FAILURE,
            std::to_string(result.failedFaces) + " face(s) could not be tessellated");
    }
    if (result.degenerateTriangles > 0) {
        ctx.diag().warning(ErrorCode::SHAPE_INVALID,
            std::to_string(result.degenerateTriangles) + " degenerate triangle(s) detected");
    }

    return scope.makeResult(std::move(result));
}

// ─── Incremental tessellation ────────────────────────────────────────

OperationResult<MeshResult> tessellateIncremental(KernelContext& ctx,
                                                  const NamedShape& shape,
                                                  MeshCache& cache,
                                                  const MeshParams& params)
{
    DiagnosticScope scope(ctx);
    MeshResult result;

    if (shape.isNull()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT, "Cannot tessellate null shape");
        return scope.makeFailure<MeshResult>();
    }
    if (params.deflectionMode == DeflectionMode::Absolute && params.linearDeflection <= 0) {
        ctx.diag().error(ErrorCode::INVALID_INPUT, "Linear deflection must be positive");
        return scope.makeFailure<MeshResult>();
    }
    if (params.deflectionMode == DeflectionMode::Relative && params.linearDeflection <= 0) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
            "Relative deflection fraction must be positive");
        return scope.makeFailure<MeshResult>();
    }

    // If params changed, invalidate entire cache
    if (!cache.paramsMatch(params)) {
        cache.invalidateAll();
        cache.setCachedParams(params);
    }

    const double effectiveLinear = resolveLinearDeflection(shape, params);

    // Tessellate the shape (OCCT's BRepMesh handles incremental internally)
    double angularRad = params.angularDeflection * M_PI / 180.0;
    BRepMesh_IncrementalMesh mesher(shape.shape(), effectiveLinear,
                                    Standard_False, angularRad,
                                    params.parallel ? Standard_True : Standard_False);
    mesher.Perform();

    if (!mesher.IsDone()) {
        ctx.diag().error(ErrorCode::OCCT_FAILURE, "BRepMesh_IncrementalMesh failed");
        return scope.makeFailure<MeshResult>();
    }

    // Extract per-face meshes, using cache where available
    TopTools_IndexedMapOfShape faceMap;
    TopExp::MapShapes(shape.shape(), TopAbs_FACE, faceMap);

    std::map<int, MeshCache::CachedFace> faces;
    int cacheHits = 0;
    int cacheMisses = 0;

    for (int faceIdx = 1; faceIdx <= faceMap.Extent(); ++faceIdx) {
        TopoDS_Face face = TopoDS::Face(faceMap(faceIdx));
        uint64_t fp = computeFaceFingerprint(face);

        if (cache.isCacheValid(faceIdx, fp)) {
            // Use cached data — geometry fingerprint matches
            faces[faceIdx] = *cache.getCachedFace(faceIdx);
            ++cacheHits;
        } else {
            // Tessellate this face and cache the result
            auto cachedFace = tessellateSingleFace(ctx, face, faceIdx, params);
            cachedFace.geometryFingerprint = fp;
            faces[faceIdx] = cachedFace;
            cache.cacheFace(faceIdx, std::move(cachedFace));
            ++cacheMisses;
        }
    }

    // Assemble result from all faces
    result = assembleMeshFromFaces(ctx, shape, faces, faceMap.Extent(), params);

    // Extract edges
    if (params.extractEdges) {
        extractEdges(shape, faceMap, result);
    }

    // Report diagnostics (warnings only — not fatal)
    if (result.failedFaces > 0) {
        ctx.diag().warning(ErrorCode::OCCT_FAILURE,
            std::to_string(result.failedFaces) + " face(s) could not be tessellated");
    }
    if (result.degenerateTriangles > 0) {
        ctx.diag().warning(ErrorCode::SHAPE_INVALID,
            std::to_string(result.degenerateTriangles) + " degenerate triangle(s) detected");
    }
    if (cacheHits > 0) {
        ctx.diag().info(ErrorCode::OK,
            "Incremental mesh: " + std::to_string(cacheHits) + " cached, "
            + std::to_string(cacheMisses) + " retessellated");
    }

    return scope.makeResult(std::move(result));
}

// ─── Selection helper ────────────────────────────────────────────────

int MeshResult::faceIdForTriangle(uint32_t triangleIndex) const {
    // Each triangle occupies 3 consecutive entries in `indices`. Face groups
    // are appended in ascending face-ID order and their [indexStart,
    // indexStart+indexCount) ranges partition the index array without gaps,
    // so a std::upper_bound on indexStart locates the owning group.
    const uint32_t idxPos = triangleIndex * 3u;
    if (idxPos >= indices.size()) return -1;
    if (faceGroups.empty()) return -1;

    // Find the first group whose indexStart is strictly greater than idxPos.
    // The owning group is the one immediately before it.
    auto it = std::upper_bound(
        faceGroups.begin(), faceGroups.end(), idxPos,
        [](uint32_t pos, const FaceGroup& g) { return pos < g.indexStart; });
    if (it == faceGroups.begin()) return -1; // idxPos before first group

    --it;
    if (idxPos < it->indexStart + it->indexCount) {
        return it->faceId;
    }
    return -1; // idxPos fell into a gap (shouldn't happen with valid meshes)
}

} // namespace oreo
