// SPDX-License-Identifier: LGPL-2.1-or-later

// oreo_mesh_io.cpp — STL via OCCT StlAPI + hand-rolled 3MF export.
//
// STL notes:
//   * StlAPI_Reader returns a TopoDS_Shape (compound of shells) and
//     auto-detects ASCII vs binary by sniffing the header.
//   * StlAPI_Writer needs an incremental-meshed shape; the kernel's
//     existing mesh pipeline (mesh/oreo_mesh.cpp) lives in a different
//     module, so we re-invoke BRepMesh_IncrementalMesh here rather than
//     cross the layering fence. Identical defaults to the OreoMesh path.
//
// 3MF notes:
//   * The Core 3MF spec (https://3mf.io/) says a conforming consumer
//     MUST accept STORE-only ZIPs with the three required parts below.
//     Hand-rolled store-only writer avoids a deflate dep.
//   * CRC-32/ISO-3309 (IEEE polynomial reversed = 0xEDB88320) is the
//     only non-trivial piece of the ZIP format we have to carry; it's
//     ~20 lines.
//   * Every 3MF <mesh> must carry <vertices> with at least 3 entries
//     and <triangles> with at least 1 entry, and each triangle's
//     vertex indices must be < vertex count. We enforce these before
//     emitting to avoid shipping malformed files that most slicers
//     accept silently and a minority reject loudly.

#include "oreo_mesh_io.h"
#include "temp_file.h"
#include "shape_fix.h"

#include "core/diagnostic_scope.h"
#include "core/occt_try.h"
#include "naming/element_map.h"

#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <StlAPI_Reader.hxx>
#include <StlAPI_Writer.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Shell.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace oreo {

namespace {

// ── Tessellation helper shared by STL + 3MF export paths ─────
struct Mesh {
    std::vector<std::array<double, 3>> vertices;
    std::vector<std::array<std::uint32_t, 3>> triangles;
};

// Tessellate `shape` into a flat vertex + triangle mesh. Honours the
// context's meshTriangles / meshVertices quotas — if either would be
// exceeded we emit an error and return an empty mesh. Returns true on
// success, false on failure (diagnostic already written).
bool tessellate(KernelContext& ctx, const TopoDS_Shape& shape,
                double linDef, double angDefRad, Mesh& out) {
    if (shape.IsNull()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                         "Cannot tessellate a null shape");
        return false;
    }

    // BRepMesh mutates its input (attaches Poly_Triangulation). Copying
    // the TopoDS_Shape handle is cheap — it bumps a ref-count — but the
    // underlying triangulation lives on the original shape too. That is
    // acceptable: the caller owns `shape` and we're inside a
    // short-lived export, so a stale triangulation being left behind on
    // the NamedShape is not a correctness issue. Meshing is idempotent
    // (re-meshing with the same parameters replaces the attached triangulation).
    // isInParallel=False is REQUIRED here. OCCT maintains process-wide
    // static caches (see src/core/thread_safety.h rule #2) that are not
    // thread-safe; running BRepMesh in parallel contradicts the documented
    // safe default in MeshParams (mesh/oreo_mesh.h: `parallel = false`)
    // and can corrupt triangulation in multi-tenant server builds.
    // If a future single-worker export ever wants to enable parallel
    // meshing, promote this into a MeshParams-driven call so the choice
    // is explicit per-request, not a hardcoded IO default.
    BRepMesh_IncrementalMesh mesher(const_cast<TopoDS_Shape&>(shape),
                                    linDef, /*isRelative=*/Standard_False,
                                    angDefRad, /*isInParallel=*/Standard_False);
    mesher.Perform();

    const std::uint64_t triCap  = ctx.quotas().maxMeshTriangles;
    const std::uint64_t vtxCap  = ctx.quotas().maxMeshVertices;
    std::uint64_t triTotal = 0, vtxTotal = 0;

