// oreo_step.cpp — Production-grade STEP I/O with XDE metadata.
//
// Import: file path or memory buffer -> XCAF document -> shapes + colors/names/layers
// Export: NamedShape(s) + metadata -> XCAF document -> STEP file
//
// Uses STEPCAFControl_Reader/Writer for extended data (colors, names, layers).
// Falls back to basic STEPControl if XCAF processing fails — geometry is
// always returned even if metadata extraction fails.
//
// Memory buffer I/O avoids temp files where possible, falls back to temp
// files when OCCT's API requires a file path.

#include "oreo_step.h"
#include "shape_fix.h"
#include "core/occt_scope_guard.h"
#include "core/oreo_error.h"
#include "core/operation_result.h"
#include "core/diagnostic_scope.h"
#include "naming/named_shape.h"
#include "naming/element_map.h"

// XCAF document framework
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_LayerTool.hxx>
#include <TDocStd_Document.hxx>
#include <TDataStd_Name.hxx>
#include <TDF_Label.hxx>
#include <TDF_LabelSequence.hxx>

// STEP CAF I/O (XDE-aware)
#include <STEPCAFControl_Reader.hxx>
#include <STEPCAFControl_Writer.hxx>

// Basic STEP I/O (fallback)
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>

#include <Interface_Static.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS_Shape.hxx>
#include <Quantity_Color.hxx>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace oreo {

namespace {

// Generate a unique temp file path that won't collide with other processes.
std::string uniqueTempPath(const std::string& prefix, const std::string& ext) {
    static std::atomic<int> counter{0};
    std::string name = prefix + "_" + std::to_string(counter++) + "_"
                     + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()))
                     + ext;
    auto tempDir = std::filesystem::temp_directory_path();
    return (tempDir / name).string();
}

// Initialize element map for an imported shape (no history — fresh names).
ElementMapPtr initImportedElementMap(const TopoDS_Shape& shape) {
    auto map = std::make_shared<ElementMap>();

    auto mapType = [&](TopAbs_ShapeEnum type, const char* typeName) {
        TopTools_IndexedMapOfShape subShapes;
        TopExp::MapShapes(shape, type, subShapes);
        for (int i = 1; i <= subShapes.Extent(); ++i) {
            IndexedName idx(typeName, i);
            MappedName name(std::string(typeName) + std::to_string(i) + ";:Nimport");
            map->setElementName(idx, name);
        }
    };

    mapType(TopAbs_FACE, "Face");
    mapType(TopAbs_EDGE, "Edge");
    mapType(TopAbs_VERTEX, "Vertex");

    return map;
}

