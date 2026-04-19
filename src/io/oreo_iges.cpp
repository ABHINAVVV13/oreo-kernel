// SPDX-License-Identifier: LGPL-2.1-or-later

// oreo_iges.cpp — IGES I/O via OCCT IGESCAFControl.
//
// Mirrors oreo_step.cpp's XDE-first / basic-fallback structure so both
// formats fail the same way and callers can swap between them without
// learning a second error contract.

#include "oreo_iges.h"
#include "shape_fix.h"
#include "temp_file.h"
#include "core/occt_scope_guard.h"
#include "core/occt_try.h"
#include "core/diagnostic_scope.h"
#include "naming/element_map.h"

#include <IGESCAFControl_Reader.hxx>
#include <IGESCAFControl_Writer.hxx>
#include <IGESControl_Reader.hxx>
#include <IGESControl_Writer.hxx>
#include <IFSelect_ReturnStatus.hxx>

#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <TDocStd_Document.hxx>
#include <TDF_Label.hxx>
#include <TDF_LabelSequence.hxx>

#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>
#include <Interface_Static.hxx>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>

namespace oreo {

namespace {

// Temp-file naming + RAII are centralised in io/temp_file.h so STEP / IGES /
// STL share one cross-process-safe implementation. Alias here for brevity.
using TempFile = oreo::io_detail::TempFile;

Handle(TDocStd_Document) makeXcafDoc() {
    Handle(XCAFApp_Application) app = XCAFApp_Application::GetApplication();
    Handle(TDocStd_Document) doc;
    app->NewDocument("MDTV-XCAF", doc);
    return doc;
}

ElementMapPtr initImportedElementMap(KernelContext& ctx, const TopoDS_Shape& shape) {
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

bool importWithXcaf(const std::string& path, TopoDS_Shape& outShape) {
    Handle(TDocStd_Document) doc = makeXcafDoc();
    if (doc.IsNull()) return false;

    IGESCAFControl_Reader reader;
    reader.SetColorMode(Standard_True);
    reader.SetNameMode(Standard_True);
    reader.SetLayerMode(Standard_True);
    if (reader.ReadFile(path.c_str()) != IFSelect_RetDone) return false;
    if (!reader.Transfer(doc)) return false;

    Handle(XCAFDoc_ShapeTool) shapeTool = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    if (shapeTool.IsNull()) return false;

    TDF_LabelSequence roots;
    shapeTool->GetFreeShapes(roots);
    if (roots.Length() == 0) return false;

    if (roots.Length() == 1) {
        outShape = shapeTool->GetShape(roots.Value(1));
    } else {
        // IGESCAFControl doesn't expose OneShape() directly — reuse the
        // basic reader's accumulator on the same file.
        IGESControl_Reader basic;
        if (basic.ReadFile(path.c_str()) != IFSelect_RetDone) return false;
        basic.TransferRoots();
        outShape = basic.OneShape();
    }
    return !outShape.IsNull();
}

bool importBasicFallback(const std::string& path, TopoDS_Shape& outShape) {
    IGESControl_Reader reader;
    if (reader.ReadFile(path.c_str()) != IFSelect_RetDone) return false;
    if (reader.NbRootsForTransfer() == 0) return false;
    reader.TransferRoots();
    outShape = reader.OneShape();
    return !outShape.IsNull();
}

bool exportWithXcaf(const std::vector<NamedShape>& shapes, const std::string& path) {
    Handle(TDocStd_Document) doc = makeXcafDoc();
    if (doc.IsNull()) return false;

    Handle(XCAFDoc_ShapeTool) shapeTool = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    if (shapeTool.IsNull()) return false;

    for (const auto& ns : shapes) {
        if (ns.isNull()) continue;
        shapeTool->AddShape(ns.shape());
    }

    IGESCAFControl_Writer writer;
    writer.SetColorMode(Standard_True);
    writer.SetNameMode(Standard_True);
    writer.SetLayerMode(Standard_True);
    if (!writer.Transfer(doc)) return false;
    return writer.Write(path.c_str()) == Standard_True;
}

bool exportBasicFallback(const std::vector<NamedShape>& shapes, const std::string& path) {
    IGESControl_Writer writer;
    for (const auto& ns : shapes) {
        if (ns.isNull()) continue;
        if (!writer.AddShape(ns.shape())) return false;
    }
    writer.ComputeModel();
    return writer.Write(path.c_str()) == Standard_True;
}

} // anonymous namespace

// ── Import ─────────────────────────────────────────────────────

OperationResult<NamedShape> importIgesFile(KernelContext& ctx, const std::string& path) {
    DiagnosticScope scope(ctx);
    if (path.empty()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT, "Empty IGES file path");
        return scope.makeFailure<NamedShape>();
    }
    if (ctx.checkCancellation()) return scope.makeFailure<NamedShape>();

    // Quota: piggy-back on maxStepBytes since IGES files occupy the
    // same ingestion slot as STEP in practice.
    {
        const std::uint64_t maxStep = ctx.quotas().maxStepBytes;
        if (maxStep > 0) {
            std::error_code ec;
            auto sz = std::filesystem::file_size(path, ec);
            if (!ec && static_cast<std::uint64_t>(sz) > maxStep) {
                ctx.diag().error(ErrorCode::RESOURCE_EXHAUSTED,
                    std::string("IGES file is ") + std::to_string(sz)
                    + " bytes — exceeds quota maxStepBytes="
                    + std::to_string(maxStep));
                return scope.makeFailure<NamedShape>();
            }
        }
    }