    for (TopExp_Explorer exp(shape, TopAbs_FACE); exp.More(); exp.Next()) {
        const TopoDS_Face& face = TopoDS::Face(exp.Current());
        TopLoc_Location loc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
        if (tri.IsNull()) continue;

        const std::uint32_t vtxOffset = static_cast<std::uint32_t>(out.vertices.size());
        const int nbNodes = tri->NbNodes();
        const int nbTris  = tri->NbTriangles();

        triTotal += static_cast<std::uint64_t>(nbTris);
        vtxTotal += static_cast<std::uint64_t>(nbNodes);
        if (triCap > 0 && triTotal > triCap) {
            ctx.diag().error(ErrorCode::RESOURCE_EXHAUSTED,
                std::string("Tessellation would produce ")
                + std::to_string(triTotal) + " triangles — exceeds quota "
                + std::to_string(triCap) + ". Reduce tessellation "
                "deflection or raise quotas.maxMeshTriangles.");
            out = {};
            return false;
        }
        if (vtxCap > 0 && vtxTotal > vtxCap) {
            ctx.diag().error(ErrorCode::RESOURCE_EXHAUSTED,
                std::string("Tessellation would produce ")
                + std::to_string(vtxTotal) + " vertices — exceeds quota "
                + std::to_string(vtxCap) + ".");
            out = {};
            return false;
        }

        out.vertices.reserve(out.vertices.size() + static_cast<std::size_t>(nbNodes));
        out.triangles.reserve(out.triangles.size() + static_cast<std::size_t>(nbTris));

        const gp_Trsf& trsf = loc.Transformation();
        const bool identityLoc = loc.IsIdentity();

        for (int i = 1; i <= nbNodes; ++i) {
            gp_Pnt p = tri->Node(i);
            if (!identityLoc) p.Transform(trsf);
            out.vertices.push_back({p.X(), p.Y(), p.Z()});
        }

        // OCCT reports triangles with winding in the topological face's
        // forward orientation. If the face is reversed in its parent
        // shell we have to flip winding so the emitted surface normal
        // points outward — printers silently misprint when winding is
        // inconsistent.
        const bool reversed = (face.Orientation() == TopAbs_REVERSED);
        for (int i = 1; i <= nbTris; ++i) {
            Standard_Integer a, b, c;
            tri->Triangle(i).Get(a, b, c);
            std::uint32_t va = vtxOffset + static_cast<std::uint32_t>(a - 1);
            std::uint32_t vb = vtxOffset + static_cast<std::uint32_t>(b - 1);
            std::uint32_t vc = vtxOffset + static_cast<std::uint32_t>(c - 1);
            if (reversed) std::swap(vb, vc);
            // Skip degenerate triangles (any two indices equal) — these
            // can arise from near-duplicate vertices on tangential faces
            // and either confuse downstream consumers or cause 3MF
            // conformance validators to reject the file.
            if (va == vb || vb == vc || va == vc) continue;
            out.triangles.push_back({va, vb, vc});
        }
    }

    if (out.triangles.empty()) {
        ctx.diag().error(ErrorCode::OCCT_FAILURE,
            "Tessellation produced no triangles — shape may be empty "
            "or meshing parameters are too coarse for the geometry scale.");
        return false;
    }
    return true;
}

// Ensure parent directory exists; OCCT writers expect the path to be
// openable directly.
bool ensureParentDir(KernelContext& ctx, const std::string& path) {
    std::error_code ec;
    std::filesystem::path fsPath(path);
    auto parent = fsPath.parent_path();
    if (parent.empty()) return true;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
            std::string("Cannot create parent directory '") + parent.string()
            + "': " + ec.message());
        return false;
    }
    return true;
}

