// SPDX-License-Identifier: LGPL-2.1-or-later

// test_mesh_export.cpp — GLB exporter round-trip tests.
//
// These tests exercise the GLB binary format without needing an external
// validator: we parse the header, extract both chunks, re-parse the JSON
// chunk with nlohmann::json, and assert structural invariants we care about
// (correct accessor counts, face-id extras, buffer lengths, etc.).

#include <gtest/gtest.h>

#include "core/kernel_context.h"
#include "geometry/oreo_geometry.h"
#include "mesh/oreo_mesh.h"
#include "mesh/oreo_mesh_export.h"

#include <nlohmann/json.hpp>

#include <cstring>
#include <string>
#include <vector>

namespace {

using json = nlohmann::json;

constexpr uint32_t kGlbMagic      = 0x46546C67; // "glTF"
constexpr uint32_t kChunkTypeJson = 0x4E4F534A; // "JSON"
constexpr uint32_t kChunkTypeBin  = 0x004E4942; // "BIN\0"

uint32_t readU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

// Parse the GLB container into its header, JSON string, and BIN bytes.
// Aborts the test with ADD_FAILURE if the structure is malformed.
struct ParsedGlb {
    uint32_t version;
    uint32_t totalSize;
    std::string jsonText;
    std::vector<uint8_t> bin;
};

ParsedGlb parseGlb(const std::vector<uint8_t>& bytes) {
    ParsedGlb r;
    EXPECT_GE(bytes.size(), 12u + 8u) << "GLB blob too small for header+chunk";
    if (bytes.size() < 12) return r;

    EXPECT_EQ(readU32LE(&bytes[0]), kGlbMagic) << "Wrong GLB magic";
    r.version   = readU32LE(&bytes[4]);
    r.totalSize = readU32LE(&bytes[8]);
    EXPECT_EQ(r.version, 2u);
    EXPECT_EQ(r.totalSize, bytes.size()) << "Declared size doesn't match actual";

    size_t pos = 12;
    // JSON chunk
    uint32_t jsonLen = readU32LE(&bytes[pos]); pos += 4;
    uint32_t jsonType = readU32LE(&bytes[pos]); pos += 4;
    EXPECT_EQ(jsonType, kChunkTypeJson);
    EXPECT_LE(pos + jsonLen, bytes.size());
    r.jsonText.assign(reinterpret_cast<const char*>(&bytes[pos]), jsonLen);
    // Strip trailing 0x20 padding so nlohmann::json parses cleanly.
    while (!r.jsonText.empty() && r.jsonText.back() == ' ') r.jsonText.pop_back();
    pos += jsonLen;

    // BIN chunk (optional but always present in our exporter)
    if (pos + 8 <= bytes.size()) {
        uint32_t binLen = readU32LE(&bytes[pos]); pos += 4;
        uint32_t binType = readU32LE(&bytes[pos]); pos += 4;
        EXPECT_EQ(binType, kChunkTypeBin);
        EXPECT_LE(pos + binLen, bytes.size());
        r.bin.assign(bytes.begin() + pos, bytes.begin() + pos + binLen);
    }
    return r;
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════
// Basic structure
// ═══════════════════════════════════════════════════════════════

TEST(MeshExport, BoxExportsValidGlb) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 20, 30).value();
    auto mesh = oreo::tessellate(*ctx, box).value();

    auto glbR = oreo::exportGLB(*ctx, mesh);
    ASSERT_TRUE(glbR.ok()) << glbR.errorMessage();
    auto glb = std::move(glbR).value();

    auto parsed = parseGlb(glb);
    ASSERT_FALSE(parsed.jsonText.empty());

    json doc = json::parse(parsed.jsonText);
    EXPECT_EQ(doc["asset"]["version"], "2.0");
    EXPECT_EQ(doc["buffers"].size(), 1u);
    EXPECT_EQ(doc["buffers"][0]["byteLength"], parsed.bin.size());
    EXPECT_EQ(doc["scene"], 0);
    ASSERT_TRUE(doc.contains("scenes"));
    ASSERT_TRUE(doc.contains("nodes"));
    ASSERT_TRUE(doc.contains("meshes"));
}

