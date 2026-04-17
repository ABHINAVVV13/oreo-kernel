// SPDX-License-Identifier: LGPL-2.1-or-later

// operation_result.h — Typed result object for kernel operations.
//
// Every kernel operation returns OperationResult<T> instead of a naked T.
// The result carries:
//   - The value (if operation succeeded)
//   - Diagnostics from this specific operation (not from the context globally)
//   - Success/failure flag
//   - Degraded-geometry / degraded-naming flags
//
// This replaces the pattern of returning nullable NamedShape and checking
// a separate context.diag for errors. The diagnostics travel WITH the result.
//
// ── API surface (post OR-1..OR-7 overhaul) ───────────────────────────────
//
//   value()            REQUIRES ok() — undefined behaviour otherwise (debug assert).
//                      Returns T& / const T& / T&&. Fast. No throw.
//   valueOrThrow()     Legacy exception-throwing accessor, kept for back-compat.
//   tryValue()         Returns T* / nullptr; safe to call without checking ok().
//   valueOr(ref)       Returns reference to the argument when !ok (caller owns lifetime).
//   valueOr(val)       By-value overload that accepts rvalues safely.
//   map(f)             If ok, wrap f(value) in a new OperationResult; else propagate.
//   flatMap(f)         Like map, but f itself returns an OperationResult.
//   andThen(f)         Run f() for side-effects when ok; propagates same type.
//   allErrorMessages() All error/fatal messages; errorMessage() returns first for back-compat.
//   operator==         Structural comparison (ok + severity/code pairs).

#ifndef OREO_OPERATION_RESULT_H
#define OREO_OPERATION_RESULT_H

#include "diagnostic.h"

#include <cassert>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace oreo {

template<typename T>
class OperationResult {
public:
    // ── Factory ──────────────────────────────────────────────

    // Successful result
    static OperationResult success(T value, std::vector<Diagnostic> diags = {}) {
        OperationResult r;
        r.value_ = std::move(value);
        r.diagnostics_ = std::move(diags);
        r.ok_ = true;
        r.recomputeFlags();
        return r;
    }

    // Failed result
    static OperationResult failure(std::vector<Diagnostic> diags) {
        OperationResult r;
        r.diagnostics_ = std::move(diags);
        r.ok_ = false;
        r.recomputeFlags();
        return r;
    }

    // ── Success query ────────────────────────────────────────

    bool ok() const { return ok_; }
    explicit operator bool() const { return ok_; }

    // ── Value access (OR-1) ──────────────────────────────────

    // Fast accessor — REQUIRES ok(). Undefined behaviour if !ok.
    // In debug builds this is caught by the assert; in release it is UB,
    // matching std::optional::operator*. Use tryValue() or valueOr() if
    // the caller has not verified ok() first.
    const T& value() const& {
        assert(ok_ && "OperationResult::value() called on failed result");
        return *value_;
    }
    T& value() & {
        assert(ok_ && "OperationResult::value() called on failed result");
        return *value_;
    }
    T&& value() && {
        assert(ok_ && "OperationResult::value() called on failed result");
        return std::move(*value_);
    }

    // Legacy exception-throwing accessor — preserved so callers that relied
    // on the old behaviour (throw on failure) keep compiling.
    const T& valueOrThrow() const& {
        if (!ok_) throw std::runtime_error("Accessing value of failed OperationResult");
        return *value_;
    }
    T& valueOrThrow() & {
        if (!ok_) throw std::runtime_error("Accessing value of failed OperationResult");
        return *value_;
    }
    T&& valueOrThrow() && {
        if (!ok_) throw std::runtime_error("Accessing value of failed OperationResult");
        return std::move(*value_);
    }

    // Pointer accessor — never throws, never asserts. Returns nullptr on failure.
    T* tryValue() { return ok_ ? &*value_ : nullptr; }
    const T* tryValue() const { return ok_ ? &*value_ : nullptr; }

    // ── valueOr (OR-2) ───────────────────────────────────────

    // By-value form: safely accepts both lvalue and rvalue defaults
    // (including literals / temporaries). Always returns a T by value,
    // so there is no dangling-reference risk.
    //
    // A reference-returning overload was removed because it was
    // ambiguous with this one under MSVC overload resolution and the
    // by-value form subsumes its use cases (the caller can always copy
    // a local into defaultVal).
    T valueOr(T defaultVal) const {
        return ok_ ? *value_ : std::move(defaultVal);
    }

    // ── Combinators (OR-3) ───────────────────────────────────

    // map: if ok, apply f to value and wrap result; else propagate failure.
    template <typename F>
    auto map(F&& f) const -> OperationResult<decltype(f(std::declval<const T&>()))> {
        using U = decltype(f(std::declval<const T&>()));
        if (!ok_) return OperationResult<U>::failure(diagnostics_);
        return OperationResult<U>::success(f(*value_), diagnostics_);
    }

    // flatMap: f itself returns an OperationResult — chain diagnostics.
    // If the outer is failed we propagate its diagnostics only (f is not called).
    // If f returns a failure, its diagnostics are *appended* to ours.
    template <typename F>
    auto flatMap(F&& f) const -> decltype(f(std::declval<const T&>())) {
        using Ret = decltype(f(std::declval<const T&>()));
        if (!ok_) return Ret::failure(diagnostics_);
        Ret next = f(*value_);
        // Chain diagnostics: ours first, then next's.
        std::vector<Diagnostic> merged = diagnostics_;
        const auto& tail = next.diagnostics();
        merged.insert(merged.end(), tail.begin(), tail.end());
        if (next.ok()) {
            return Ret::success(std::move(next).valueOrThrow(), std::move(merged));
        }
        return Ret::failure(std::move(merged));
    }

