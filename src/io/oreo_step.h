// oreo_step.h — STEP import/export with XDE metadata (colors, names, layers).
//
// Uses STEPCAFControl_Reader/Writer + XCAF document model to preserve
// extended data through round-trips. Falls back gracefully if XDE
// processing fails — geometry is always returned even if metadata is lost.
//
// Every operation takes a KernelContext& as its first parameter.
// Every operation takes a KernelContext& as its first parameter.

#ifndef OREO_STEP_H
#define OREO_STEP_H

#include "core/kernel_context.h"
#include "core/operation_result.h"
#include "core/diagnostic_scope.h"
#include "naming/named_shape.h"

#include <Quantity_Color.hxx>

#include <cstdint>
#include <string>
#include <vector>

namespace oreo {

// Per-shape metadata extracted from STEP XDE document.
struct StepShapeMetadata {
    std::string name;                   // Part/product name from STEP
    bool        hasColor = false;
    Quantity_Color color;               // RGB color (valid only if hasColor)
    std::string layerName;              // Layer assignment (empty if none)
};

// Result of importing a STEP file — shape + optional metadata.
struct StepImportResult {
    NamedShape shape;
    StepShapeMetadata metadata;

    bool isNull() const { return shape.isNull(); }
};

// ═══════════════════════════════════════════════════════════════
// Context-aware STEP I/O functions
// ═══════════════════════════════════════════════════════════════

// Import a STEP file from a memory buffer.
// Uses XCAF reader for color/name/layer extraction.
// Runs ShapeFix pipeline on the imported shape.
OperationResult<StepImportResult> importStep(KernelContext& ctx, const uint8_t* data, size_t len);

// Import a STEP file from a file path.
OperationResult<StepImportResult> importStepFile(KernelContext& ctx, const std::string& path);

// Legacy overloads — return NamedShape only (discard metadata).
OperationResult<NamedShape> importStepShape(KernelContext& ctx, const uint8_t* data, size_t len);
OperationResult<NamedShape> importStepShapeFile(KernelContext& ctx, const std::string& path);

// Export one or more shapes to STEP format with optional metadata.
// Returns the STEP file as a byte buffer.
OperationResult<std::vector<uint8_t>> exportStep(KernelContext& ctx, const std::vector<NamedShape>& shapes,
                                const std::vector<StepShapeMetadata>& metadata = {});

// Export to a file path with optional metadata.
OperationResult<bool> exportStepFile(KernelContext& ctx, const std::vector<NamedShape>& shapes,
                    const std::string& path,
                    const std::vector<StepShapeMetadata>& metadata = {});

} // namespace oreo

#endif // OREO_STEP_H
