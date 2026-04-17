// oreo_mesh_export.cpp — GLB (binary glTF 2.0) writer.
//
// Layout of the output file:
//   [12 B header]  magic "glTF" | version 2 | total length
//   [JSON chunk]   chunk header + UTF-8 JSON, padded to 4 B with spaces
//   [BIN chunk]    chunk header + raw vertex/index data, padded to 4 B with 0x00
//
// The BIN chunk packs (in order): positions, normals, triangle indices,
// optional edge positions. Each logical region is exposed via a bufferView.
// Per-face primitives reference subranges of the shared indices bufferView
// via per-accessor byteOffset — no data duplication.

#include "oreo_mesh_export.h"
#include "core/diagnostic_scope.h"
#include "core/oreo_error.h"

#include <nlohmann/json.hpp>

#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace oreo {

namespace {

// GLB constants (glTF 2.0 spec, §3.1).
constexpr uint32_t kGlbMagic        = 0x46546C67; // "glTF"
constexpr uint32_t kGlbVersion      = 2;
constexpr uint32_t kChunkTypeJson   = 0x4E4F534A; // "JSON"
constexpr uint32_t kChunkTypeBin    = 0x004E4942; // "BIN\0"

// glTF accessor componentTypes.
constexpr int kCompFloat            = 5126;
constexpr int kCompUInt32           = 5125;

// glTF bufferView targets.
constexpr int kTargetArrayBuffer    = 34962; // POSITION, NORMAL
constexpr int kTargetElementArray   = 34963; // indices

// glTF primitive modes.
constexpr int kPrimitiveTriangles   = 4;
constexpr int kPrimitiveLineStrip   = 3;

// Append raw bytes from a source buffer to a destination byte vector.
template <typename T>
void appendBytes(std::vector<uint8_t>& dst, const T* src, size_t count) {
    const auto* p = reinterpret_cast<const uint8_t*>(src);
    dst.insert(dst.end(), p, p + count * sizeof(T));
}

// Pad `buf` with `padByte` until its length is a multiple of 4. Required by
// the GLB spec for both the chunk contents and chunk boundaries.
void padTo4(std::vector<uint8_t>& buf, uint8_t padByte) {
    while (buf.size() % 4 != 0) buf.push_back(padByte);
}

void writeU32LE(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>( v        & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >>  8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

// Compute per-component min/max of the positions array. The glTF 2.0 spec
// (§5.1.3) REQUIRES min/max on any accessor that backs a POSITION attribute,
// so validators reject files that omit this.
struct Vec3MinMax {
    float min[3] = { std::numeric_limits<float>::infinity(),
                     std::numeric_limits<float>::infinity(),
                     std::numeric_limits<float>::infinity() };
    float max[3] = { -std::numeric_limits<float>::infinity(),
                     -std::numeric_limits<float>::infinity(),
                     -std::numeric_limits<float>::infinity() };
};

Vec3MinMax computeVec3MinMax(const std::vector<float>& v) {
    Vec3MinMax r;
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        for (int k = 0; k < 3; ++k) {
            float c = v[i + k];
            if (c < r.min[k]) r.min[k] = c;
            if (c > r.max[k]) r.max[k] = c;
        }
    }
    // Empty input: leave min=+inf, max=-inf which the caller must handle.
    return r;
}

} // anonymous namespace

OperationResult<std::vector<uint8_t>> exportGLB(
    KernelContext& ctx,
    const MeshResult& mesh,
    const GltfExportParams& params)
{
    DiagnosticScope scope(ctx);

    if (mesh.empty()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT, "Cannot export empty mesh to GLB");
        return scope.makeFailure<std::vector<uint8_t>>();
    }
    if (mesh.positions.size() % 3 != 0 || mesh.normals.size() % 3 != 0) {
        ctx.diag().error(ErrorCode::SHAPE_INVALID,
            "Mesh positions/normals are not aligned to 3 floats per vertex");
        return scope.makeFailure<std::vector<uint8_t>>();
    }
    if (mesh.indices.size() % 3 != 0) {
        ctx.diag().error(ErrorCode::SHAPE_INVALID,
            "Mesh indices are not aligned to 3 indices per triangle");
        return scope.makeFailure<std::vector<uint8_t>>();
    }