ElementMapPtr initImportedMeshElementMap(KernelContext& ctx,
                                          const TopoDS_Shape& shape) {
    // Flat element map: one face entry per imported triangle's owning
    // face, plus edge/vertex coverage. Matches the STEP import scheme
    // so downstream code treats an imported STL the same way.
    auto map = std::make_shared<ElementMap>();
    auto mapType = [&](TopAbs_ShapeEnum type, const char* typeName) {
        TopTools_IndexedMapOfShape subs;
        TopExp::MapShapes(shape, type, subs);
        for (int i = 1; i <= subs.Extent(); ++i) {
            IndexedName idx(typeName, i);
            MappedName base(std::string(typeName) + std::to_string(i));
            MappedName name = ElementMap::encodeElementName(
                typeName, base, ctx.tags().nextShapeIdentity(), ";:Nimport");
            map->setElementName(idx, name);
        }
    };
    mapType(TopAbs_FACE,   "Face");
    mapType(TopAbs_EDGE,   "Edge");
    mapType(TopAbs_VERTEX, "Vertex");
    return map;
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════
// STL import
// ═══════════════════════════════════════════════════════════════

OperationResult<NamedShape> importStlFile(KernelContext& ctx, const std::string& path) {
    DiagnosticScope scope(ctx);
    if (path.empty()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT, "Empty STL file path");
        return scope.makeFailure<NamedShape>();
    }
    if (ctx.checkCancellation()) return scope.makeFailure<NamedShape>();

    // Quota: file size sanity check.
    {
        const std::uint64_t maxStep = ctx.quotas().maxStepBytes;
        if (maxStep > 0) {
            std::error_code ec;
            auto sz = std::filesystem::file_size(path, ec);
            if (!ec && static_cast<std::uint64_t>(sz) > maxStep) {
                ctx.diag().error(ErrorCode::RESOURCE_EXHAUSTED,
                    std::string("STL file is ") + std::to_string(sz)
                    + " bytes — exceeds quota maxStepBytes="
                    + std::to_string(maxStep));
                return scope.makeFailure<NamedShape>();
            }
        }
    }

    OREO_OCCT_TRY
    StlAPI_Reader reader;
    TopoDS_Shape shape;
    if (!reader.Read(shape, path.c_str())) {
        ctx.diag().error(ErrorCode::STEP_IMPORT_FAILED,
            "STL reader failed to parse '" + path + "'",
            {}, "File may be truncated, non-STL, or use an unsupported variant.");
        return scope.makeFailure<NamedShape>();
    }
    if (shape.IsNull()) {
        ctx.diag().error(ErrorCode::STEP_IMPORT_FAILED,
            "STL reader returned a null shape from '" + path + "'");
        return scope.makeFailure<NamedShape>();
    }

    // StlAPI_Reader hands back a compound of faces. Try to sew them
    // into a shell so the rest of the kernel sees a coherent topology.
    // On failure we fall back to the raw compound.
    TopoDS_Shape result = shape;
    {
        BRepBuilderAPI_Sewing sewer(/*tolerance=*/1e-6);
        sewer.Add(shape);
        sewer.Perform();
        TopoDS_Shape sewn = sewer.SewedShape();
        if (!sewn.IsNull()) result = sewn;
    }

    auto map = initImportedMeshElementMap(ctx, result);
    return scope.makeResult(NamedShape(result, map, ctx.tags().nextShapeIdentity()));
    OREO_OCCT_CATCH_NS(scope, ctx, "importStlFile")
}

OperationResult<NamedShape> importStl(KernelContext& ctx,
                                       const std::uint8_t* data, std::size_t len) {
    DiagnosticScope scope(ctx);
    if (!data || len == 0) {
        ctx.diag().error(ErrorCode::INVALID_INPUT, "Empty STL data buffer");
        return scope.makeFailure<NamedShape>();
    }

    // OCCT's StlAPI_Reader only has a file-path API, so we stage into
    // a temp file. Use the shared collision-resistant helper (atomic
    // counter + thread id) so concurrent server requests cannot step
    // on each other's staging files — the previous ::rand() naming
    // was trivial to alias under load.
    io_detail::TempFile tmp("oreo_import_stl", ".stl");
    {
        std::ofstream ofs(tmp.path(), std::ios::binary);
        if (!ofs) {
            ctx.diag().error(ErrorCode::STEP_IMPORT_FAILED,
                "Failed to open temp file for STL buffer import: " + tmp.path());
            return scope.makeFailure<NamedShape>();
        }
        ofs.write(reinterpret_cast<const char*>(data),
                  static_cast<std::streamsize>(len));
    }
    return importStlFile(ctx, tmp.path());
}

// ═══════════════════════════════════════════════════════════════
// STL export
// ═══════════════════════════════════════════════════════════════

