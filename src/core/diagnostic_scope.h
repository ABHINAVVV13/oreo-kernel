// diagnostic_scope.h — Composable diagnostic scoping.
//
// Every kernel operation opens a DiagnosticScope. The scope captures
// diagnostics added during that operation, and can compose them into
// an OperationResult or propagate to the parent context.
// When the scope ends, its diagnostics can be:
//   - Extracted into an OperationResult
//   - Merged back into the parent context
//   - Discarded
//
// Usage:
//   NamedShape extrude(KernelContext& ctx, ...) {
//       DiagnosticScope scope(ctx);  // Captures diagnostics from here
//       // ... do work, report to ctx.diag ...
//       return scope.makeResult(shape);  // Extracts scope's diagnostics into result
//   }

#ifndef OREO_DIAGNOSTIC_SCOPE_H
#define OREO_DIAGNOSTIC_SCOPE_H

#include "diagnostic.h"
#include "operation_result.h"

#include <vector>

namespace oreo {

class KernelContext;

class DiagnosticScope {
public:
    // Enter a new scope. Records the current diagnostic count.
    explicit DiagnosticScope(KernelContext& ctx);

    // On destruction, does NOT clear — diagnostics remain in context
    // unless explicitly extracted via extractDiagnostics().
    ~DiagnosticScope() = default;

    // No copy
    DiagnosticScope(const DiagnosticScope&) = delete;
    DiagnosticScope& operator=(const DiagnosticScope&) = delete;

    // Extract diagnostics added during this scope.
    // Does NOT clear them from the context (parent may still want them).
    std::vector<Diagnostic> extractDiagnostics() const;

    // Check if any errors were added during this scope.
    bool hasErrors() const;
    bool hasWarnings() const;

    // Build an OperationResult from this scope's diagnostics.
    template<typename T>
    OperationResult<T> makeResult(T value) const {
        auto diags = extractDiagnostics();
        bool hasErr = false;
        for (auto& d : diags) {
            if (d.severity == Severity::Error || d.severity == Severity::Fatal) {
                hasErr = true;
                break;
            }
        }
        if (hasErr) {
            return OperationResult<T>::failure(std::move(diags));
        }
        return OperationResult<T>::success(std::move(value), std::move(diags));
    }

    // Build a failed result from this scope's diagnostics.
    template<typename T>
    OperationResult<T> makeFailure() const {
        return OperationResult<T>::failure(extractDiagnostics());
    }

private:
    KernelContext& ctx_;
    int startIndex_;   // Diagnostic count when scope was entered
    int generation_;   // DiagnosticCollector generation at scope entry;
                       // if it changes, clear() was called and startIndex_ is invalid
};

} // namespace oreo

#endif // OREO_DIAGNOSTIC_SCOPE_H