    // ── BIN chunk — pack raw geometry ────────────────────────────
    //
    // Layout:
    //   [0] positions   (totalVertices × 3 × float)
    //   [1] normals     (totalVertices × 3 × float) — only if hasNormals
    //   [2] indices     (totalIndices × uint32)
    //   [3] edge positions (if any edges + includeEdges)
    //
    // Each region starts 4-byte aligned because every element is float32
    // or uint32. Between regions we re-pad defensively with zeros.
    std::vector<uint8_t> bin;
    bin.reserve(mesh.positions.size() * 4
              + mesh.normals.size() * 4
              + mesh.indices.size() * 4);

    const uint32_t positionsByteOffset = static_cast<uint32_t>(bin.size());
    appendBytes(bin, mesh.positions.data(), mesh.positions.size());
    const uint32_t positionsByteLength = static_cast<uint32_t>(bin.size() - positionsByteOffset);
    padTo4(bin, 0);

    const bool emitNormals = mesh.hasNormals && mesh.normals.size() == mesh.positions.size();
    uint32_t normalsByteOffset = 0;
    uint32_t normalsByteLength = 0;
    if (emitNormals) {
        normalsByteOffset = static_cast<uint32_t>(bin.size());
        appendBytes(bin, mesh.normals.data(), mesh.normals.size());
        normalsByteLength = static_cast<uint32_t>(bin.size() - normalsByteOffset);
        padTo4(bin, 0);
    }

    const uint32_t indicesByteOffset = static_cast<uint32_t>(bin.size());
    appendBytes(bin, mesh.indices.data(), mesh.indices.size());
    const uint32_t indicesByteLength = static_cast<uint32_t>(bin.size() - indicesByteOffset);
    padTo4(bin, 0);

    // Edge wireframe — pack per-segment positions contiguously and remember
    // each segment's byte range so each can become its own LINE_STRIP primitive.
    const bool emitEdges = params.includeEdges && !mesh.edges.empty();
    struct EdgeSlot { uint32_t byteOffset; uint32_t vertexCount; int edgeId; std::string name; };
    std::vector<EdgeSlot> edgeSlots;
    uint32_t edgePosByteOffset = 0;
    uint32_t edgePosByteLength = 0;
    if (emitEdges) {
        edgeSlots.reserve(mesh.edges.size());
        edgePosByteOffset = static_cast<uint32_t>(bin.size());
        for (const auto& seg : mesh.edges) {
            if (seg.vertices.size() < 6 || seg.vertices.size() % 3 != 0) continue;
            EdgeSlot s;
            s.byteOffset = static_cast<uint32_t>(bin.size() - edgePosByteOffset);
            s.vertexCount = static_cast<uint32_t>(seg.vertices.size() / 3);
            s.edgeId = seg.edgeId;
            s.name = seg.edgeName;
            appendBytes(bin, seg.vertices.data(), seg.vertices.size());
            edgeSlots.push_back(std::move(s));
        }
        edgePosByteLength = static_cast<uint32_t>(bin.size() - edgePosByteOffset);
        padTo4(bin, 0);
    }

    // ── Build glTF JSON document ─────────────────────────────────
    using nlohmann::json;
    json gltf;

    gltf["asset"] = {
        {"version",   "2.0"},
        {"generator", params.generator},
    };

    // Buffer — single BIN buffer, embedded in chunk 1 of the GLB (no URI).
    gltf["buffers"] = json::array({
        json{ {"byteLength", bin.size()} }
    });

    // BufferViews. Numbered in the order we appended them.
    json bufferViews = json::array();

    int bvPositions = static_cast<int>(bufferViews.size());
    bufferViews.push_back(json{
        {"buffer",     0},
        {"byteOffset", positionsByteOffset},
        {"byteLength", positionsByteLength},
        {"target",     kTargetArrayBuffer},
    });

    int bvNormals = -1;
    if (emitNormals) {
        bvNormals = static_cast<int>(bufferViews.size());
        bufferViews.push_back(json{
            {"buffer",     0},
            {"byteOffset", normalsByteOffset},
            {"byteLength", normalsByteLength},
            {"target",     kTargetArrayBuffer},
        });
    }

    int bvIndices = static_cast<int>(bufferViews.size());
    bufferViews.push_back(json{
        {"buffer",     0},
        {"byteOffset", indicesByteOffset},
        {"byteLength", indicesByteLength},
        {"target",     kTargetElementArray},
    });

