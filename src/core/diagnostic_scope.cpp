// diagnostic_scope.cpp — DiagnosticScope implementation.

#include "diagnostic_scope.h"
#include "kernel_context.h"

namespace oreo {

DiagnosticScope::DiagnosticScope(KernelContext& ctx)
    : ctx_(ctx)
    , startIndex_(ctx.diag().count())
    , generation_(ctx.diag().scopeGeneration())
{}

#ifndef NDEBUG
DiagnosticScope::~DiagnosticScope() {
    // DS-4: warn (once, via the collector) if the scope carried unobserved
    // errors. We must not throw out of the destructor, so we check the
    // generation guard first and never touch the collector if it has been
    // cleared. Also skip if the collector is already saturated.
    if (extracted_) return;
    if (generation_ != ctx_.diag().scopeGeneration()) return;

    const auto& all = ctx_.diag().all();
    bool hasUnobservedError = false;
    for (int i = startIndex_; i < static_cast<int>(all.size()); ++i) {
        if (all[i].severity == Severity::Error || all[i].severity == Severity::Fatal) {
            hasUnobservedError = true;
            break;
        }
    }
    if (!hasUnobservedError) return;

    // Single-line note. We route through the collector's warning() so the
    // message participates in normal diagnostic flow, sinks, and the
    // truncation cap.
    try {
        ctx_.diag().warning(
            ErrorCode::INTERNAL_ERROR,
            "DiagnosticScope destroyed without extracting error diagnostics (debug check).",
            "Call extractDiagnostics() / makeResult() / makeFailure() before scope exit.");
    } catch (...) {
        // Destructors must not throw; swallow.
    }
}
#endif

std::vector<Diagnostic> DiagnosticScope::extractDiagnostics() const {
    extracted_ = true;

    // If clear() was called while this scope was live, our startIndex_
    // is meaningless — poison the scope and return empty rather than
    // returning garbage. DS-3.
    if (generation_ != ctx_.diag().scopeGeneration()) {
        poisoned_ = true;
        return {};
    }

    const auto& all = ctx_.diag().all();
    if (startIndex_ >= static_cast<int>(all.size())) return {};
    return std::vector<Diagnostic>(all.begin() + startIndex_, all.end());
}

bool DiagnosticScope::hasErrors() const {
    if (generation_ != ctx_.diag().scopeGeneration()) {
        poisoned_ = true;
        return false;
    }
    const auto& all = ctx_.diag().all();
    for (int i = startIndex_; i < static_cast<int>(all.size()); ++i) {
        if (all[i].severity == Severity::Error || all[i].severity == Severity::Fatal)
            return true;
    }
    return false;
}

bool DiagnosticScope::hasWarnings() const {
    if (generation_ != ctx_.diag().scopeGeneration()) {
        poisoned_ = true;
        return false;
    }
    const auto& all = ctx_.diag().all();
    for (int i = startIndex_; i < static_cast<int>(all.size()); ++i) {
        if (all[i].severity == Severity::Warning) return true;
    }
    return false;
}

} // namespace oreo
