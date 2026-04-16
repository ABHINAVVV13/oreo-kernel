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

#ifndef OREO_OPERATION_RESULT_H
#define OREO_OPERATION_RESULT_H

#include "diagnostic.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreo {

template<typename T>
class OperationResult {
public:
    // Successful result
    static OperationResult success(T value, std::vector<Diagnostic> diags = {}) {
        OperationResult r;
        r.value_ = std::move(value);
        r.diagnostics_ = std::move(diags);
        r.ok_ = true;
        return r;
    }

    // Failed result
    static OperationResult failure(std::vector<Diagnostic> diags) {
        OperationResult r;
        r.diagnostics_ = std::move(diags);
        r.ok_ = false;
        return r;
    }

    // Check success
    bool ok() const { return ok_; }
    explicit operator bool() const { return ok_; }

    // Access value (throws if not ok)
    const T& value() const& {
        if (!ok_) throw std::runtime_error("Accessing value of failed OperationResult");
        return *value_;
    }
    T& value() & {
        if (!ok_) throw std::runtime_error("Accessing value of failed OperationResult");
        return *value_;
    }
    T&& value() && {
        if (!ok_) throw std::runtime_error("Accessing value of failed OperationResult");
        return std::move(*value_);
    }

    // Access value with default (no throw)
    const T& valueOr(const T& defaultVal) const {
        return ok_ ? *value_ : defaultVal;
    }

    // Diagnostics attached to this result
    const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

    // Convenience queries on attached diagnostics
    bool hasErrors() const {
        for (auto& d : diagnostics_)
            if (d.severity == Severity::Error || d.severity == Severity::Fatal) return true;
        return false;
    }
    bool hasWarnings() const {
        for (auto& d : diagnostics_)
            if (d.severity == Severity::Warning) return true;
        return false;
    }
    bool isGeometryDegraded() const {
        for (auto& d : diagnostics_)
            if (d.geometryDegraded) return true;
        return false;
    }
    bool isNamingDegraded() const {
        for (auto& d : diagnostics_)
            if (d.namingDegraded) return true;
        return false;
    }

    // Get the first error message (for quick debugging)
    std::string errorMessage() const {
        for (auto& d : diagnostics_)
            if (d.severity == Severity::Error || d.severity == Severity::Fatal) return d.message;
        return {};
    }

private:
    OperationResult() = default;
    std::optional<T> value_;
    std::vector<Diagnostic> diagnostics_;
    bool ok_ = false;
};

} // namespace oreo

#endif // OREO_OPERATION_RESULT_H
