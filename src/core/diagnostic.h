// SPDX-License-Identifier: LGPL-2.1-or-later

// diagnostic.h — Structured diagnostics system for oreo-kernel.
//
// Replaces the old thread-local single-error system with:
//   - Multiple diagnostics per operation (errors + warnings + info)
//   - Severity levels (Debug, Trace, Info, Warning, Error, Fatal)
//   - Degraded-geometry and degraded-naming flags
//   - Feature and element reference context
//   - Serializable for persistence with rebuild state
//   - JSON round-trip via nlohmann::json
//   - Optional pluggable sinks (see logging_sink.h)
//
// DiagnosticCollector is CONTEXT-BOUND — each KernelContext has its own.

#ifndef OREO_DIAGNOSTIC_H
#define OREO_DIAGNOSTIC_H

#pragma once

#include "thread_safety.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace oreo {

class IDiagnosticSink;  // Forward decl — see logging_sink.h

// ─── ErrorCode ──────────────────────────────────────────────
// Moved here from oreo_error.h — this is the canonical definition.
//
// Every enumerator is pinned to an explicit integer so that serialized
// diagnostics remain comparable across kernel versions. Values leave
// gaps of ~10 per block so that new codes can be added without shifting
// the existing surface. NEVER repurpose or remove an existing value.

enum class ErrorCode {
    OK = 0,
    INVALID_INPUT        = 1,   // null shape, zero-length vector, etc.
    OCCT_FAILURE         = 2,   // OCCT operation returned !IsDone()
    BOOLEAN_FAILED       = 3,   // Boolean operation failed (common with near-degenerate geometry)
    SHAPE_INVALID        = 4,   // BRepCheck_Analyzer says the result is invalid
    SHAPE_FIX_FAILED     = 5,   // ShapeFix was attempted but couldn't repair the shape
    SKETCH_SOLVE_FAILED  = 6,   // PlaneGCS solver returned non-convergent
    SKETCH_REDUNDANT     = 7,   // Sketch has redundant constraints
    SKETCH_CONFLICTING   = 8,   // Sketch has conflicting constraints
    // gap for future geometry codes: 9
    STEP_IMPORT_FAILED   = 10,  // STEP file could not be read
    STEP_EXPORT_FAILED   = 11,  // STEP file could not be written
    SERIALIZE_FAILED     = 12,  // Binary serialization failed
    DESERIALIZE_FAILED   = 13,  // Binary deserialization failed
    // Identity v2 new codes (block at 14; see docs/identity-model.md §8 Q2).
    // Kept in the I/O block because all four surface at serialization or
    // name-parsing boundaries.
    LEGACY_IDENTITY_DOWNGRADE    = 14, // Warning: v1 tag read, high docId bits inferred or lost
    V2_IDENTITY_NOT_REPRESENTABLE = 15, // Error: v2 identity with counter > UINT32_MAX can't fit in v1/v2 format
    MALFORMED_ELEMENT_NAME       = 16, // Warning: element name had unparseable ;:P / ;:H payload
    BUFFER_TOO_SMALL             = 17, // Error: size-probe buffer was too small for the output
    // gap for future I/O codes: 18-19
    NOT_INITIALIZED      = 20,  // oreo_init() was not called
    INTERNAL_ERROR       = 21,  // Bug in oreo-kernel
    // gap for future internal-state codes: 22-29

    // ── D-6: General-purpose codes (block at 100) ──────────
    NOT_SUPPORTED        = 100, // Operation not implemented for this input
    INVALID_STATE        = 101, // Caller invoked API in the wrong state
    OUT_OF_RANGE         = 102, // Numeric or index argument out of range
    TIMEOUT              = 103, // Operation exceeded its deadline
    CANCELLED            = 104, // Operation was cancelled by the caller
    RESOURCE_EXHAUSTED   = 105, // Memory, handles, or other quota exhausted
    // gap for future general codes: 106-109

    // ── Internal markers (block at 200) ────────────────────
    DIAG_TRUNCATED       = 200, // Emitted when the collector hits maxDiagnostics_
};

// Free helper — returns a short static string for the given code.
// Never returns nullptr; unknown values return "UNKNOWN".
const char* errorCodeToString(ErrorCode code);

// ─── Severity ────────────────────────────────────────────────
// CC-13: Debug/Trace sit below Info for verbose structured logging.

enum class Severity {
    Debug   = 0,  // Very verbose — feature-level tracing
    Trace   = 1,  // Fine-grained step tracing (below Info)
    Info    = 2,  // Informational — no action needed
    Warning = 3,  // Non-fatal issue — result is usable but may be degraded
    Error   = 4,  // Operation failed — no valid result
    Fatal   = 5,  // Unrecoverable — kernel state may be corrupt
};