OperationResult<bool> exportStlFile(KernelContext& ctx, const NamedShape& shape,
                                     const std::string& path,
                                     const StlExportOptions& opts) {
    DiagnosticScope scope(ctx);
    if (shape.isNull()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT, "Cannot export a null shape to STL");
        return scope.makeFailure<bool>();
    }
    if (path.empty()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT, "Empty STL export path");
        return scope.makeFailure<bool>();
    }
    if (!ensureParentDir(ctx, path)) return scope.makeFailure<bool>();

    OREO_OCCT_TRY
    TopoDS_Shape working = shape.shape();
    if (opts.healBeforeTessellate) {
        // fixShape mutates in place. Copy the handle (cheap) so the
        // caller's NamedShape is untouched.
        TopoDS_Shape copy = working;
        fixShape(copy);
        working = copy;
    }

    // Run a tessellation pass so OCCT's StlAPI_Writer has triangles
    // attached to every face. Same deflection semantics as the OreoMesh
    // pipeline.
    //
    // isInParallel=False is REQUIRED for parity with the shared
    // `tessellate()` helper above and the safe default documented on
    // MeshParams (mesh/oreo_mesh.h: `parallel = false`). OCCT's
    // BRepMesh maintains process-wide static caches that are not
    // thread-safe (see src/core/thread_safety.h rule #2); running with
    // parallel=True in a multi-tenant server build can race with any
    // other BRepMesh invocation in the same process and corrupt the
    // resulting triangulation. Prior to 2026-04-19 this path still
    // passed Standard_True, which is what the audit flagged.
    BRepMesh_IncrementalMesh mesher(working, opts.linearDeflection,
                                    /*isRelative=*/Standard_False,
                                    opts.angularDeflection * (M_PI / 180.0),
                                    /*isInParallel=*/Standard_False);
    mesher.Perform();
    if (!mesher.IsDone()) {
        ctx.diag().error(ErrorCode::OCCT_FAILURE,
            "Tessellation pre-pass failed; STL export aborted.");
        return scope.makeFailure<bool>();
    }

    StlAPI_Writer writer;
    writer.ASCIIMode() = (opts.format == StlFormat::Ascii) ? Standard_True : Standard_False;
    if (!writer.Write(working, path.c_str())) {
        ctx.diag().error(ErrorCode::STEP_EXPORT_FAILED,
            "StlAPI_Writer::Write failed for path '" + path + "'",
            {}, "Check filesystem permissions and free space.");
        return scope.makeFailure<bool>();
    }
    return scope.makeResult(true);
    OREO_OCCT_CATCH_T(scope, ctx, bool, "exportStlFile")
}

OperationResult<std::vector<std::uint8_t>> exportStl(KernelContext& ctx,
                                                      const NamedShape& shape,
                                                      const StlExportOptions& opts) {
    DiagnosticScope scope(ctx);
    // Collision-resistant staging — the TempFile destructor removes
    // the file on every return path, including exception unwind.
    io_detail::TempFile tmp("oreo_export_stl", ".stl");
    auto wr = exportStlFile(ctx, shape, tmp.path(), opts);
    if (!wr.ok()) {
        return scope.makeFailure<std::vector<std::uint8_t>>();
    }

    std::ifstream ifs(tmp.path(), std::ios::binary | std::ios::ate);
    if (!ifs) {
        ctx.diag().error(ErrorCode::STEP_EXPORT_FAILED,
            "Failed to reopen STL temp file for buffer read");
        return scope.makeFailure<std::vector<std::uint8_t>>();
    }
    auto size = ifs.tellg();
    ifs.seekg(0);
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(size));
    ifs.read(reinterpret_cast<char*>(buf.data()), size);

    const std::uint64_t maxSer = ctx.quotas().maxSerializeBytes;
    if (maxSer > 0 && static_cast<std::uint64_t>(buf.size()) > maxSer) {
        ctx.diag().error(ErrorCode::RESOURCE_EXHAUSTED,
            std::string("STL export is ") + std::to_string(buf.size())
            + " bytes — exceeds context quota maxSerializeBytes="
            + std::to_string(maxSer));
        return scope.makeFailure<std::vector<std::uint8_t>>();
    }
    return scope.makeResult(std::move(buf));
}

