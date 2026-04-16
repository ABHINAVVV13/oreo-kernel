// diagnostic_scope.cpp — DiagnosticScope implementation.

#include "diagnostic_scope.h"
#include "kernel_context.h"

namespace oreo {

DiagnosticScope::DiagnosticScope(KernelContext& ctx)
    : ctx_(ctx)
    , startIndex_(ctx.diag.count())
    , generation_(ctx.diag.scopeGeneration())
{}

std::vector<Diagnostic> DiagnosticScope::extractDiagnostics() const {
    // If clear() was called while this scope was live, our startIndex_
    // is meaningless — return empty rather than returning garbage.
    if (generation_ != ctx_.diag.scopeGeneration()) return {};

    const auto& all = ctx_.diag.all();
    if (startIndex_ >= static_cast<int>(all.size())) return {};
    return std::vector<Diagnostic>(all.begin() + startIndex_, all.end());
}

bool DiagnosticScope::hasErrors() const {
    if (generation_ != ctx_.diag.scopeGeneration()) return false;
    const auto& all = ctx_.diag.all();
    for (int i = startIndex_; i < static_cast<int>(all.size()); ++i) {
        if (all[i].severity == Severity::Error || all[i].severity == Severity::Fatal)
            return true;
    }
    return false;
}

bool DiagnosticScope::hasWarnings() const {
    if (generation_ != ctx_.diag.scopeGeneration()) return false;
    const auto& all = ctx_.diag.all();
    for (int i = startIndex_; i < static_cast<int>(all.size()); ++i) {
        if (all[i].severity == Severity::Warning) return true;
    }
    return false;
}

} // namespace oreo
