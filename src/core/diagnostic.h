// diagnostic.h — Structured diagnostics system for oreo-kernel.
//
// Replaces the old thread-local single-error system with:
//   - Multiple diagnostics per operation (errors + warnings + info)
//   - Severity levels (Info, Warning, Error, Fatal)
//   - Degraded-geometry and degraded-naming flags
//   - Feature and element reference context
//   - Serializable for persistence with rebuild state
//
// DiagnosticCollector is CONTEXT-BOUND — each KernelContext has its own.

#ifndef OREO_DIAGNOSTIC_H
#define OREO_DIAGNOSTIC_H

#include "thread_safety.h"

#include <string>
#include <vector>

namespace oreo {

// ─── ErrorCode ──────────────────────────────────────────────
// Moved here from oreo_error.h — this is the canonical definition.

enum class ErrorCode {
    OK = 0,
    INVALID_INPUT,       // null shape, zero-length vector, etc.
    OCCT_FAILURE,        // OCCT operation returned !IsDone()
    BOOLEAN_FAILED,      // Boolean operation failed (common with near-degenerate geometry)
    SHAPE_INVALID,       // BRepCheck_Analyzer says the result is invalid
    SHAPE_FIX_FAILED,    // ShapeFix was attempted but couldn't repair the shape
    SKETCH_SOLVE_FAILED, // PlaneGCS solver returned non-convergent
    SKETCH_REDUNDANT,    // Sketch has redundant constraints
    SKETCH_CONFLICTING,  // Sketch has conflicting constraints
    STEP_IMPORT_FAILED,  // STEP file could not be read
    STEP_EXPORT_FAILED,  // STEP file could not be written
    SERIALIZE_FAILED,    // Binary serialization failed
    DESERIALIZE_FAILED,  // Binary deserialization failed
    NOT_INITIALIZED,     // oreo_init() was not called
    INTERNAL_ERROR,      // Bug in oreo-kernel
};

// ─── Severity ────────────────────────────────────────────────

enum class Severity {
    Info,       // Informational — no action needed
    Warning,    // Non-fatal issue — result is usable but may be degraded
    Error,      // Operation failed — no valid result
    Fatal,      // Unrecoverable — kernel state may be corrupt
};

// ─── Diagnostic ──────────────────────────────────────────────

struct Diagnostic {
    Severity severity = Severity::Error;
    ErrorCode code = ErrorCode::OK;     // Specific error code
    std::string message;                // Human-readable description
    std::string featureId;              // Which feature produced this (empty if not in feature context)
    std::string elementRef;             // Which element reference failed (empty if not applicable)
    std::string suggestion;             // Actionable recovery suggestion

    // Quality flags
    bool geometryDegraded = false;      // Shape is geometrically valid but quality is reduced
                                        // (e.g., ShapeFix healed a broken result)
    bool namingDegraded = false;        // Element names may be unstable
                                        // (e.g., NullMapper fallback was used)

    // Convenience constructors
    static Diagnostic info(ErrorCode code, const std::string& msg);
    static Diagnostic warning(ErrorCode code, const std::string& msg,
                              const std::string& suggestion = {});
    static Diagnostic error(ErrorCode code, const std::string& msg,
                            const std::string& suggestion = {});
    static Diagnostic fatal(ErrorCode code, const std::string& msg);
};

// ─── DiagnosticCollector ─────────────────────────────────────

class OREO_CONTEXT_BOUND DiagnosticCollector {
public:
    DiagnosticCollector() = default;

    // Report a diagnostic
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

    // Clear all diagnostics. Increments the generation counter so that
    // any live DiagnosticScope detects invalidation.
    void clear();

    // Generation counter — incremented on every clear(). Used by
    // DiagnosticScope to detect if its startIndex_ was invalidated.
    int scopeGeneration() const { return generation_; }

    // ── Query ────────────────────────────────────────────

    bool empty() const { return diagnostics_.empty(); }
    bool hasErrors() const;
    bool hasWarnings() const;
    bool hasFatal() const;
    bool isGeometryDegraded() const;
    bool isNamingDegraded() const;

    // Get all diagnostics
    const std::vector<Diagnostic>& all() const { return diagnostics_; }

    // Filter by severity
    std::vector<Diagnostic> errors() const;
    std::vector<Diagnostic> warnings() const;

    // Last error (backward compatibility with old API)
    const Diagnostic* lastError() const;

    // Count
    int count() const { return static_cast<int>(diagnostics_.size()); }
    int errorCount() const;
    int warningCount() const;

private:
    std::vector<Diagnostic> diagnostics_;
    int generation_ = 0;  // Incremented on clear(); see scopeGeneration()
};

} // namespace oreo

#endif // OREO_DIAGNOSTIC_H