    int bvEdges = -1;
    if (emitEdges && edgePosByteLength > 0) {
        bvEdges = static_cast<int>(bufferViews.size());
        bufferViews.push_back(json{
            {"buffer",     0},
            {"byteOffset", edgePosByteOffset},
            {"byteLength", edgePosByteLength},
            {"target",     kTargetArrayBuffer},
        });
    }
    gltf["bufferViews"] = std::move(bufferViews);

    // Accessors.
    json accessors = json::array();

    const uint32_t vertexCount = static_cast<uint32_t>(mesh.positions.size() / 3);
    Vec3MinMax pmm = computeVec3MinMax(mesh.positions);
    int accPosition = static_cast<int>(accessors.size());
    accessors.push_back(json{
        {"bufferView",    bvPositions},
        {"componentType", kCompFloat},
        {"count",         vertexCount},
        {"type",          "VEC3"},
        {"min",           json::array({pmm.min[0], pmm.min[1], pmm.min[2]})},
        {"max",           json::array({pmm.max[0], pmm.max[1], pmm.max[2]})},
    });

    int accNormal = -1;
    if (emitNormals) {
        accNormal = static_cast<int>(accessors.size());
        accessors.push_back(json{
            {"bufferView",    bvNormals},
            {"componentType", kCompFloat},
            {"count",         vertexCount},
            {"type",          "VEC3"},
        });
    }

    // Index accessors. If perFacePrimitives is on, one per face group; else
    // one covering the entire index range.
    struct PrimitiveSpec {
        int accIndices;
        int materialIndex; // -1 if no material
        int faceId;        // -1 if no face metadata
        std::string name;
    };
    std::vector<PrimitiveSpec> triPrims;

    if (params.perFacePrimitives && !mesh.faceGroups.empty()) {
        triPrims.reserve(mesh.faceGroups.size());
        for (const auto& g : mesh.faceGroups) {
            if (g.indexCount == 0) continue;
            int accIdx = static_cast<int>(accessors.size());
            accessors.push_back(json{
                {"bufferView",    bvIndices},
                {"byteOffset",    g.indexStart * 4u},      // uint32 = 4 bytes
                {"componentType", kCompUInt32},
                {"count",         g.indexCount},
                {"type",          "SCALAR"},
            });
            PrimitiveSpec p;
            p.accIndices    = accIdx;
            p.materialIndex = static_cast<int>(triPrims.size());
            p.faceId        = g.faceId;
            p.name          = g.faceName;
            triPrims.push_back(std::move(p));
        }
    } else {
        int accIdx = static_cast<int>(accessors.size());
        accessors.push_back(json{
            {"bufferView",    bvIndices},
            {"componentType", kCompUInt32},
            {"count",         mesh.indices.size()},
            {"type",          "SCALAR"},
        });
        PrimitiveSpec p;
        p.accIndices    = accIdx;
        p.materialIndex = -1;
        p.faceId        = -1;
        p.name          = "";
        triPrims.push_back(std::move(p));
    }

    // Edge accessors (one per kept segment) + primitives.
    struct EdgePrimSpec { int accPosition; int faceRangeCount; int edgeId; std::string name; };
    std::vector<EdgePrimSpec> edgePrims;
    if (emitEdges && bvEdges >= 0) {
        edgePrims.reserve(edgeSlots.size());
        for (const auto& s : edgeSlots) {
            int accIdx = static_cast<int>(accessors.size());
            accessors.push_back(json{
                {"bufferView",    bvEdges},
                {"byteOffset",    s.byteOffset},
                {"componentType", kCompFloat},
                {"count",         s.vertexCount},
                {"type",          "VEC3"},
            });
            edgePrims.push_back({accIdx, static_cast<int>(s.vertexCount), s.edgeId, s.name});
        }
    }
    gltf["accessors"] = std::move(accessors);