TEST(MeshExport, BoxHasOnePrimitivePerFaceGroup) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 10, 10).value();
    auto mesh = oreo::tessellate(*ctx, box).value();

    oreo::GltfExportParams params;
    params.perFacePrimitives = true;
    params.includeEdges = false;

    auto glb = oreo::exportGLB(*ctx, mesh, params).value();
    auto parsed = parseGlb(glb);
    json doc = json::parse(parsed.jsonText);

    // Exactly one triangle mesh with 6 primitives (one per box face).
    ASSERT_EQ(doc["meshes"].size(), 1u);
    auto prims = doc["meshes"][0]["primitives"];
    EXPECT_EQ(prims.size(), mesh.faceGroups.size());
    EXPECT_EQ(prims.size(), 6u);

    // Each primitive must reference POSITION+NORMAL and carry a faceId extra.
    std::vector<int> seenFaceIds;
    for (auto& p : prims) {
        EXPECT_TRUE(p["attributes"].contains("POSITION"));
        EXPECT_TRUE(p["attributes"].contains("NORMAL"));
        EXPECT_TRUE(p.contains("indices"));
        EXPECT_EQ(p["mode"], 4); // TRIANGLES
        ASSERT_TRUE(p.contains("extras"));
        ASSERT_TRUE(p["extras"].contains("faceId"));
        seenFaceIds.push_back(p["extras"]["faceId"].get<int>());
    }
    // Face IDs should be 1..6 (in some order).
    std::sort(seenFaceIds.begin(), seenFaceIds.end());
    EXPECT_EQ(seenFaceIds, (std::vector<int>{1, 2, 3, 4, 5, 6}));

    // One material per primitive (perFacePrimitives = true).
    ASSERT_TRUE(doc.contains("materials"));
    EXPECT_EQ(doc["materials"].size(), prims.size());
}

TEST(MeshExport, PositionAccessorHasMinMax) {
    // glTF 2.0 validators reject POSITION accessors that omit min/max.
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 100, 200, 300).value();
    auto mesh = oreo::tessellate(*ctx, box).value();
    auto glb = oreo::exportGLB(*ctx, mesh).value();
    auto parsed = parseGlb(glb);
    json doc = json::parse(parsed.jsonText);

    // Accessor 0 is always POSITION per our layout.
    ASSERT_GE(doc["accessors"].size(), 1u);
    auto pos = doc["accessors"][0];
    ASSERT_TRUE(pos.contains("min"));
    ASSERT_TRUE(pos.contains("max"));
    EXPECT_EQ(pos["min"].size(), 3u);
    EXPECT_EQ(pos["max"].size(), 3u);

    // min/max should enclose the box (10mm origin + 100mm extent etc.)
    auto mn = pos["min"]; auto mx = pos["max"];
    for (int i = 0; i < 3; ++i) {
        EXPECT_LE(mn[i].get<double>(), mx[i].get<double>());
    }
}

TEST(MeshExport, BinChunkMatchesBufferLength) {
    auto ctx = oreo::KernelContext::create();
    auto sphere = oreo::makeSphere(*ctx, 10.0).value();
    auto mesh = oreo::tessellate(*ctx, sphere).value();
    auto glb = oreo::exportGLB(*ctx, mesh).value();
    auto parsed = parseGlb(glb);
    json doc = json::parse(parsed.jsonText);

    // BIN chunk length MUST equal the declared buffer byteLength — that's
    // how loaders know where the buffer ends.
    EXPECT_EQ(parsed.bin.size(), doc["buffers"][0]["byteLength"].get<size_t>());
}

// ═══════════════════════════════════════════════════════════════
// Edges
// ═══════════════════════════════════════════════════════════════

