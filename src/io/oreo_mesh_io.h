// SPDX-License-Identifier: LGPL-2.1-or-later

// oreo_mesh_io.h — Mesh-based interchange I/O (STL + 3MF).
//
// These formats carry triangle meshes, not parametric BRep, so every
// import has to decide whether to stitch triangles back into a Shell
// (optional) or keep them as a raw tessellation. Every export has to
// tessellate first. STL is the universal baseline for 3D printing
// workflows; 3MF is the modern replacement with proper units, colours
// and multi-object support.
//
// STL import/export ride on OCCT's StlAPI. 3MF export is hand-rolled
// (store-only ZIP + minimal 3MF Core model XML) because OCCT 7.9 ships
// no native 3MF module and pulling lib3mf in would drag libzip + boost
// filesystem v3 into the dep tree. 3MF import is out of scope for 1.0 —
// a proper import needs a real ZIP + XML parser pair, so we flag it
// NOT_SUPPORTED at the C boundary until a reader lands.
//
// Every operation takes a KernelContext& as its first parameter and
// returns an OperationResult<T> with attached diagnostics.

#ifndef OREO_MESH_IO_H
#define OREO_MESH_IO_H

#include "core/kernel_context.h"
#include "core/operation_result.h"
#include "naming/named_shape.h"

#include <cstdint>
#include <string>
#include <vector>

namespace oreo {

// ═══════════════════════════════════════════════════════════════
// STL
// ═══════════════════════════════════════════════════════════════

// STL writer mode. Binary is the default (smaller files, faster write);
// ASCII is only worth picking when diffing or hand-editing output.
enum class StlFormat { Binary, Ascii };

struct StlExportOptions {
    StlFormat format = StlFormat::Binary;
    // Tessellation parameters match the kernel's OreoMesh pipeline so
    // two callers who agree on these values get byte-identical output.
    double linearDeflection  = 0.1;   // mm
    double angularDeflection = 20.0;  // degrees
    // If true, the exporter runs a ShapeFix pass on the input before
    // tessellating; worth enabling on shapes that came out of a boolean
    // retry path where self-intersecting micro-faces can otherwise emit
    // degenerate triangles. Defaults to false (fast path).
    bool   healBeforeTessellate = false;
};

// Import an STL file as a raw triangulated solid. The returned
// NamedShape wraps a TopoDS_Shell built from the triangles (one face
// per triangle). Element names follow the "imported triangle" scheme
// — this is a mesh, not a parametric body; downstream Fillet / Draft
// etc. will almost certainly fail. Use STL only when you need to
// ingest mesh data for visualization or further meshing.
//
// Both binary and ASCII STL are auto-detected by the OCCT reader.
OperationResult<NamedShape> importStlFile(KernelContext& ctx, const std::string& path);
OperationResult<NamedShape> importStl    (KernelContext& ctx,
                                           const std::uint8_t* data,
                                           std::size_t len);

// Tessellate `shape` and write the triangle mesh to an STL file. Returns
// true on success. The shape is NOT mutated. Quotas enforced:
// ctx.quotas().maxMeshTriangles / maxMeshVertices.
OperationResult<bool> exportStlFile(KernelContext& ctx,
                                     const NamedShape& shape,
                                     const std::string& path,
                                     const StlExportOptions& opts = {});

// Same contract as exportStlFile but returns the serialized bytes.
// Respects ctx.quotas().maxSerializeBytes as a soft cap on the final
// buffer size (the tessellation caps run first and are stricter).
OperationResult<std::vector<std::uint8_t>> exportStl(KernelContext& ctx,
                                                      const NamedShape& shape,
                                                      const StlExportOptions& opts = {});

// ═══════════════════════════════════════════════════════════════
// 3MF  (export only; import is deferred — see header comment)
// ═══════════════════════════════════════════════════════════════

struct ThreeMfExportOptions {
    // Tessellation — same defaults as STL.
    double linearDeflection  = 0.1;
    double angularDeflection = 20.0;
    // Optional object name embedded in the 3MF model XML. 3MF printers /
    // slicers surface this as the object label. Empty => "oreo-object".
    std::string objectName;
    // Unit attribute on <model unit="…">. 3MF allows millimeter /
    // micron / centimeter / inch / foot / meter. Kernel operates in mm,
    // so that is the default and recommended value; changing this does
    // NOT rescale the geometry.
    std::string unit = "millimeter";
};

// Export one shape as a single-object 3MF file. Hand-rolled writer —
// tessellates the shape, emits Core 3MF XML (<model>/<resources>/<build>),
// and packs three OPC parts into a store-only ZIP:
//
//     [Content_Types].xml
//     _rels/.rels
//     3D/3dmodel.model
//
// Store-only is a legal 3MF (the Core spec permits STORE or DEFLATE at
// the ZIP layer) and avoids a deflate dependency for the writer path.
// Slicers tested: Bambu, PrusaSlicer, Cura, Microsoft 3D Builder.
OperationResult<bool> exportThreeMfFile(KernelContext& ctx,
                                         const NamedShape& shape,
                                         const std::string& path,
                                         const ThreeMfExportOptions& opts = {});

OperationResult<std::vector<std::uint8_t>> exportThreeMf(KernelContext& ctx,
                                                          const NamedShape& shape,
                                                          const ThreeMfExportOptions& opts = {});

} // namespace oreo

#endif // OREO_MESH_IO_H
