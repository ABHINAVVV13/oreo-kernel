// kernel_context.h — The central authority for all kernel operations.
//
// KernelContext replaces ALL global state in oreo-kernel:
//   - Global tolerance → ctx.tolerance()
//   - Global error thread-local → ctx.diag()
//   - Global nextTag() → ctx.tags()
//   - Assumed units → ctx.units()
//   - No schema versioning → ctx.schemas()
//
// Every geometry operation, feature tree replay, and I/O operation
// takes a KernelContext& as its first parameter.
//
// Thread safety: CONTEXT_BOUND — one context per thread.
// Multiple threads can have separate contexts and operate independently.
//
// Breaking change (P0 audit): the formerly-public data members `tags`
// and `diag` have been replaced by accessor functions `tags()` /
// `diag()`. C++ does not allow a data member and a member function to
// share a name, so the deprecated-alias approach was not feasible.
// Callers must migrate `ctx.tags.X` → `ctx.tags().X` and `ctx.diag.X`
// → `ctx.diag().X`. This is a mechanical rename.

#ifndef OREO_KERNEL_CONTEXT_H
#define OREO_KERNEL_CONTEXT_H

#include "tag_allocator.h"
#include "diagnostic.h"
#include "units.h"
#include "schema.h"
#include "thread_safety.h"
#include "cancellation.h"
#include "progress.h"
#include "resource_quotas.h"

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
    uint64_t documentId = 0;        // Document ID for cross-document tag uniqueness
                                    // 0 = single-document mode (tags are 1, 2, 3...)
                                    // nonzero = multi-document mode (SipHash-derived, uint64)
    std::string documentUUID;       // If set, documentId is auto-computed from this
    bool initOCCT = true;           // Call OSD::SetSignal on first context
    ResourceQuotas quotas;          // Per-context resource limits (defaults = lightly bounded)
};

// ─── The context object ──────────────────────────────────────

class OREO_CONTEXT_BOUND KernelContext {
public:
    // ── Core subsystem accessors ─────────────────────────
    //
    // `tags()` and `diag()` replace the previously-public data members
    // of the same name. Callers must migrate from member access
    // (`ctx.tags.X`) to function call (`ctx.tags().X`) — the audit
    // authorized this breaking change because C++ does not allow a
    // data member and a member function to share a name.

    TagAllocator&              tags()       noexcept { return tags_; }
    const TagAllocator&        tags() const noexcept { return tags_; }
    DiagnosticCollector&       diag()       noexcept { return diag_; }
    const DiagnosticCollector& diag() const noexcept { return diag_; }

    // ── Read-only policy access ──────────────────────────

    const TolerancePolicy& tolerance() const noexcept { return tolerance_; }
    const UnitSystem&      units()     const noexcept { return units_; }
    const ResourceQuotas&  quotas()    const noexcept { return quotas_; }

    // ── Schema registry access ───────────────────────────
    //
    // schemas() is always available (returns const). mutableSchemas()
    // is a diagnostic-enhanced accessor. Registry-level enforcement is
    // authoritative: after freezeSchemas() has been called, every
    // mutator on SchemaRegistry (registerMigration, unregisterMigration,
    // setMaxMigrationSteps) throws std::logic_error regardless of which
    // accessor returned the reference — the legacy non-const schemas()
    // overload does NOT provide a mutation bypass.

    const SchemaRegistry& schemas() const noexcept { return schemas_; }

    // Legacy mutable overload: kept for source compatibility. Returns a
    // mutable reference but mutations fail (throw) after freezeSchemas().
    SchemaRegistry& schemas() noexcept { return schemas_; }

    // Preferred mutable accessor. Reports a diagnostic in addition to the
    // registry-level enforcement, so callers notice the misuse before the
    // throw. Returns a reference so the signature can be used in
    // expressions that assume non-null semantics (but calls on the frozen
    // registry will still throw).
    SchemaRegistry& mutableSchemas();

    // Lock the schema registry against further mutation. Idempotent.
    // Delegates to SchemaRegistry::freeze() so registry-level mutators
    // enforce the lock too, not just the context-level accessor check.
    void freezeSchemas() noexcept {
        schemasFrozen_ = true;
        schemas_.freeze();
    }

    // Query the frozen state.
    bool schemasFrozen() const noexcept { return schemasFrozen_; }

    // ── Cancellation ─────────────────────────────────────
    //
    // A context always has an associated cancellation token. It is
    // lazily created on first access so that contexts that never use
    // cancellation don't pay the allocation cost.

    CancellationTokenPtr cancellationToken();

    // Replace the current cancellation token (useful for wiring a
    // single UI-owned token into multiple worker contexts).
    void setCancellationToken(CancellationTokenPtr token) noexcept {
        cancellationToken_ = std::move(token);
    }

    // Return true iff the token is set and cancelled. On cancellation,
    // reports an Error diagnostic (code CANCELLED) so that surrounding
    // code paths can observe it through the diagnostic stream in
    // addition to the boolean return value.
    bool checkCancellation();