// RAII temp file cleanup
struct TempFile {
    std::string path;
    TempFile(const std::string& prefix, const std::string& ext)
        : path(uniqueTempPath(prefix, ext)) {}
    ~TempFile() {
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};

// Create a fresh XCAF document for import/export.
Handle(TDocStd_Document) createXcafDocument() {
    Handle(XCAFApp_Application) app = XCAFApp_Application::GetApplication();
    Handle(TDocStd_Document) doc;
    app->NewDocument("MDTV-XCAF", doc);
    return doc;
}

// Extract metadata (name, color, layer) from an XCAF label.
StepShapeMetadata extractMetadata(const TDF_Label& label,
                                  const Handle(XCAFDoc_ColorTool)& colorTool,
                                  const Handle(XCAFDoc_LayerTool)& layerTool) {
    StepShapeMetadata meta;

    // Name
    Handle(TDataStd_Name) nameAttr;
    if (label.FindAttribute(TDataStd_Name::GetID(), nameAttr)) {
        meta.name = nameAttr->Get().ToExtString()
            ? std::string(TCollection_AsciiString(nameAttr->Get()).ToCString())
            : std::string();
    }

    // Color — try surface color first, then generic color
    Quantity_Color color;
    if (colorTool->GetColor(label, XCAFDoc_ColorSurf, color)) {
        meta.hasColor = true;
        meta.color = color;
    } else if (colorTool->GetColor(label, XCAFDoc_ColorGen, color)) {
        meta.hasColor = true;
        meta.color = color;
    } else if (colorTool->GetColor(label, XCAFDoc_ColorCurv, color)) {
        meta.hasColor = true;
        meta.color = color;
    }

    // Layer — take first layer assignment
    TDF_LabelSequence layers;
    layerTool->GetLayers(label, layers);
    if (layers.Length() > 0) {
        TCollection_ExtendedString layerName;
        if (layerTool->GetLayer(layers.Value(1), layerName)) {
            meta.layerName = TCollection_AsciiString(layerName).ToCString();
        }
    }

    return meta;
}

// Import using XCAF reader (full metadata).
// Returns true on success, fills shape/metadata output params.
bool importWithXcaf(const std::string& path,
                    TopoDS_Shape& outShape,
                    StepShapeMetadata& outMeta) {
    Handle(TDocStd_Document) doc = createXcafDocument();
    if (doc.IsNull()) return false;

    STEPCAFControl_Reader cafReader;
    cafReader.SetColorMode(Standard_True);
    cafReader.SetNameMode(Standard_True);
    cafReader.SetLayerMode(Standard_True);
    cafReader.SetPropsMode(Standard_True);

    IFSelect_ReturnStatus status = cafReader.ReadFile(path.c_str());
    if (status != IFSelect_RetDone) return false;

    if (!cafReader.Transfer(doc)) return false;

    // Get shape and color tools from the document
    Handle(XCAFDoc_ShapeTool) shapeTool = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    Handle(XCAFDoc_ColorTool) colorTool = XCAFDoc_DocumentTool::ColorTool(doc->Main());
    Handle(XCAFDoc_LayerTool) layerTool = XCAFDoc_DocumentTool::LayerTool(doc->Main());

    if (shapeTool.IsNull()) return false;

    // Get all free (root-level) shapes
    TDF_LabelSequence freeShapes;
    shapeTool->GetFreeShapes(freeShapes);

    if (freeShapes.Length() == 0) return false;

    // For single root shape, extract directly; for multiple, use the
    // underlying basic reader to combine them (same as OneShape behavior)
    if (freeShapes.Length() == 1) {
        TDF_Label rootLabel = freeShapes.Value(1);
        outShape = shapeTool->GetShape(rootLabel);
        outMeta = extractMetadata(rootLabel, colorTool, layerTool);
    } else {
        // Multiple roots — get combined shape from the basic reader
        outShape = cafReader.ChangeReader().OneShape();

        // Extract metadata from first root (best effort)
        TDF_Label firstLabel = freeShapes.Value(1);
        outMeta = extractMetadata(firstLabel, colorTool, layerTool);
    }

    return !outShape.IsNull();
}

// Import using basic STEPControl_Reader (fallback, no metadata).
bool importBasicFallback(const std::string& path, TopoDS_Shape& outShape) {
    STEPControl_Reader reader;
    IFSelect_ReturnStatus status = reader.ReadFile(path.c_str());
    if (status != IFSelect_RetDone) return false;

    int rootCount = reader.NbRootsForTransfer();
    if (rootCount == 0) return false;

    reader.TransferRoots();
    outShape = reader.OneShape();
    return !outShape.IsNull();
}

// Export using XCAF writer (full metadata).
bool exportWithXcaf(const std::vector<NamedShape>& shapes,
                    const std::vector<StepShapeMetadata>& metadata,
                    const std::string& path) {
    Handle(TDocStd_Document) doc = createXcafDocument();
    if (doc.IsNull()) return false;

    Handle(XCAFDoc_ShapeTool) shapeTool = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    Handle(XCAFDoc_ColorTool) colorTool = XCAFDoc_DocumentTool::ColorTool(doc->Main());
    Handle(XCAFDoc_LayerTool) layerTool = XCAFDoc_DocumentTool::LayerTool(doc->Main());

    if (shapeTool.IsNull()) return false;

    for (size_t i = 0; i < shapes.size(); ++i) {
        if (shapes[i].isNull()) continue;

        TDF_Label label = shapeTool->AddShape(shapes[i].shape());

        // Apply metadata if provided
        if (i < metadata.size()) {
            const auto& meta = metadata[i];

            // Name
            if (!meta.name.empty()) {
                TDataStd_Name::Set(label, TCollection_ExtendedString(meta.name.c_str(), Standard_True));
            }

            // Color
            if (meta.hasColor) {
                colorTool->SetColor(label, meta.color, XCAFDoc_ColorSurf);
            }

            // Layer
            if (!meta.layerName.empty()) {
                layerTool->SetLayer(label,
                    TCollection_ExtendedString(meta.layerName.c_str(), Standard_True));
            }
        }
    }

    STEPCAFControl_Writer cafWriter;
    cafWriter.SetColorMode(Standard_True);
    cafWriter.SetNameMode(Standard_True);
    cafWriter.SetLayerMode(Standard_True);

    // NOTE: OCCT global settings must be set by the caller (exportStepFile)
    // which holds the OcctStaticGuard mutex. Do NOT acquire guard here.

    if (!cafWriter.Transfer(doc)) return false;

    IFSelect_ReturnStatus status = cafWriter.Write(path.c_str());
    return status == IFSelect_RetDone;
}

// Export using basic STEPControl_Writer (fallback, no metadata).
bool exportBasicFallback(const std::vector<NamedShape>& shapes, const std::string& path) {
    STEPControl_Writer writer;
    // NOTE: OCCT global settings set by caller (exportStepFile) which holds mutex.

    for (auto& ns : shapes) {
        if (ns.isNull()) continue;
        IFSelect_ReturnStatus status = writer.Transfer(ns.shape(), STEPControl_AsIs);
        if (status != IFSelect_RetDone) return false;
    }

    IFSelect_ReturnStatus status = writer.Write(path.c_str());
    return status == IFSelect_RetDone;
}

} // anonymous namespace

