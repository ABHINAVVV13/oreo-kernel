// oreo_mesh_export.h — GLB (binary glTF 2.0) export for MeshResult.
//
// Produces a standards-compliant .glb blob that loads in three.js, Babylon.js,
// Blender, Windows 3D Viewer, etc. Face groups map 1:1 to glTF primitives so
// per-face colour and per-face selection metadata survive the export. The
// stable CAD face ID is embedded in each primitive's `extras.faceId` field
// (GLTF standard extension mechanism) so custom viewers can round-trip it.
//
// One buffer, one bufferView per logical attribute (POSITION, NORMAL, indices,
// edge positions). Per-face primitives reference sub-ranges of the shared
// indices bufferView via per-accessor byteOffset — no data duplication.

#ifndef OREO_MESH_EXPORT_H
#define OREO_MESH_EXPORT_H

#include "core/kernel_context.h"
#include "core/operation_result.h"
#include "mesh/oreo_mesh.h"

#include <cstdint>
#include <string>
#include <vector>

namespace oreo {

struct GltfExportParams {
    // Emit each FaceGroup as a separate primitive (preserves per-face colour
    // and faceId metadata). If false, a single primitive covers the whole
    // mesh — smaller file, no per-face selection. Default: true.
    bool perFacePrimitives = true;

    // Emit the edge wireframe as a second mesh with GLTF mode=LINES. Useful
    // for CAD-style rendering where feature lines must overlay the solid.
    // Default: true when MeshResult has edges, ignored otherwise.
    bool includeEdges = true;

    // Root node name. Shows up in the scene tree of viewers. Default: "oreo".
    std::string name = "oreo";

    // generator string written to asset.generator. Appended to the kernel
    // version so downstream tools can tell which build produced the file.
    std::string generator = "oreo-kernel";
};

// Serialize a MeshResult into a .glb byte buffer (GLB = binary glTF 2.0).
// Returns OperationResult<std::vector<uint8_t>>. On failure the diagnostic
// describes what went wrong (empty mesh, index out of range, etc.).
//
// Typical usage:
//   auto mesh = oreo::tessellate(*ctx, shape).value();
//   auto glbR = oreo::exportGLB(*ctx, mesh);
//   if (glbR.ok()) {
//       std::ofstream out("model.glb", std::ios::binary);
//       auto bytes = glbR.value();
//       out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
//   }
OperationResult<std::vector<uint8_t>> exportGLB(
    KernelContext& ctx,
    const MeshResult& mesh,
    const GltfExportParams& params = {});

} // namespace oreo

#endif // OREO_MESH_EXPORT_H
