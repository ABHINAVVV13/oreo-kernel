// SPDX-License-Identifier: LGPL-2.1-or-later

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
// NESTING SEMANTICS (DS-2):
//   DiagnosticScope is a VIEW over the context's diagnostic vector, not
//   a private buffer. An outer scope sees the union of its own diagnostics
//   plus every diagnostic reported inside any inner scope it contains —
//   nested scopes do not hide their diagnostics from their parent. This is
//   intentional: it lets a top-level operation observe everything that
//   happened inside its sub-operations. If you need isolation, use a
//   separate KernelContext / DiagnosticCollector.
//
// POISONING (DS-3):
//   If DiagnosticCollector::clear() runs while a DiagnosticScope is live,
//   the scope's recorded startIndex_ is meaningless. The scope detects this
//   via the collector's generation counter, sets an internal poisoned_ flag
//   (queryable via wasPoisoned()), and from that point returns empty
//   diagnostics / false from hasErrors() etc. This is safer than returning
//   undefined slices of the vector.
//
// EXTRACTION TRACKING (DS-4):
//   Debug builds log a single-line warning in the destructor if the scope
//   went out of scope carrying errors that were never extracted via
//   extractDiagnostics(), makeResult(), or makeFailure(). Release builds
//   skip this check for zero overhead.
//
// Usage:
//   NamedShape extrude(KernelContext& ctx, ...) {
//       DiagnosticScope scope(ctx);  // Captures diagnostics from here
//       // ... do work, report to ctx.diag ...
//       return scope.makeResult(shape);  // Extracts scope's diagnostics into result
//   }

#ifndef OREO_DIAGNOSTIC_SCOPE_H
#define OREO_DIAGNOSTIC_SCOPE_H

#pragma once

#include "diagnostic.h"
#include "operation_result.h"

#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace oreo {

class KernelContext;

class DiagnosticScope {
public:
    // Enter a new scope. Records the current diagnostic count.
    explicit DiagnosticScope(KernelContext& ctx);

    // DS-4: debug builds emit a warning if the scope was never extracted
    // AND it carried errors/fatals. Release builds use the trivial dtor.
#ifndef NDEBUG
    ~DiagnosticScope();
#else
    ~DiagnosticScope() = default;
#endif

    // No copy
    DiagnosticScope(const DiagnosticScope&) = delete;
    DiagnosticScope& operator=(const DiagnosticScope&) = delete;

    // Extract diagnostics added during this scope.
    // Does NOT clear them from the context (parent may still want them).
    // Marks the scope as "extracted" for DS-4 tracking and sets the
    // poisoned_ flag if the generation mismatched.
    std::vector<Diagnostic> extractDiagnostics() const;

    // Check if any errors were added during this scope.
    bool hasErrors() const;
    bool hasWarnings() const;

    // DS-3: true if clear() was observed to have run between scope entry
    // and the most recent query. Callers should treat a poisoned scope as
    // "state lost" and bail out gracefully.
    bool wasPoisoned() const { return poisoned_; }

    // Build an OperationResult from this scope's diagnostics.
    //
    // DS-5: SFINAE-constrain so T must be move-constructible (OperationResult
    // stores T in std::optional and moves it into place; non-movable Ts
    // would fail deep in the template stack with confusing diagnostics).
    template <typename T,
              typename = std::enable_if_t<std::is_move_constructible_v<T>>>
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
    template <typename T,
              typename = std::enable_if_t<std::is_move_constructible_v<T>>>
    OperationResult<T> makeFailure() const {
        return OperationResult<T>::failure(extractDiagnostics());
    }

private:
    KernelContext& ctx_;
    int startIndex_;      // Diagnostic count when scope was entered

    // DS-1: widened from int to uint64_t to match DiagnosticCollector.
    uint64_t generation_; // DiagnosticCollector generation at scope entry;
                          // if it changes, clear() was called and startIndex_ is invalid

    // DS-3/DS-4: mutable because they track observed state from const accessors.
    mutable bool poisoned_ = false;
    mutable bool extracted_ = false;
};

} // namespace oreo

#endif // OREO_DIAGNOSTIC_SCOPE_H