    // Materials — one per triangle primitive when perFacePrimitives is on.
    if (params.perFacePrimitives && !mesh.faceGroups.empty()) {
        json materials = json::array();
        for (const auto& g : mesh.faceGroups) {
            if (g.indexCount == 0) continue;
            json m;
            m["pbrMetallicRoughness"] = {
                {"baseColorFactor", json::array({g.r, g.g, g.b, g.a})},
                {"metallicFactor",  0.1},
                {"roughnessFactor", 0.75},
            };
            if (!g.faceName.empty()) m["name"] = g.faceName;
            // A two-sided BRep face must render from both sides for selection
            // to light up correctly in viewers that backface-cull.
            m["doubleSided"] = true;
            materials.push_back(std::move(m));
        }
        if (!materials.empty()) gltf["materials"] = std::move(materials);
    }

    // Triangle mesh.
    json triPrimArray = json::array();
    for (const auto& p : triPrims) {
        json prim;
        json attrs;
        attrs["POSITION"] = accPosition;
        if (accNormal >= 0) attrs["NORMAL"] = accNormal;
        prim["attributes"] = std::move(attrs);
        prim["indices"]    = p.accIndices;
        prim["mode"]       = kPrimitiveTriangles;
        if (p.materialIndex >= 0) prim["material"] = p.materialIndex;
        if (p.faceId >= 0) {
            json extras;
            extras["faceId"] = p.faceId;
            if (!p.name.empty()) extras["faceName"] = p.name;
            prim["extras"] = std::move(extras);
        }
        triPrimArray.push_back(std::move(prim));
    }

    json meshes = json::array();
    json solidMesh;
    solidMesh["primitives"] = std::move(triPrimArray);
    solidMesh["name"] = params.name + "-solid";
    meshes.push_back(std::move(solidMesh));

    // Edge mesh (separate so viewers can toggle it independently).
    int edgeMeshIndex = -1;
    if (!edgePrims.empty()) {
        json edgePrimArray = json::array();
        for (const auto& p : edgePrims) {
            json prim;
            prim["attributes"] = json{{"POSITION", p.accPosition}};
            prim["mode"]       = kPrimitiveLineStrip;
            json extras;
            extras["edgeId"] = p.edgeId;
            if (!p.name.empty()) extras["edgeName"] = p.name;
            prim["extras"] = std::move(extras);
            edgePrimArray.push_back(std::move(prim));
        }
        json edgeMesh;
        edgeMesh["primitives"] = std::move(edgePrimArray);
        edgeMesh["name"] = params.name + "-edges";
        edgeMeshIndex = static_cast<int>(meshes.size());
        meshes.push_back(std::move(edgeMesh));
    }
    gltf["meshes"] = std::move(meshes);

    // Nodes + scene.
    json nodes = json::array();
    nodes.push_back(json{{"mesh", 0}, {"name", params.name}});
    if (edgeMeshIndex >= 0) {
        nodes.push_back(json{{"mesh", edgeMeshIndex}, {"name", params.name + "-edges"}});
    }
    gltf["nodes"] = std::move(nodes);
    gltf["scene"] = 0;
    gltf["scenes"] = json::array({
        json{{"nodes", edgeMeshIndex >= 0 ? json::array({0, 1}) : json::array({0})}}
    });

    // ── Assemble GLB container ───────────────────────────────────
    std::string jsonText = gltf.dump();
    std::vector<uint8_t> jsonChunk(jsonText.begin(), jsonText.end());
    padTo4(jsonChunk, 0x20 /* ASCII space — GLB spec requires space padding */);

    // Total size = 12 (header) + 8 + jsonChunk.size() (JSON chunk hdr + data)
    //            + 8 + bin.size() (BIN chunk hdr + data)
    const uint32_t totalSize = static_cast<uint32_t>(
        12 + 8 + jsonChunk.size() + 8 + bin.size());

    std::vector<uint8_t> out;
    out.reserve(totalSize);

    // GLB header
    writeU32LE(out, kGlbMagic);
    writeU32LE(out, kGlbVersion);
    writeU32LE(out, totalSize);

    // JSON chunk
    writeU32LE(out, static_cast<uint32_t>(jsonChunk.size()));
    writeU32LE(out, kChunkTypeJson);
    out.insert(out.end(), jsonChunk.begin(), jsonChunk.end());

    // BIN chunk
    writeU32LE(out, static_cast<uint32_t>(bin.size()));
    writeU32LE(out, kChunkTypeBin);
    out.insert(out.end(), bin.begin(), bin.end());

    return scope.makeResult(std::move(out));
}

} // namespace oreo