// ─── Diagnostic ──────────────────────────────────────────────

struct Diagnostic {
    // D-8: default to Info so a freshly constructed Diagnostic is benign.
    Severity severity = Severity::Info;
    ErrorCode code = ErrorCode::OK;     // Specific error code
    std::string message;                // Human-readable description
    std::string featureId;              // Which feature produced this (empty if not in feature context)
    // D-11: Format: "type#tag" or "tag" as decimal string; empty if not applicable.
    std::string elementRef;             // Which element reference failed (empty if not applicable)
    std::string suggestion;             // Actionable recovery suggestion

    // Quality flags
    bool geometryDegraded = false;      // Shape is geometrically valid but quality is reduced
                                        // (e.g., ShapeFix healed a broken result)
    bool namingDegraded = false;        // Element names may be unstable
                                        // (e.g., NullMapper fallback was used)

    // D-9: Automatic metadata stamped in DiagnosticCollector::report().
    // Callers MAY set these explicitly (e.g. when replaying persisted diagnostics);
    // otherwise the collector fills them on report() when the fields are still 0.
    uint64_t timestampNs = 0;           // std::chrono::system_clock epoch, nanoseconds
    uint64_t threadId = 0;              // Hashed std::this_thread::get_id()
    uint64_t contextId = 0;             // Collector-wide ID (see setContextId)

    // Convenience constructors
    static Diagnostic info(ErrorCode code, const std::string& msg);
    static Diagnostic warning(ErrorCode code, const std::string& msg,
                              const std::string& suggestion = {});
    static Diagnostic error(ErrorCode code, const std::string& msg,
                            const std::string& suggestion = {});
    static Diagnostic fatal(ErrorCode code, const std::string& msg);
    // D-4: fatal overload with suggestion (parallel to error())
    static Diagnostic fatal(ErrorCode code, const std::string& msg,
                            const std::string& suggestion);

    // D-7 / CC-12: JSON round-trip.
    nlohmann::json toJSON() const;
    static Diagnostic fromJSON(const nlohmann::json& j);
};

// ─── DiagnosticCollector ─────────────────────────────────────
//
// Thread-safety: OREO_CONTEXT_BOUND (one collector per KernelContext, used by
// exactly one worker thread at a time). Internal threadId stamping assumes
// callers respect that contract.

class OREO_CONTEXT_BOUND DiagnosticCollector {
public:
    DiagnosticCollector();

    // Report a diagnostic. Populates timestampNs / threadId / contextId on
    // the incoming value if they are still zero, then stores it and forwards
    // to every registered sink. Safe against std::bad_alloc: on allocation
    // failure the diagnostic is written to a per-thread last-resort ring
    // buffer and overflowCount_ is incremented instead of propagating.
    void report(Diagnostic diag);

    // Report convenience overloads
    void info(ErrorCode code, const std::string& msg);
    void warning(ErrorCode code, const std::string& msg,
                 const std::string& suggestion = {});
    void error(ErrorCode code, const std::string& msg,
               const std::string& suggestion = {});
    void error(ErrorCode code, const std::string& msg,
               const std::string& entityRef, const std::string& suggestion);
    void fatal(ErrorCode code, const std::string& msg);
    // D-4: fatal overload with suggestion
    void fatal(ErrorCode code, const std::string& msg, const std::string& suggestion);

    // Clear all diagnostics. Increments the generation counter so that
    // any live DiagnosticScope detects invalidation. Also resets incremental
    // counters; does NOT clear registered sinks or reset the context ID.
    void clear();

    // Generation counter — incremented on every clear(). Used by
    // DiagnosticScope to detect if its startIndex_ was invalidated.
    // DS-1: widened to uint64_t to make overflow effectively impossible.
    uint64_t scopeGeneration() const { return generation_; }

    // ── Query ────────────────────────────────────────────

    bool empty() const { return diagnostics_.empty(); }
    // D-3 / OR-5: these are now O(1) — backed by incremental counters
    // maintained in report() and reset in clear().
    bool hasErrors() const { return (errorCount_ + fatalCount_) > 0; }
    bool hasWarnings() const { return warningCount_ > 0; }
    bool hasFatal() const { return fatalCount_ > 0; }
    bool isGeometryDegraded() const { return geometryDegraded_; }
    bool isNamingDegraded() const { return namingDegraded_; }