// ═══════════════════════════════════════════════════════════════
// 3MF export — store-only ZIP over Core 3MF Model XML
// ═══════════════════════════════════════════════════════════════

namespace {

// CRC-32/ISO-3309 (reflected polynomial 0xEDB88320). The table is
// thread-safely initialized exactly once via a C++11 function-local
// static of a constexpr-friendly wrapper — the standard guarantees
// that function-local static initialization is thread-safe and
// happens-before any call to the function that dereferences it.
//
// (The previous implementation used a plain `static bool ready` which
// is a textbook data race when two threads export 3MF concurrently
// — TSan flags it, and the reads of `table[...]` before `ready` is
// published on the publishing thread are UB.)
struct Crc32Table {
    std::uint32_t table[256];
    constexpr Crc32Table() : table() {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
    }
};

std::uint32_t crc32Update(std::uint32_t crc, const std::uint8_t* p, std::size_t n) {
    // Magic-static: C++11 [stmt.dcl]/4 guarantees concurrent first-time
    // initialization is serialized and the construction happens-before
    // any observer's read. This removes the race cleanly without
    // introducing a heavyweight std::call_once.
    static const Crc32Table kTable{};
    crc ^= 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i)
        crc = kTable.table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

void zipWriteLE(std::vector<std::uint8_t>& buf, std::uint16_t v) {
    buf.push_back(static_cast<std::uint8_t>(v & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}
void zipWriteLE(std::vector<std::uint8_t>& buf, std::uint32_t v) {
    buf.push_back(static_cast<std::uint8_t>(v & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}
void zipAppend(std::vector<std::uint8_t>& buf, const std::string& s) {
    buf.insert(buf.end(), s.begin(), s.end());
}
void zipAppend(std::vector<std::uint8_t>& buf, const std::uint8_t* p, std::size_t n) {
    buf.insert(buf.end(), p, p + n);
}

// One file entry in the store-only ZIP.
struct ZipEntry {
    std::string  name;     // OPC part name (POSIX-style, no leading slash)
    std::vector<std::uint8_t> data;
    std::uint32_t crc = 0;
    std::uint32_t localHeaderOffset = 0;
};

// Emit a minimal, spec-compliant store-only ZIP. No compression, no
// ZIP64 (3MF is usually < 4 GiB — we enforce that upstream by refusing
// oversized meshes via the mesh-quota). DOS timestamp fixed at
// 2026-01-01 00:00:00 so byte-identical rebuilds survive across hosts
// — the determinism test suite relies on this.
std::vector<std::uint8_t> buildZip(std::vector<ZipEntry>& entries) {
    constexpr std::uint16_t kDosTime = (0 << 11) | (0 << 5) | (0 / 2);          // 00:00:00
    constexpr std::uint16_t kDosDate = ((2026 - 1980) << 9) | (1 << 5) | 1;     // 2026-01-01

    std::vector<std::uint8_t> buf;
    buf.reserve(4096);

    // Local file headers + data.
    for (auto& e : entries) {
        e.crc = crc32Update(0, e.data.data(), e.data.size());
        e.localHeaderOffset = static_cast<std::uint32_t>(buf.size());
        zipWriteLE(buf, std::uint32_t{0x04034b50u});  // local file header signature
        zipWriteLE(buf, std::uint16_t{20});           // version needed to extract
        zipWriteLE(buf, std::uint16_t{0});            // flags
        zipWriteLE(buf, std::uint16_t{0});            // STORE
        zipWriteLE(buf, kDosTime);
        zipWriteLE(buf, kDosDate);
        zipWriteLE(buf, e.crc);
        zipWriteLE(buf, static_cast<std::uint32_t>(e.data.size())); // compressed
        zipWriteLE(buf, static_cast<std::uint32_t>(e.data.size())); // uncompressed
        zipWriteLE(buf, static_cast<std::uint16_t>(e.name.size()));
        zipWriteLE(buf, std::uint16_t{0});            // extra field length
        zipAppend(buf, e.name);
        zipAppend(buf, e.data.data(), e.data.size());
    }

    // Central directory.
    const std::uint32_t cdOffset = static_cast<std::uint32_t>(buf.size());
    for (const auto& e : entries) {
        zipWriteLE(buf, std::uint32_t{0x02014b50u});  // central dir header
        zipWriteLE(buf, std::uint16_t{20});           // version made by
        zipWriteLE(buf, std::uint16_t{20});           // version needed
        zipWriteLE(buf, std::uint16_t{0});            // flags
        zipWriteLE(buf, std::uint16_t{0});            // STORE
        zipWriteLE(buf, kDosTime);
        zipWriteLE(buf, kDosDate);
        zipWriteLE(buf, e.crc);
        zipWriteLE(buf, static_cast<std::uint32_t>(e.data.size()));
        zipWriteLE(buf, static_cast<std::uint32_t>(e.data.size()));
        zipWriteLE(buf, static_cast<std::uint16_t>(e.name.size()));
        zipWriteLE(buf, std::uint16_t{0});            // extra
        zipWriteLE(buf, std::uint16_t{0});            // comment
        zipWriteLE(buf, std::uint16_t{0});            // disk number
        zipWriteLE(buf, std::uint16_t{0});            // internal attrs
        zipWriteLE(buf, std::uint32_t{0});            // external attrs
        zipWriteLE(buf, e.localHeaderOffset);
        zipAppend(buf, e.name);
    }
    const std::uint32_t cdSize = static_cast<std::uint32_t>(buf.size() - cdOffset);

    // End-of-central-directory record.
    zipWriteLE(buf, std::uint32_t{0x06054b50u});
    zipWriteLE(buf, std::uint16_t{0});                // disk number
    zipWriteLE(buf, std::uint16_t{0});                // disk with central dir
    zipWriteLE(buf, static_cast<std::uint16_t>(entries.size()));  // entries on this disk
    zipWriteLE(buf, static_cast<std::uint16_t>(entries.size()));  // total entries
    zipWriteLE(buf, cdSize);
    zipWriteLE(buf, cdOffset);
    zipWriteLE(buf, std::uint16_t{0});                // comment length

    return buf;
}

// Escape a user-supplied string for insertion into XML attribute /
// element content. 3MF tolerates the Core set; the <model> unit and
// object-name are the only two places user text reaches the XML.
std::string xmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20 &&
                    c != '\t' && c != '\n' && c != '\r') {
                    // Drop control chars; 3MF is XML 1.0, can't carry them.
                    break;
                }
                out += c;
        }
    }
    return out;
}

std::string buildContentTypesXml() {
    return
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
"<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
  "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
  "<Default Extension=\"model\" ContentType=\"application/vnd.ms-package.3dmanufacturing-3dmodel+xml\"/>"
"</Types>";
}

std::string buildRelsXml() {
    return
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
"<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
  "<Relationship Target=\"/3D/3dmodel.model\" Id=\"rel0\" "
                "Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/>"
"</Relationships>";
}

std::string buildModelXml(const Mesh& mesh, const ThreeMfExportOptions& opts) {
    std::ostringstream oss;
    oss.imbue(std::locale::classic()); // force '.' for decimals
    oss << std::setprecision(9);

    const std::string unit = opts.unit.empty() ? std::string("millimeter") : opts.unit;
    const std::string name = opts.objectName.empty() ? std::string("oreo-object") : opts.objectName;

    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        << "<model unit=\"" << xmlEscape(unit) << "\" "
        << "xml:lang=\"en-US\" "
        << "xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\">"
        << "<metadata name=\"Application\">oreo-kernel</metadata>"
        << "<resources>"
        << "<object id=\"1\" type=\"model\" name=\"" << xmlEscape(name) << "\">"
        << "<mesh>"
        << "<vertices>";

    for (const auto& v : mesh.vertices) {
        oss << "<vertex x=\"" << v[0] << "\" y=\"" << v[1] << "\" z=\"" << v[2] << "\"/>";
    }
    oss << "</vertices><triangles>";
    for (const auto& t : mesh.triangles) {
        oss << "<triangle v1=\"" << t[0] << "\" v2=\"" << t[1] << "\" v3=\"" << t[2] << "\"/>";
    }
    oss << "</triangles></mesh></object></resources>"
        << "<build><item objectid=\"1\"/></build></model>";

    return oss.str();
}

} // anonymous namespace

