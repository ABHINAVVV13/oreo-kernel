// kernel_context.h — The central authority for all kernel operations.
//
// KernelContext replaces ALL global state in oreo-kernel:
//   - Global tolerance → ctx.tolerance
//   - Global error thread-local → ctx.diag
//   - Global nextTag() → ctx.tags
//   - Assumed units → ctx.units
//   - No schema versioning → ctx.schemas
//
// Every geometry operation, feature tree replay, and I/O operation
// takes a KernelContext& as its first parameter.
//
// Thread safety: CONTEXT_BOUND — one context per thread.
// Multiple threads can have separate contexts and operate independently.

#ifndef OREO_KERNEL_CONTEXT_H
#define OREO_KERNEL_CONTEXT_H

#include "tag_allocator.h"
#include "diagnostic.h"
#include "units.h"
#include "schema.h"
#include "thread_safety.h"

#include <memory>
#include <string>

namespace oreo {

// ─── TolerancePolicy ────────────────────────────────────────
// Moved here from oreo_tolerance.h — this is the canonical definition.

struct TolerancePolicy {
    double linearPrecision = 1e-7;     // General linear tolerance (meters)
    double angularPrecision = 1e-12;    // Angular tolerance (radians)
    double booleanFuzzyFactor = 1.0;    // Multiplier for auto-fuzzy boolean tolerance
    double minEdgeLength = 1e-6;        // Minimum edge length (below = degenerate)
};

// ─── Configuration for creating a context ────────────────────

struct KernelConfig {
    TolerancePolicy tolerance;      // Defaults from TolerancePolicy struct
    UnitSystem units;               // Defaults from UnitSystem struct
    int64_t tagSeed = 0;             // Starting tag counter value (0 = fresh)
    uint32_t documentId = 0;        // Document ID for cross-document tag uniqueness
                                    // 0 = single-document mode (tags are 1, 2, 3...)
                                    // nonzero = tags encode (docId << 32) | counter
    std::string documentUUID;       // If set, documentId is auto-computed from this
    bool initOCCT = true;           // Call OSD::SetSignal on first context
};

// ─── The context object ──────────────────────────────────────

class OREO_CONTEXT_BOUND KernelContext {
public:
    // ── Owned subsystems ─────────────────────────────────

    TagAllocator tags;              // Deterministic tag allocation
    DiagnosticCollector diag;       // Structured diagnostics
    TolerancePolicy tolerance;      // Per-context tolerance policy
    UnitSystem units;               // Per-context unit system
    SchemaRegistry schemas;         // Schema versioning + migration

    // ── Factory ──────────────────────────────────────────

    // Create a new context with the given configuration.
    static std::shared_ptr<KernelContext> create(const KernelConfig& config = {});

    // ── Lifecycle ────────────────────────────────────────

    // Initialize OCCT if not already done (called once per process).
    static void initOCCT();

    // Check if OCCT has been initialized.
    static bool isOCCTInitialized();

    // ── Context identity ─────────────────────────────────

    // Each context has a unique ID (for debugging/logging).
    const std::string& id() const { return id_; }

    // ── Convenience ──────────────────────────────────────

    // Clear diagnostics and prepare for a new operation.
    void beginOperation();

    // Check if the last operation succeeded (no errors).
    bool lastOperationOK() const { return !diag.hasErrors(); }

private:
    KernelContext() = default;
    explicit KernelContext(const KernelConfig& config);

    std::string id_;
};

// ─── Free functions ─────────────────────────────────────────

// Compute auto-fuzzy tolerance for boolean operations.
// Formula (from FreeCAD): factor * sqrt(bboxExtent) * Precision::Confusion()
// Uses ctx.tolerance.booleanFuzzyFactor.
double computeBooleanFuzzy(const KernelContext& ctx, double bboxSquareExtent);

} // namespace oreo

#endif // OREO_KERNEL_CONTEXT_H