    // Get all diagnostics.
    //
    // D-2 WARNING: the returned reference may be invalidated by any subsequent
    // call to report() or clear() on this collector. Callers that need a
    // stable view across mutation MUST use snapshot() instead.
    const std::vector<Diagnostic>& all() const { return diagnostics_; }

    // D-2: stable copy of the current diagnostic vector. Safe to retain
    // across later report() / clear() calls.
    std::vector<Diagnostic> snapshot() const { return diagnostics_; }

    // Filter by severity
    std::vector<Diagnostic> errors() const;
    std::vector<Diagnostic> warnings() const;

    // D-1: Safe last-error accessor — returns by value; never dangles.
    // Prefer this over the deprecated pointer-returning overload.
    std::optional<Diagnostic> lastErrorOpt() const;

    // D-1: Legacy pointer API. Kept for backward compatibility but marked
    // deprecated — the returned pointer dangles as soon as report() or
    // clear() runs.  Use lastErrorOpt() instead.
    [[deprecated("lastError() returns a pointer into the internal vector; "
                 "it dangles after report()/clear(). Use lastErrorOpt().")]]
    const Diagnostic* lastError() const;

    // Count
    int count() const { return static_cast<int>(diagnostics_.size()); }
    // D-3: now O(1).
    int errorCount() const { return errorCount_ + fatalCount_; }
    int warningCount() const { return warningCount_; }
    int fatalCount() const { return fatalCount_; }

    // D-9: Context ID stamped into every reported diagnostic.
    void setContextId(uint64_t contextId) { contextId_ = contextId; }
    uint64_t contextId() const { return contextId_; }

    // D-10 / CC-2: Hard cap. diagnostics_.size() will NEVER exceed the
    // configured value. When a report() would cause overflow:
    //   - the caller's diagnostic is dropped;
    //   - overflowCount_ is incremented;
    //   - truncated_ is set (queryable via isTruncated());
    //   - if there is room, the LAST slot of diagnostics_ is replaced by a
    //     single DIAG_TRUNCATED marker so snapshot consumers notice the
    //     loss without exceeding the cap.
    //
    // A cap of 0 is treated as "unlimited", matching KernelContext
    // quotas semantics. Use setMaxDiagnostics(SIZE_MAX) explicitly if you
    // want "unlimited" without the 0-is-sentinel convention.
    void setMaxDiagnostics(std::size_t cap) {
        maxDiagnostics_ = (cap == 0)
            ? (std::numeric_limits<std::size_t>::max)()
            : cap;
    }
    std::size_t maxDiagnostics() const { return maxDiagnostics_; }
    bool isTruncated() const { return truncated_; }

    // CC-1: Count of diagnostics dropped due to std::bad_alloc in report().
    std::size_t overflowCount() const { return overflowCount_; }

    // ── Sinks (see logging_sink.h) ───────────────────────
    void addSink(std::shared_ptr<IDiagnosticSink> sink);
    void clearSinks();

private:
    std::vector<Diagnostic> diagnostics_;

    // DS-1: widened from int to uint64_t.
    uint64_t generation_ = 0;  // Incremented on clear(); see scopeGeneration()

    // D-3 / OR-5: incremental counters and flags — O(1) queries.
    int errorCount_ = 0;
    int warningCount_ = 0;
    int fatalCount_ = 0;
    bool geometryDegraded_ = false;
    bool namingDegraded_ = false;

    // D-9 / D-10.
    uint64_t contextId_ = 0;
    std::size_t maxDiagnostics_ = 10000;
    bool truncated_ = false;  // true once the DIAG_TRUNCATED marker is posted

    // CC-1.
    std::size_t overflowCount_ = 0;

    // Registered sinks. Copied under value semantics for safety on mutation.
    std::vector<std::shared_ptr<IDiagnosticSink>> sinks_;

    // CC-1: thread-local last-resort ring used when the main vector throws.
    // Shared across all collectors on a thread — the contract is "never lose
    // a diagnostic, even under OOM", not "preserve ordering across instances".
    static constexpr std::size_t kLastResortCapacity = 16;
    static thread_local std::array<Diagnostic, kLastResortCapacity> lastResortRing_;
    static thread_local std::size_t lastResortIndex_;

    // Internal helper — stamps timestampNs / threadId / contextId if unset.
    void stampMetadata_(Diagnostic& d) const;

    // Internal helper — updates incremental counters for a diagnostic that
    // is about to be (or has just been) pushed.
    void bumpCounters_(const Diagnostic& d);
    void recomputeCounters_();
};

} // namespace oreo

#endif // OREO_DIAGNOSTIC_H