OperationResult<std::vector<std::uint8_t>> exportThreeMf(KernelContext& ctx,
                                                          const NamedShape& shape,
                                                          const ThreeMfExportOptions& opts) {
    DiagnosticScope scope(ctx);
    if (shape.isNull()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT, "Cannot export a null shape to 3MF");
        return scope.makeFailure<std::vector<std::uint8_t>>();
    }

    Mesh mesh;
    const double angRad = opts.angularDeflection * (M_PI / 180.0);
    if (!tessellate(ctx, shape.shape(), opts.linearDeflection, angRad, mesh)) {
        return scope.makeFailure<std::vector<std::uint8_t>>();
    }

    // 3MF's <triangle> v1/v2/v3 use uint32 indices. We also need each
    // index < vertex count; tessellate() guarantees this by construction
    // but fail-closed on a mismatch so a future refactor can't ship
    // malformed files.
    const std::uint32_t vtxN = static_cast<std::uint32_t>(mesh.vertices.size());
    for (const auto& t : mesh.triangles) {
        if (t[0] >= vtxN || t[1] >= vtxN || t[2] >= vtxN) {
            ctx.diag().error(ErrorCode::INTERNAL_ERROR,
                "3MF export: triangle index >= vertex count. Tessellator "
                "produced an inconsistent mesh — refusing to ship a "
                "malformed 3MF.");
            return scope.makeFailure<std::vector<std::uint8_t>>();
        }
    }

    const std::string modelXml = buildModelXml(mesh, opts);
    const std::string ctXml    = buildContentTypesXml();
    const std::string relsXml  = buildRelsXml();

    // Order MATTERS: some 3MF validators check that [Content_Types].xml
    // is the first entry in the central directory.
    std::vector<ZipEntry> entries;
    entries.reserve(3);
    entries.push_back({"[Content_Types].xml",
                       std::vector<std::uint8_t>(ctXml.begin(), ctXml.end())});
    entries.push_back({"_rels/.rels",
                       std::vector<std::uint8_t>(relsXml.begin(), relsXml.end())});
    entries.push_back({"3D/3dmodel.model",
                       std::vector<std::uint8_t>(modelXml.begin(), modelXml.end())});

    auto buf = buildZip(entries);

    const std::uint64_t maxSer = ctx.quotas().maxSerializeBytes;
    if (maxSer > 0 && static_cast<std::uint64_t>(buf.size()) > maxSer) {
        ctx.diag().error(ErrorCode::RESOURCE_EXHAUSTED,
            std::string("3MF export is ") + std::to_string(buf.size())
            + " bytes — exceeds context quota maxSerializeBytes="
            + std::to_string(maxSer));
        return scope.makeFailure<std::vector<std::uint8_t>>();
    }
    return scope.makeResult(std::move(buf));
}

OperationResult<bool> exportThreeMfFile(KernelContext& ctx, const NamedShape& shape,
                                         const std::string& path,
                                         const ThreeMfExportOptions& opts) {
    DiagnosticScope scope(ctx);
    if (path.empty()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT, "Empty 3MF export path");
        return scope.makeFailure<bool>();
    }
    if (!ensureParentDir(ctx, path)) return scope.makeFailure<bool>();

    auto buf = exportThreeMf(ctx, shape, opts);
    if (!buf.ok()) return scope.makeFailure<bool>();

    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        ctx.diag().error(ErrorCode::STEP_EXPORT_FAILED,
            "Failed to open '" + path + "' for 3MF write");
        return scope.makeFailure<bool>();
    }
    const auto& bytes = buf.value();
    ofs.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!ofs) {
        ctx.diag().error(ErrorCode::STEP_EXPORT_FAILED,
            "Short write to '" + path + "' during 3MF export");
        return scope.makeFailure<bool>();
    }
    return scope.makeResult(true);
}

} // namespace oreo
