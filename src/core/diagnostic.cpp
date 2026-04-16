// diagnostic.cpp — DiagnosticCollector implementation.

#include "diagnostic.h"
#include "oreo_error.h"  // For ErrorCode

namespace oreo {

// ─── Diagnostic factory methods ──────────────────────────────

Diagnostic Diagnostic::info(ErrorCode code, const std::string& msg) {
    Diagnostic d;
    d.severity = Severity::Info;
    d.code = code;
    d.message = msg;
    return d;
}

Diagnostic Diagnostic::warning(ErrorCode code, const std::string& msg,
                               const std::string& suggestion) {
    Diagnostic d;
    d.severity = Severity::Warning;
    d.code = code;
    d.message = msg;
    d.suggestion = suggestion;
    return d;
}

Diagnostic Diagnostic::error(ErrorCode code, const std::string& msg,
                             const std::string& suggestion) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = code;
    d.message = msg;
    d.suggestion = suggestion;
    return d;
}

Diagnostic Diagnostic::fatal(ErrorCode code, const std::string& msg) {
    Diagnostic d;
    d.severity = Severity::Fatal;
    d.code = code;
    d.message = msg;
    return d;
}

// ─── DiagnosticCollector ─────────────────────────────────────

void DiagnosticCollector::report(Diagnostic diag) {
    diagnostics_.push_back(std::move(diag));
}

void DiagnosticCollector::info(ErrorCode code, const std::string& msg) {
    report(Diagnostic::info(code, msg));
}

void DiagnosticCollector::warning(ErrorCode code, const std::string& msg,
                                  const std::string& suggestion) {
    report(Diagnostic::warning(code, msg, suggestion));
}

void DiagnosticCollector::error(ErrorCode code, const std::string& msg,
                                const std::string& suggestion) {
    report(Diagnostic::error(code, msg, suggestion));
}

void DiagnosticCollector::error(ErrorCode code, const std::string& msg,
                                const std::string& entityRef, const std::string& suggestion) {
    auto d = Diagnostic::error(code, msg, suggestion);
    d.elementRef = entityRef;
    report(std::move(d));
}

void DiagnosticCollector::fatal(ErrorCode code, const std::string& msg) {
    report(Diagnostic::fatal(code, msg));
}

void DiagnosticCollector::clear() {
    diagnostics_.clear();
    ++generation_;  // Invalidate any live DiagnosticScope instances
}

bool DiagnosticCollector::hasErrors() const {
    for (auto& d : diagnostics_) {
        if (d.severity == Severity::Error || d.severity == Severity::Fatal)
            return true;
    }
    return false;
}

bool DiagnosticCollector::hasWarnings() const {
    for (auto& d : diagnostics_) {
        if (d.severity == Severity::Warning) return true;
    }
    return false;
}

bool DiagnosticCollector::hasFatal() const {
    for (auto& d : diagnostics_) {
        if (d.severity == Severity::Fatal) return true;
    }
    return false;
}

bool DiagnosticCollector::isGeometryDegraded() const {
    for (auto& d : diagnostics_) {
        if (d.geometryDegraded) return true;
    }
    return false;
}

bool DiagnosticCollector::isNamingDegraded() const {
    for (auto& d : diagnostics_) {
        if (d.namingDegraded) return true;
    }
    return false;
}

std::vector<Diagnostic> DiagnosticCollector::errors() const {
    std::vector<Diagnostic> result;
    for (auto& d : diagnostics_) {
        if (d.severity == Severity::Error || d.severity == Severity::Fatal)
            result.push_back(d);
    }
    return result;
}

std::vector<Diagnostic> DiagnosticCollector::warnings() const {
    std::vector<Diagnostic> result;
    for (auto& d : diagnostics_) {
        if (d.severity == Severity::Warning) result.push_back(d);
    }
    return result;
}

const Diagnostic* DiagnosticCollector::lastError() const {
    for (auto it = diagnostics_.rbegin(); it != diagnostics_.rend(); ++it) {
        if (it->severity == Severity::Error || it->severity == Severity::Fatal) {
            return &(*it);
        }
    }
    return nullptr;
}

int DiagnosticCollector::errorCount() const {
    int count = 0;
    for (auto& d : diagnostics_) {
        if (d.severity == Severity::Error || d.severity == Severity::Fatal) ++count;
    }
    return count;
}

int DiagnosticCollector::warningCount() const {
    int count = 0;
    for (auto& d : diagnostics_) {
        if (d.severity == Severity::Warning) ++count;
    }
    return count;
}

} // namespace oreo
