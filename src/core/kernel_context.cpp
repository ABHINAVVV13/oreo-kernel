// kernel_context.cpp — KernelContext implementation.
//
// OCCT init uses std::call_once for thread-safe one-time initialization.
// No process-global mutable state except the once_flag and a monotonic
// context counter used for human-readable id strings.

#include "kernel_context.h"

#include <OSD.hxx>
#include <Precision.hxx>

#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <sstream>

namespace oreo {

// ─── Process-global state (carefully bounded) ───────────────────

// Thread-safe one-time OCCT initialization.
static std::once_flag s_occtOnceFlag;
static std::atomic<bool> s_occtInitialized{false};

// Monotonic context counter for debug ids. Not persisted — only used
// for human-readable context identification within a single process
// lifetime. Widened to uint64_t per the context-layer audit.
static std::atomic<uint64_t> s_contextCounter{0};

// ─── Internal helpers ───────────────────────────────────────────

namespace {

// Defaults used when a field has to be clamped. Keeps the code DRY
// between the silent sanitizer and the reporting sanitizer.
constexpr TolerancePolicy kDefaultTolerance{};

bool isFiniteAndNonNegative(double v) noexcept {
    return std::isfinite(v) && v >= 0.0;
}

bool isFiniteAndPositive(double v) noexcept {
    return std::isfinite(v) && v > 0.0;
}

// Range-check a LengthUnit enum cast from its underlying type.
// Mirrors the valid values listed in units.h.
bool isValidLengthUnit(LengthUnit u) noexcept {
    switch (u) {
        case LengthUnit::Meter:
        case LengthUnit::Millimeter:
        case LengthUnit::Centimeter:
        case LengthUnit::Micrometer:
        case LengthUnit::Inch:
        case LengthUnit::Foot:
            return true;
    }
    return false;
}

bool isValidAngleUnit(AngleUnit u) noexcept {
    switch (u) {
        case AngleUnit::Radian:
        case AngleUnit::Degree:
            return true;
    }
    return false;
}

} // namespace

// ─── sanitizeConfig* helpers ────────────────────────────────────

KernelConfig KernelContext::sanitizeConfigSilent(const KernelConfig& in) noexcept {
    KernelConfig out = in;

    if (!isFiniteAndPositive(out.tolerance.linearPrecision))
        out.tolerance.linearPrecision = kDefaultTolerance.linearPrecision;

    if (!isFiniteAndPositive(out.tolerance.angularPrecision))
        out.tolerance.angularPrecision = kDefaultTolerance.angularPrecision;

    // booleanFuzzyFactor of 0 is legitimate (disable auto-fuzzy),
    // so >= 0 is allowed. Negative or non-finite is rejected.
    if (!isFiniteAndNonNegative(out.tolerance.booleanFuzzyFactor))
        out.tolerance.booleanFuzzyFactor = kDefaultTolerance.booleanFuzzyFactor;

    if (!isFiniteAndPositive(out.tolerance.minEdgeLength))
        out.tolerance.minEdgeLength = kDefaultTolerance.minEdgeLength;

    if (!isValidLengthUnit(out.units.documentLength))
        out.units.documentLength = LengthUnit::Millimeter;

    if (!isValidAngleUnit(out.units.documentAngle))
        out.units.documentAngle = AngleUnit::Degree;

    // UnitSystem in this codebase does not currently declare a
    // `massUnit` field (see units.h). The audit references one for
    // forward compatibility; if/when the field lands, extend the
    // matching validation here.

    return out;
}

void KernelContext::reportSanitization(const KernelConfig& raw,
                                       DiagnosticCollector& diag) {
    if (!isFiniteAndPositive(raw.tolerance.linearPrecision)) {
        diag.fatal(ErrorCode::INVALID_INPUT,
                   "KernelConfig.tolerance.linearPrecision must be finite and > 0; "
                   "clamping to default");
    }
    if (!isFiniteAndPositive(raw.tolerance.angularPrecision)) {
        diag.fatal(ErrorCode::INVALID_INPUT,
                   "KernelConfig.tolerance.angularPrecision must be finite and > 0; "
                   "clamping to default");
    }
    if (!isFiniteAndNonNegative(raw.tolerance.booleanFuzzyFactor)) {
        diag.fatal(ErrorCode::INVALID_INPUT,
                   "KernelConfig.tolerance.booleanFuzzyFactor must be finite and >= 0; "
                   "clamping to default");
    }
    if (!isFiniteAndPositive(raw.tolerance.minEdgeLength)) {
        diag.fatal(ErrorCode::INVALID_INPUT,
                   "KernelConfig.tolerance.minEdgeLength must be finite and > 0; "
                   "clamping to default");
    }
    if (!isValidLengthUnit(raw.units.documentLength)) {
        diag.fatal(ErrorCode::INVALID_INPUT,
                   "KernelConfig.units.documentLength is out of enum range; "
                   "clamping to Millimeter");
    }
    if (!isValidAngleUnit(raw.units.documentAngle)) {
        diag.fatal(ErrorCode::INVALID_INPUT,
                   "KernelConfig.units.documentAngle is out of enum range; "
                   "clamping to Degree");
    }
}

KernelConfig KernelContext::sanitizeConfig(const KernelConfig& in,
                                           DiagnosticCollector& diag) {
    // Public API path: report + clamp in one pass. Used by external
    // callers that want to pre-validate a config before create().
    reportSanitization(in, diag);
    return sanitizeConfigSilent(in);
}

// ─── Constructor ────────────────────────────────────────────────

KernelContext::KernelContext(const KernelConfig& config)
    // Member initializer list. Declaration order (matches the header):
    //   tags_, diag_, tolerance_, units_, quotas_, schemas_,
    //   schemasFrozen_, cancellationToken_, progressCb_, id_, numericId_
    //
    // tolerance_ and units_ are const; they must be built from a
    // sanitized copy of the input. The silent sanitizer is pure, so
    // calling it twice is harmless — the diagnostics for clamping are
    // emitted exactly once in the ctor body below via
    // reportSanitization.
    : tags_()
    , diag_()
    , tolerance_(sanitizeConfigSilent(config).tolerance)
    , units_(sanitizeConfigSilent(config).units)
    , quotas_(config.quotas)
    , schemas_()
    , schemasFrozen_(false)
    , cancellationToken_()
    , progressCb_()
    , id_()
    , numericId_(0)
{
    // 1. Report sanitization diagnostics into the now-live diag_.
    reportSanitization(config, diag_);

    // 2. Assign a human-readable id + numeric id.
    numericId_ = ++s_contextCounter;
    {
        std::ostringstream ss;
        ss << "ctx-" << numericId_;
        id_ = ss.str();
    }

    // 3. Thread through the context id to the diagnostic collector so
    //    multi-context logs can be correlated.
    diag_.setContextId(numericId_);

    // 4. Apply quota-driven runtime limits.
    applyQuotas();

    // 5. Tag allocator setup.
    uint64_t docId = config.documentId;
    if (docId == 0 && !config.documentUUID.empty()) {
        docId = TagAllocator::documentIdFromString(config.documentUUID);
    }
    if (docId != 0) {
        tags_.setDocumentId(docId);
    }

    // 6. tagSeed validation: negative is a user error (we used to
    //    silently skip); zero is an explicit seed; positive is also
    //    valid.
    if (config.tagSeed < 0) {
        diag_.error(ErrorCode::INVALID_INPUT,
                    "KernelConfig.tagSeed is negative; tag allocator left unseeded",
                    "Pass tagSeed >= 0");
    } else if (config.tagSeed == 0) {
        tags_.seed(0);
    } else {
        tags_.seed(config.tagSeed);
    }

    // 7. Optional OCCT init.
    if (config.initOCCT) {
        initOCCT();
    }
}

void KernelContext::applyQuotas() {
    // maxDiagnostics is the only quota the context itself enforces at
    // construction. Other fields are advisory and consulted by
    // subsystems. A value of 0 means "unlimited"; translate to the
    // sentinel the diagnostic collector expects (size_t max).
    const size_t cap = quotas_.maxDiagnostics == 0
        ? (std::numeric_limits<size_t>::max)()
        : static_cast<size_t>(quotas_.maxDiagnostics);
    diag_.setMaxDiagnostics(cap);
}

// ─── Factory ────────────────────────────────────────────────────

std::shared_ptr<KernelContext> KernelContext::create(const KernelConfig& config) {
    // KernelContext has deleted copy/move due to reference members, so
    // we can't use std::make_shared-from-value; new + shared_ptr is the
    // idiomatic form for private ctors.
    return std::shared_ptr<KernelContext>(new KernelContext(config));
}

// ─── OCCT lifecycle ─────────────────────────────────────────────

void KernelContext::initOCCT() {
    std::call_once(s_occtOnceFlag, []() {
        OSD::SetSignal(false);
        s_occtInitialized.store(true, std::memory_order_release);
    });
}

bool KernelContext::isOCCTInitialized() {
    return s_occtInitialized.load(std::memory_order_acquire);
}

// ─── Schema registry access ─────────────────────────────────────

SchemaRegistry& KernelContext::mutableSchemas() {
    if (schemasFrozen_) {
        // Diagnostic is an early-warning signal — actual mutation attempts
        // on the returned reference will throw std::logic_error from the
        // registry itself (see SchemaRegistry::registerMigration /
        // unregisterMigration / setMaxMigrationSteps).
        diag_.error(ErrorCode::INVALID_STATE,
                    "SchemaRegistry is frozen; mutations on the returned "
                    "reference will throw std::logic_error",
                    "Perform all migration registration before freezeSchemas()");
    }
    return schemas_;
}

// ─── Cancellation ───────────────────────────────────────────────

CancellationTokenPtr KernelContext::cancellationToken() {
    if (!cancellationToken_) {
        cancellationToken_ = std::make_shared<CancellationToken>();
    }
    return cancellationToken_;
}

bool KernelContext::checkCancellation() {
    if (cancellationToken_ && cancellationToken_->isCancelled()) {
        diag_.error(ErrorCode::CANCELLED, "Operation cancelled");
        return true;
    }
    return false;
}

// ─── Progress reporting ─────────────────────────────────────────

void KernelContext::reportProgress(double fraction, const std::string& phase) {
    if (!progressCb_) return;

    // Clamp to [0, 1]; NaN becomes 0.
    double clamped;
    if (std::isnan(fraction) || fraction < 0.0) {
        clamped = 0.0;
    } else if (fraction > 1.0) {
        clamped = 1.0;
    } else {
        clamped = fraction;
    }

    // Callbacks are best-effort. Any exception from user code must not
    // propagate into a geometry operation and corrupt its state.
    try {
        progressCb_(clamped, phase);
    } catch (...) {
        diag_.warning(ErrorCode::INTERNAL_ERROR,
                      "Progress callback threw an exception; suppressed",
                      "Ensure progress callbacks do not throw");
    }
}

// ─── clone / merge / safeReset ──────────────────────────────────

std::unique_ptr<KernelContext> KernelContext::clone() const {
    // Rebuild a KernelConfig from this context's current (already
    // validated) policy so the fresh context constructs cleanly
    // without emitting duplicate sanitization diagnostics.
    KernelConfig cfg;
    cfg.tolerance  = tolerance_;
    cfg.units      = units_;
    cfg.tagSeed    = tags_.currentValue();     // preserve counter
    cfg.documentId = tags_.documentId();
    cfg.initOCCT   = false;                    // already done by now
    cfg.quotas     = quotas_;

    std::unique_ptr<KernelContext> copy(new KernelContext(cfg));

    // Copy schema registry state and frozen flag. (Assignment uses the
    // SchemaRegistry's implicit copy assignment over its value-type
    // members: map of versions + vector of migrations.)
    copy->schemas_       = schemas_;
    copy->schemasFrozen_ = schemasFrozen_;

    // Diagnostic snapshot: append every diagnostic this context holds.
    // DiagnosticCollector::report() copies its argument, so this is a
    // faithful shallow snapshot.
    for (const auto& d : diag_.all()) {
        copy->diag_.report(d);
    }

    // Cancellation token and progress callback are NOT copied — each
    // context owns its own cancellation/progress wiring. Callers that
    // want to share a token can use setCancellationToken() post-clone.
    return copy;
}

void KernelContext::merge(const KernelContext& other) {
    // Copy the other context's diagnostic snapshot into this one.
    // Deliberately avoids touching tolerance_ / units_ / schemas_ /
    // tags_ — merge() is only for accumulating reports from
    // sub-operations that ran on a scratch context.
    for (const auto& d : other.diag_.all()) {
        diag_.report(d);
    }
}

void KernelContext::safeReset() {
    // Clear transient state only. tolerance_ / units_ are const.
    // schemas_ / tags_ / quotas_ are preserved intentionally — they
    // carry structural state that a reset should not erase.
    diag_.clear();
    if (cancellationToken_) {
        cancellationToken_->reset();
    }
    // Re-apply the diagnostic buffer cap in case clear() reset it.
    applyQuotas();
    // Re-thread the context id (clear() may have reset it per the
    // diagnostic-layer contract — defensive re-install).
    diag_.setContextId(numericId_);
}

// ─── computeBooleanFuzzy ────────────────────────────────────────

double computeBooleanFuzzy(KernelContext& ctx, double bboxSquareExtent) {
    if (!std::isfinite(bboxSquareExtent) || bboxSquareExtent < 0.0) {
        ctx.diag().warning(
            ErrorCode::INVALID_STATE,
            "Non-finite bbox extent; falling back to default fuzzy",
            "Check input geometry for NaN/Inf coordinates");
        return 0.0;
    }
    return ctx.tolerance().booleanFuzzyFactor
           * std::sqrt(bboxSquareExtent)
           * Precision::Confusion();
}

double computeBooleanFuzzy(const KernelContext& ctx, double bboxSquareExtent) {
    // Const overload: no diagnostic sink available, so we fall back to
    // the silent pre-audit behaviour. Callers that want the warning
    // must pass a non-const reference.
    if (!std::isfinite(bboxSquareExtent) || bboxSquareExtent < 0.0) return 0.0;
    return ctx.tolerance().booleanFuzzyFactor
           * std::sqrt(bboxSquareExtent)
           * Precision::Confusion();
}

} // namespace oreo