// ── Import from memory buffer ────────────────────────────────────────────────

OperationResult<StepImportResult> importStep(KernelContext& ctx, const uint8_t* data, size_t len) {
    DiagnosticScope scope(ctx);

    if (!data || len == 0) {
        ctx.diag().error(ErrorCode::INVALID_INPUT, "Empty STEP data buffer");
        return scope.makeFailure<StepImportResult>();
    }

    // OCCT's STEP readers require a file path.
    // Write to a temp file, import, then clean up.
    TempFile tmp("oreo_import", ".step");
    {
        std::ofstream ofs(tmp.path, std::ios::binary);
        if (!ofs) {
            ctx.diag().error(ErrorCode::STEP_IMPORT_FAILED,
                         "Failed to create temp file for STEP import");
            return scope.makeFailure<StepImportResult>();
        }
        ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
    }

    return importStepFile(ctx, tmp.path);
}

// ── Import from file path ────────────────────────────────────────────────────

OperationResult<StepImportResult> importStepFile(KernelContext& ctx, const std::string& path) {
    DiagnosticScope scope(ctx);

    if (path.empty()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT, "Empty STEP file path");
        return scope.makeFailure<StepImportResult>();
    }

    // Scoped guard for OCCT global settings during import
    OcctStaticGuard importGuard;
    importGuard.set("xstep.cascade.unit", "MM");

    StepImportResult result;
    TopoDS_Shape shape;
    StepShapeMetadata meta;

    // ── Attempt 1: XCAF reader (colors, names, layers) ──────────────
    bool xcafOk = importWithXcaf(path, shape, meta);

    // ── Attempt 2: Basic reader fallback ────────────────────────────
    if (!xcafOk) {
        bool basicOk = importBasicFallback(path, shape);
        if (!basicOk) {
            ctx.diag().error(ErrorCode::STEP_IMPORT_FAILED,
                         "STEP import failed (both XCAF and basic readers): " + path,
                         {}, "Check file exists and is a valid STEP/AP214 file");
            return scope.makeFailure<StepImportResult>();
        }
        // Warn that metadata was lost
        ctx.diag().warning(ErrorCode::SHAPE_INVALID,
                     "STEP import fell back to basic reader — colors, names, and layers not imported",
                     "File geometry is intact but XDE metadata could not be extracted");
        meta = {};
    }

    if (shape.IsNull()) {
        ctx.diag().error(ErrorCode::STEP_IMPORT_FAILED,
                     "STEP import produced null shape from: " + path);
        return scope.makeFailure<StepImportResult>();
    }

    // Run ShapeFix pipeline on imported geometry
    if (!isShapeValid(shape)) {
        fixShape(shape);
    }

    auto tag = ctx.tags().nextShapeIdentity();
    auto map = initImportedElementMap(shape);
    result.shape = NamedShape(shape, map, tag);
    result.metadata = meta;
    return scope.makeResult(std::move(result));
}