    // andThen: invoke f() only if ok — same T preserved either way.
    // f takes const T& and returns nothing (side-effect only).
    template <typename F>
    OperationResult andThen(F&& f) const {
        if (!ok_) return *this;
        f(*value_);
        return *this;
    }

    // ── Diagnostics ──────────────────────────────────────────

    // Read-only view
    const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

    // Setter that also refreshes cached flags (OR-5)
    void setDiagnostics(std::vector<Diagnostic> diags) {
        diagnostics_ = std::move(diags);
        recomputeFlags();
    }

    // Cached severity queries (OR-5). If diagnostics were mutated without
    // going through setDiagnostics(), the cache may be stale — fall back
    // to a rescan in that case. We detect this by tracking size at cache time.
    bool hasErrors() const {
        if (diagnostics_.size() != cachedSize_) return rescanHasErrors();
        return hasErrors_;
    }
    bool hasWarnings() const {
        if (diagnostics_.size() != cachedSize_) return rescanHasWarnings();
        return hasWarnings_;
    }
    bool hasFatal() const {
        if (diagnostics_.size() != cachedSize_) return rescanHasFatal();
        return hasFatal_;
    }
    bool isGeometryDegraded() const {
        if (diagnostics_.size() != cachedSize_) return rescanGeomDegraded();
        return geomDegraded_;
    }
    bool isNamingDegraded() const {
        if (diagnostics_.size() != cachedSize_) return rescanNamingDegraded();
        return namingDegraded_;
    }

    // ── Messages (OR-6) ──────────────────────────────────────

    // First error/fatal message — back-compatible convenience.
    std::string errorMessage() const {
        for (auto& d : diagnostics_)
            if (d.severity == Severity::Error || d.severity == Severity::Fatal) return d.message;
        return {};
    }

    // All error/fatal messages, in diagnostic order.
    std::vector<std::string> allErrorMessages() const {
        std::vector<std::string> out;
        for (auto& d : diagnostics_)
            if (d.severity == Severity::Error || d.severity == Severity::Fatal)
                out.push_back(d.message);
        return out;
    }

    // ── Equality (OR-7) ──────────────────────────────────────

    // Structural comparison: matches on ok_ and the sequence of
    // (severity, code) pairs. Ignores message strings, suggestions,
    // featureId/elementRef, degrade flags, timestamps, threadIds — all
    // of which are environment-dependent and would make equality flaky.
    bool operator==(const OperationResult& other) const {
        if (ok_ != other.ok_) return false;
        if (diagnostics_.size() != other.diagnostics_.size()) return false;
        for (size_t i = 0; i < diagnostics_.size(); ++i) {
            const auto& a = diagnostics_[i];
            const auto& b = other.diagnostics_[i];
            if (a.severity != b.severity) return false;
            if (a.code != b.code) return false;
        }
        return true;
    }
    bool operator!=(const OperationResult& other) const { return !(*this == other); }

private:
    OperationResult() = default;

    // Recompute cached flags from the full diagnostics vector.
    void recomputeFlags() {
        hasErrors_ = false;
        hasWarnings_ = false;
        hasFatal_ = false;
        geomDegraded_ = false;
        namingDegraded_ = false;
        for (const auto& d : diagnostics_) {
            if (d.severity == Severity::Fatal) { hasFatal_ = true; hasErrors_ = true; }
            else if (d.severity == Severity::Error) { hasErrors_ = true; }
            else if (d.severity == Severity::Warning) { hasWarnings_ = true; }
            if (d.geometryDegraded) geomDegraded_ = true;
            if (d.namingDegraded) namingDegraded_ = true;
        }
        cachedSize_ = diagnostics_.size();
    }

    // Fallback rescans when the cache is stale (diagnostics appended directly).
    bool rescanHasErrors() const {
        for (auto& d : diagnostics_)
            if (d.severity == Severity::Error || d.severity == Severity::Fatal) return true;
        return false;
    }
    bool rescanHasWarnings() const {
        for (auto& d : diagnostics_)
            if (d.severity == Severity::Warning) return true;
        return false;
    }
    bool rescanHasFatal() const {
        for (auto& d : diagnostics_)
            if (d.severity == Severity::Fatal) return true;
        return false;
    }
    bool rescanGeomDegraded() const {
        for (auto& d : diagnostics_)
            if (d.geometryDegraded) return true;
        return false;
    }
    bool rescanNamingDegraded() const {
        for (auto& d : diagnostics_)
            if (d.namingDegraded) return true;
        return false;
    }

    std::optional<T> value_;
    std::vector<Diagnostic> diagnostics_;
    bool ok_ = false;

    // Cached severity flags (OR-5). Populated by recomputeFlags().
    // cachedSize_ tracks the diagnostics vector size at cache time;
    // if it differs from the current size, diagnostics were mutated
    // behind our back and we fall back to a rescan.
    bool hasErrors_ = false;
    bool hasWarnings_ = false;
    bool hasFatal_ = false;
    bool geomDegraded_ = false;
    bool namingDegraded_ = false;
    size_t cachedSize_ = 0;
};

} // namespace oreo

#endif // OREO_OPERATION_RESULT_H