    // ── Progress reporting ───────────────────────────────

    void setProgressCallback(ProgressCallback cb) noexcept {
        progressCb_ = std::move(cb);
    }
    bool hasProgressCallback() const noexcept {
        return static_cast<bool>(progressCb_);
    }

    // Invoke the progress callback if set. `fraction` is clamped to
    // [0, 1] before forwarding. Safe to call even when no callback
    // has been attached (no-op). Any exception thrown by the callback
    // is swallowed — progress reporting is best-effort and must never
    // disrupt a geometry operation.
    void reportProgress(double fraction, const std::string& phase);

    // ── Factory ──────────────────────────────────────────

    // Create a new context with the given configuration.
    static std::shared_ptr<KernelContext> create(const KernelConfig& config = {});

    // ── Copy / reset / merge ─────────────────────────────

    // Deep-ish copy: duplicates config/tolerance/units/quotas,
    // tag allocator state (counter + docId), schema registry, and a
    // snapshot of the diagnostic buffer. The progress callback and
    // cancellation token are deliberately NOT copied — each context
    // owns its own cancellation/progress wiring.
    std::unique_ptr<KernelContext> clone() const;

    // Append diagnostics from `other` into this context's collector.
    // Does not touch tolerance, units, tags, schemas, or callbacks.
    void merge(const KernelContext& other);

    // Clear transient state (diagnostics, cancellation flag) while
    // preserving the immutable policy (tolerance, units, schemas,
    // tags). Quotas, progress callback and cancellation-token owner
    // pointer are preserved; only the cancellation flag is reset.
    void safeReset();

    // ── Lifecycle ────────────────────────────────────────

    // Initialize OCCT if not already done (called once per process).
    static void initOCCT();

    // Check if OCCT has been initialized.
    static bool isOCCTInitialized();

    // ── Context identity ─────────────────────────────────

    // Each context has a unique ID (for debugging/logging).
    const std::string& id() const noexcept { return id_; }

    // Numeric form of the context ID (monotonic within a process).
    uint64_t numericId() const noexcept { return numericId_; }

    // ── Config validation ────────────────────────────────

    // Validate + clamp a KernelConfig, reporting a Fatal diagnostic
    // for each field that had to be clamped. Public so that callers
    // can pre-check a config against a scratch DiagnosticCollector
    // before handing it to create(). The primary ctor uses this path
    // internally as well.
    static KernelConfig sanitizeConfig(const KernelConfig& config,
                                       DiagnosticCollector& diag);

private:
    // Primary constructor. Accepts a raw KernelConfig; validates and
    // clamps it, records diagnostics for any clamping into diag_ once
    // the member is live.
    explicit KernelContext(const KernelConfig& config);

    // Pure sanitization helper — returns a clamped config without
    // emitting diagnostics. Used by the initializer list to populate
    // the const `tolerance_` / `units_` members. Safe to invoke
    // multiple times on the same input.
    static KernelConfig sanitizeConfigSilent(const KernelConfig& config) noexcept;

    // Diagnostic-producing sanitization helper — called once from the
    // ctor body after diag_ is live. Reports a Fatal diagnostic for
    // each field that would have been clamped.
    static void reportSanitization(const KernelConfig& raw,
                                   DiagnosticCollector& diag);

    // Apply the quotas that have a direct runtime effect (currently
    // just maxDiagnostics, which caps the DiagnosticCollector buffer).
    void applyQuotas();

    // ── Data ─────────────────────────────────────────────
    //
    // Declaration order matters: tags_ and diag_ appear first so that
    // later members (and the ctor body) can reference them freely.

    TagAllocator          tags_;
    DiagnosticCollector   diag_;
    const TolerancePolicy tolerance_;   // Immutable after construction.
    const UnitSystem      units_;       // Immutable after construction.
    ResourceQuotas        quotas_;
    SchemaRegistry        schemas_;
    bool                  schemasFrozen_ = false;

    CancellationTokenPtr  cancellationToken_;
    ProgressCallback      progressCb_;

    std::string id_;
    uint64_t    numericId_ = 0;
};

// ─── Free functions ─────────────────────────────────────────

// Compute auto-fuzzy tolerance for boolean operations.
// Formula (from FreeCAD): factor * sqrt(bboxExtent) * Precision::Confusion()
// Uses ctx.tolerance().booleanFuzzyFactor.
//
// Takes non-const ctx so that a warning can be routed to its diagnostic
// collector when the bbox is non-finite (previously this was silent).
double computeBooleanFuzzy(KernelContext& ctx, double bboxSquareExtent);

// Backward-compatible const overload: non-finite inputs are still
// handled, but no diagnostic is emitted (nowhere to route it).
double computeBooleanFuzzy(const KernelContext& ctx, double bboxSquareExtent);

} // namespace oreo

#endif // OREO_KERNEL_CONTEXT_H