// ── Legacy overloads (return NamedShape only) ────────────────────────────────

OperationResult<NamedShape> importStepShape(KernelContext& ctx, const uint8_t* data, size_t len) {
    auto result = importStep(ctx, data, len);
    if (!result.ok()) {
        return OperationResult<NamedShape>::failure(result.diagnostics());
    }
    return OperationResult<NamedShape>::success(std::move(result).value().shape, result.diagnostics());
}

OperationResult<NamedShape> importStepShapeFile(KernelContext& ctx, const std::string& path) {
    auto result = importStepFile(ctx, path);
    if (!result.ok()) {
        return OperationResult<NamedShape>::failure(result.diagnostics());
    }
    return OperationResult<NamedShape>::success(std::move(result).value().shape, result.diagnostics());
}

// ── Export to memory buffer ──────────────────────────────────────────────────

OperationResult<std::vector<uint8_t>> exportStep(KernelContext& ctx, const std::vector<NamedShape>& shapes,
                                const std::vector<StepShapeMetadata>& metadata) {
    DiagnosticScope scope(ctx);

    if (shapes.empty()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT, "No shapes to export");
        return scope.makeFailure<std::vector<uint8_t>>();
    }

    TempFile tmp("oreo_export", ".step");
    auto fileResult = exportStepFile(ctx, shapes, tmp.path, metadata);
    if (!fileResult.ok()) {
        return scope.makeFailure<std::vector<uint8_t>>();  // Error already set
    }

    // Read the file back into memory
    std::ifstream ifs(tmp.path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        ctx.diag().error(ErrorCode::STEP_EXPORT_FAILED, "Failed to read back exported STEP file");
        return scope.makeFailure<std::vector<uint8_t>>();
    }

    auto fileSize = ifs.tellg();
    if (fileSize <= 0) {
        ctx.diag().error(ErrorCode::STEP_EXPORT_FAILED, "Exported STEP file is empty");
        return scope.makeFailure<std::vector<uint8_t>>();
    }

    ifs.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(fileSize));
    ifs.read(reinterpret_cast<char*>(buf.data()), fileSize);
    return scope.makeResult(std::move(buf));
}

// ── Export to file path ──────────────────────────────────────────────────────

OperationResult<bool> exportStepFile(KernelContext& ctx, const std::vector<NamedShape>& shapes,
                    const std::string& path,
                    const std::vector<StepShapeMetadata>& metadata) {
    DiagnosticScope scope(ctx);

    if (shapes.empty()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT, "No shapes to export");
        return scope.makeFailure<bool>();
    }
    if (path.empty()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT, "Empty export file path");
        return scope.makeFailure<bool>();
    }

    // Scoped guard: locks OCCT global mutex, saves/restores Interface_Static settings
    OcctStaticGuard exportGuard;
    exportGuard.set("write.step.schema", "AP214");
    exportGuard.set("write.step.unit", "MM");
    exportGuard.set("xstep.cascade.unit", "MM");

    // ── Attempt 1: XCAF writer (preserves colors, names, layers) ────
    bool xcafOk = exportWithXcaf(shapes, metadata, path);

    if (xcafOk) return scope.makeResult(true);

    // ── Attempt 2: Basic writer fallback ────────────────────────────
    bool basicOk = exportBasicFallback(shapes, path);
    if (!basicOk) {
        ctx.diag().error(ErrorCode::STEP_EXPORT_FAILED,
                     "STEP export failed (both XCAF and basic writers): " + path,
                     {}, "Shape may be invalid — try running ShapeFix first. "
                         "Check write permissions for the target path.");
        return scope.makeFailure<bool>();
    }

    // Warn that metadata was lost
    ctx.diag().warning(ErrorCode::SHAPE_INVALID,
                 "STEP export fell back to basic writer — colors, names, and layers not written",
                 "File geometry is intact but XDE metadata could not be serialized");
    return scope.makeResult(true);
}

} // namespace oreo