    // IGES honours the same "write.iges.unit" / "read.iges.unit" global
    // knobs as STEP — scope them through OcctStaticGuard so parallel
    // import threads don't clobber each other's state.
    OcctStaticGuard importGuard;
    importGuard.set("read.iges.bspline.continuity", "2");
    importGuard.set("xstep.cascade.unit", "MM");

    OREO_OCCT_TRY
    TopoDS_Shape shape;
    bool xcafOk = importWithXcaf(path, shape);
    if (!xcafOk) {
        if (!importBasicFallback(path, shape)) {
            ctx.diag().error(ErrorCode::STEP_IMPORT_FAILED,
                "IGES import failed (both XCAF and basic readers): " + path,
                {}, "File may be truncated or use unsupported IGES entities.");
            return scope.makeFailure<NamedShape>();
        }
        ctx.diag().warning(ErrorCode::SHAPE_INVALID,
            "IGES import fell back to basic reader — colors / names / layers not imported",
            "File geometry is intact but XDE metadata could not be extracted");
    }
    if (shape.IsNull()) {
        ctx.diag().error(ErrorCode::STEP_IMPORT_FAILED,
            "IGES import produced null shape from: " + path);
        return scope.makeFailure<NamedShape>();
    }
    if (!isShapeValid(shape)) {
        fixShape(shape);
    }
    auto map = initImportedElementMap(ctx, shape);
    return scope.makeResult(NamedShape(shape, map, ctx.tags().nextShapeIdentity()));
    OREO_OCCT_CATCH_NS(scope, ctx, "importIgesFile")
}

OperationResult<NamedShape> importIges(KernelContext& ctx,
                                        const std::uint8_t* data, std::size_t len) {
    DiagnosticScope scope(ctx);
    if (!data || len == 0) {
        ctx.diag().error(ErrorCode::INVALID_INPUT, "Empty IGES data buffer");
        return scope.makeFailure<NamedShape>();
    }
    const std::uint64_t maxStep = ctx.quotas().maxStepBytes;
    if (maxStep > 0 && static_cast<std::uint64_t>(len) > maxStep) {
        ctx.diag().error(ErrorCode::RESOURCE_EXHAUSTED,
            std::string("IGES buffer is ") + std::to_string(len)
            + " bytes — exceeds quota maxStepBytes="
            + std::to_string(maxStep));
        return scope.makeFailure<NamedShape>();
    }

    TempFile tmp("oreo_import_iges", ".iges");
    {
        std::ofstream ofs(tmp.path(), std::ios::binary);
        if (!ofs) {
            ctx.diag().error(ErrorCode::STEP_IMPORT_FAILED,
                "Failed to stage IGES buffer into temp file: " + tmp.path());
            return scope.makeFailure<NamedShape>();
        }
        ofs.write(reinterpret_cast<const char*>(data),
                  static_cast<std::streamsize>(len));
    }
    return importIgesFile(ctx, tmp.path());
}

// ── Export ─────────────────────────────────────────────────────

OperationResult<bool> exportIgesFile(KernelContext& ctx,
                                      const std::vector<NamedShape>& shapes,
                                      const std::string& path) {
    DiagnosticScope scope(ctx);
    if (shapes.empty()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT, "No shapes to export to IGES");
        return scope.makeFailure<bool>();
    }
    if (path.empty()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT, "Empty IGES export path");
        return scope.makeFailure<bool>();
    }

    OcctStaticGuard exportGuard;
    exportGuard.set("write.iges.unit", "MM");
    exportGuard.set("write.iges.brep.mode", "1");
    exportGuard.set("xstep.cascade.unit",  "MM");

    OREO_OCCT_TRY
    if (exportWithXcaf(shapes, path)) return scope.makeResult(true);
    if (!exportBasicFallback(shapes, path)) {
        ctx.diag().error(ErrorCode::STEP_EXPORT_FAILED,
            "IGES export failed (both XCAF and basic writers): " + path,
            {}, "Shape may be invalid — try running ShapeFix first. "
                "Check write permissions for the target path.");
        return scope.makeFailure<bool>();
    }
    ctx.diag().warning(ErrorCode::SHAPE_INVALID,
        "IGES export fell back to basic writer — colors / names / layers not written");
    return scope.makeResult(true);
    OREO_OCCT_CATCH_T(scope, ctx, bool, "exportIgesFile")
}

OperationResult<std::vector<std::uint8_t>> exportIges(KernelContext& ctx,
                                                       const std::vector<NamedShape>& shapes) {
    DiagnosticScope scope(ctx);
    TempFile tmp("oreo_export_iges", ".iges");
    auto wr = exportIgesFile(ctx, shapes, tmp.path());
    if (!wr.ok()) return scope.makeFailure<std::vector<std::uint8_t>>();

    std::ifstream ifs(tmp.path(), std::ios::binary | std::ios::ate);
    if (!ifs) {
        ctx.diag().error(ErrorCode::STEP_EXPORT_FAILED,
            "Failed to read back exported IGES file: " + tmp.path());
        return scope.makeFailure<std::vector<std::uint8_t>>();
    }
    auto size = ifs.tellg();
    ifs.seekg(0);
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(size));
    ifs.read(reinterpret_cast<char*>(buf.data()), size);
    return scope.makeResult(std::move(buf));
}

} // namespace oreo