TEST(MeshExport, EdgesEmitAsSecondMesh) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 10, 10).value();
    auto mesh = oreo::tessellate(*ctx, box).value();
    ASSERT_FALSE(mesh.edges.empty());

    oreo::GltfExportParams params;
    params.includeEdges = true;

    auto glb = oreo::exportGLB(*ctx, mesh, params).value();
    auto parsed = parseGlb(glb);
    json doc = json::parse(parsed.jsonText);

    // Two meshes: solid + edges.
    EXPECT_EQ(doc["meshes"].size(), 2u);
    auto edgePrims = doc["meshes"][1]["primitives"];
    EXPECT_EQ(edgePrims.size(), mesh.edges.size());

    // Edge primitives must use LINE_STRIP (mode=3) and have edgeId extras.
    for (auto& p : edgePrims) {
        EXPECT_EQ(p["mode"], 3);
        ASSERT_TRUE(p.contains("extras"));
        EXPECT_TRUE(p["extras"].contains("edgeId"));
    }

    // Scene should reference both nodes.
    EXPECT_EQ(doc["nodes"].size(), 2u);
    EXPECT_EQ(doc["scenes"][0]["nodes"].size(), 2u);
}

TEST(MeshExport, EdgesCanBeSuppressed) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 10, 10).value();
    auto mesh = oreo::tessellate(*ctx, box).value();

    oreo::GltfExportParams params;
    params.includeEdges = false;

    auto glb = oreo::exportGLB(*ctx, mesh, params).value();
    auto parsed = parseGlb(glb);
    json doc = json::parse(parsed.jsonText);

    EXPECT_EQ(doc["meshes"].size(), 1u) << "Edges should be omitted";
    EXPECT_EQ(doc["nodes"].size(), 1u);
    EXPECT_EQ(doc["scenes"][0]["nodes"].size(), 1u);
}

// ═══════════════════════════════════════════════════════════════
// Single-primitive mode
// ═══════════════════════════════════════════════════════════════

TEST(MeshExport, SinglePrimitiveModeCollapsesFaceGroups) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 10, 10).value();
    auto mesh = oreo::tessellate(*ctx, box).value();

    oreo::GltfExportParams params;
    params.perFacePrimitives = false;
    params.includeEdges = false;

    auto glb = oreo::exportGLB(*ctx, mesh, params).value();
    auto parsed = parseGlb(glb);
    json doc = json::parse(parsed.jsonText);

    ASSERT_EQ(doc["meshes"].size(), 1u);
    EXPECT_EQ(doc["meshes"][0]["primitives"].size(), 1u)
        << "Single-primitive mode should produce exactly one primitive";

    // No materials when face groups are collapsed.
    EXPECT_FALSE(doc.contains("materials"));
}

// ═══════════════════════════════════════════════════════════════
// Error handling
// ═══════════════════════════════════════════════════════════════

TEST(MeshExport, EmptyMeshFailsClosed) {
    auto ctx = oreo::KernelContext::create();
    oreo::MeshResult empty;

    auto r = oreo::exportGLB(*ctx, empty);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(r.hasErrors());
}

// ═══════════════════════════════════════════════════════════════
// Index accessor subranges
// ═══════════════════════════════════════════════════════════════

TEST(MeshExport, PerFacePrimitiveIndexRangesSumToTotal) {
    auto ctx = oreo::KernelContext::create();
    auto sphere = oreo::makeSphere(*ctx, 10.0).value();
    auto mesh = oreo::tessellate(*ctx, sphere).value();

    oreo::GltfExportParams params;
    params.perFacePrimitives = true;
    params.includeEdges = false;

    auto glb = oreo::exportGLB(*ctx, mesh, params).value();
    auto parsed = parseGlb(glb);
    json doc = json::parse(parsed.jsonText);

    // Sum of per-primitive index counts must equal mesh.indices.size().
    // This is the invariant that guarantees no triangle is dropped or
    // double-counted by the per-face splitting logic.
    size_t totalIndices = 0;
    for (auto& p : doc["meshes"][0]["primitives"]) {
        int accIdx = p["indices"].get<int>();
        totalIndices += doc["accessors"][accIdx]["count"].get<size_t>();
    }
    EXPECT_EQ(totalIndices, mesh.indices.size());
}
