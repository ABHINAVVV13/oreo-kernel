// diagnostic.cpp — DiagnosticCollector implementation.

#include "diagnostic.h"
#include "logging_sink.h"
#include "oreo_error.h"  // Legacy redirect — kept for source compatibility.

#include <chrono>
#include <cstddef>
#include <functional>
#include <new>
#include <thread>
#include <utility>

namespace oreo {

// ─── errorCodeToString ───────────────────────────────────────
// D-12: one case per enum value; keep in lockstep with ErrorCode.

const char* errorCodeToString(ErrorCode code) {
    switch (code) {
        case ErrorCode::OK:                  return "OK";
        case ErrorCode::INVALID_INPUT:       return "INVALID_INPUT";
        case ErrorCode::OCCT_FAILURE:        return "OCCT_FAILURE";
        case ErrorCode::BOOLEAN_FAILED:      return "BOOLEAN_FAILED";
        case ErrorCode::SHAPE_INVALID:       return "SHAPE_INVALID";
        case ErrorCode::SHAPE_FIX_FAILED:    return "SHAPE_FIX_FAILED";
        case ErrorCode::SKETCH_SOLVE_FAILED: return "SKETCH_SOLVE_FAILED";
        case ErrorCode::SKETCH_REDUNDANT:    return "SKETCH_REDUNDANT";
        case ErrorCode::SKETCH_CONFLICTING:  return "SKETCH_CONFLICTING";
        case ErrorCode::STEP_IMPORT_FAILED:  return "STEP_IMPORT_FAILED";
        case ErrorCode::STEP_EXPORT_FAILED:  return "STEP_EXPORT_FAILED";
        case ErrorCode::SERIALIZE_FAILED:    return "SERIALIZE_FAILED";
        case ErrorCode::DESERIALIZE_FAILED:  return "DESERIALIZE_FAILED";
        case ErrorCode::NOT_INITIALIZED:     return "NOT_INITIALIZED";
        case ErrorCode::INTERNAL_ERROR:      return "INTERNAL_ERROR";
        case ErrorCode::NOT_SUPPORTED:       return "NOT_SUPPORTED";
        case ErrorCode::INVALID_STATE:       return "INVALID_STATE";
        case ErrorCode::OUT_OF_RANGE:        return "OUT_OF_RANGE";
        case ErrorCode::TIMEOUT:             return "TIMEOUT";
        case ErrorCode::CANCELLED:           return "CANCELLED";
        case ErrorCode::RESOURCE_EXHAUSTED:  return "RESOURCE_EXHAUSTED";
        case ErrorCode::DIAG_TRUNCATED:      return "DIAG_TRUNCATED";
    }
    return "UNKNOWN";
}

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

// D-4: fatal with suggestion.
Diagnostic Diagnostic::fatal(ErrorCode code, const std::string& msg,
                             const std::string& suggestion) {
    Diagnostic d;
    d.severity = Severity::Fatal;
    d.code = code;
    d.message = msg;
    d.suggestion = suggestion;
    return d;
}

// ─── JSON round-trip (D-7 / CC-12) ───────────────────────────

nlohmann::json Diagnostic::toJSON() const {
    nlohmann::json j;
    j["severity"]         = static_cast<int>(severity);
    j["code"]             = static_cast<int>(code);
    j["codeName"]         = errorCodeToString(code);  // informational; ignored on read
    j["message"]          = message;
    j["featureId"]        = featureId;
    j["elementRef"]       = elementRef;
    j["suggestion"]       = suggestion;
    j["geometryDegraded"] = geometryDegraded;
    j["namingDegraded"]   = namingDegraded;
    j["timestampNs"]      = timestampNs;
    j["threadId"]         = threadId;
    j["contextId"]        = contextId;
    return j;
}

Diagnostic Diagnostic::fromJSON(const nlohmann::json& j) {
    Diagnostic d;
    if (j.contains("severity"))         d.severity         = static_cast<Severity>(j.at("severity").get<int>());
    if (j.contains("code"))             d.code             = static_cast<ErrorCode>(j.at("code").get<int>());
    if (j.contains("message"))          d.message          = j.at("message").get<std::string>();
    if (j.contains("featureId"))        d.featureId        = j.at("featureId").get<std::string>();
    if (j.contains("elementRef"))       d.elementRef       = j.at("elementRef").get<std::string>();
    if (j.contains("suggestion"))       d.suggestion       = j.at("suggestion").get<std::string>();
    if (j.contains("geometryDegraded")) d.geometryDegraded = j.at("geometryDegraded").get<bool>();
    if (j.contains("namingDegraded"))   d.namingDegraded   = j.at("namingDegraded").get<bool>();
    if (j.contains("timestampNs"))      d.timestampNs      = j.at("timestampNs").get<uint64_t>();
    if (j.contains("threadId"))         d.threadId         = j.at("threadId").get<uint64_t>();
    if (j.contains("contextId"))        d.contextId        = j.at("contextId").get<uint64_t>();
    return d;
}

// ─── DiagnosticCollector ─────────────────────────────────────

// CC-1: thread_local last-resort ring definition.
thread_local std::array<Diagnostic, DiagnosticCollector::kLastResortCapacity>
    DiagnosticCollector::lastResortRing_{};
thread_local std::size_t DiagnosticCollector::lastResortIndex_ = 0;

DiagnosticCollector::DiagnosticCollector() {
    // CC-1: reserve up front so the typical case never allocates inside
    // report(). The cap is conservative — a busy rebuild can hit 100s of
    // diagnostics before maxDiagnostics_ kicks in.
    try {
        diagnostics_.reserve(256);
    } catch (const std::bad_alloc&) {
        // Best-effort: if we can't reserve even this much, report() falls
        // back to the last-resort ring.
    }
}

void DiagnosticCollector::stampMetadata_(Diagnostic& d) const {
    if (d.timestampNs == 0) {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        d.timestampNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    }
    if (d.threadId == 0) {
        d.threadId = static_cast<uint64_t>(
            std::hash<std::thread::id>{}(std::this_thread::get_id()));
    }
    if (d.contextId == 0) {
        d.contextId = contextId_;
    }
}

void DiagnosticCollector::bumpCounters_(const Diagnostic& d) {
    switch (d.severity) {
        case Severity::Fatal:   ++fatalCount_;   break;
        case Severity::Error:   ++errorCount_;   break;
        case Severity::Warning: ++warningCount_; break;
        case Severity::Info:
        case Severity::Trace:
        case Severity::Debug:
        default:
            break;
    }
    if (d.geometryDegraded) geometryDegraded_ = true;
    if (d.namingDegraded)   namingDegraded_   = true;
}

void DiagnosticCollector::report(Diagnostic diag) {
    stampMetadata_(diag);

    // D-10 / CC-2: hard cap. diagnostics_.size() must never exceed
    // maxDiagnostics_. On overflow:
    //   - set truncated_ and bump overflowCount_;
    //   - if there is room, replace the LAST slot with a single
    //     DIAG_TRUNCATED marker so consumers of snapshot()/all() see the
    //     loss without exceeding the cap;
    //   - otherwise (cap == 0 is impossible here because setMaxDiagnostics
    //     remaps it to SIZE_MAX, so this branch only triggers when the cap
    //     is non-zero and already reached), drop silently.
    //
    // Dropping the new diagnostic preserves the FIRST-N-kept invariant
    // that matters for root-cause analysis — the earliest diagnostic is
    // almost always more informative than the Nth.
    if (diagnostics_.size() >= maxDiagnostics_) {
        ++overflowCount_;
        if (!truncated_) {
            truncated_ = true;
            // Replace the last kept diagnostic with the marker so the
            // vector size is unchanged. When maxDiagnostics_ == 0 (only
            // possible if a caller used SIZE_MAX sentinel and still
            // overflowed) there is no last slot to replace — skip.
            if (!diagnostics_.empty()) {
                Diagnostic marker;
                marker.severity = Severity::Warning;
                marker.code = ErrorCode::DIAG_TRUNCATED;
                marker.message =
                    "Diagnostic cap reached; further diagnostics dropped "
                    "(last kept diagnostic was replaced by this marker).";
                marker.suggestion =
                    "Increase setMaxDiagnostics() or clear() the collector.";
                stampMetadata_(marker);
                // Replacing an existing slot is non-throwing (no allocation).
                diagnostics_.back() = std::move(marker);
                bumpCounters_(diagnostics_.back());
            }
        }
        return;
    }

    // CC-1: push_back may throw on OOM. On failure, write to the last-resort
    // ring so the diagnostic isn't silently lost, and increment overflowCount_.
    try {
        diagnostics_.push_back(std::move(diag));
        bumpCounters_(diagnostics_.back());
    } catch (const std::bad_alloc&) {
        ++overflowCount_;
        try {
            lastResortRing_[lastResortIndex_ % kLastResortCapacity] = std::move(diag);
            ++lastResortIndex_;
        } catch (...) {
            // Nothing more we can do — drop silently.
        }
        return;
    }

    // Forward to sinks. Sinks are declared noexcept, but defend anyway —
    // a misbehaving sink must never break the collector.
    for (const auto& sink : sinks_) {
        if (!sink) continue;
        try {
            sink->onReport(diagnostics_.back());
        } catch (...) {
            // Swallow — sinks are not allowed to propagate exceptions.
        }
    }
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

void DiagnosticCollector::fatal(ErrorCode code, const std::string& msg,
                                const std::string& suggestion) {
    report(Diagnostic::fatal(code, msg, suggestion));
}

void DiagnosticCollector::clear() {
    diagnostics_.clear();
    errorCount_ = 0;
    warningCount_ = 0;
    fatalCount_ = 0;
    geometryDegraded_ = false;
    namingDegraded_ = false;
    truncated_ = false;
    ++generation_;  // Invalidate any live DiagnosticScope instances
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

std::optional<Diagnostic> DiagnosticCollector::lastErrorOpt() const {
    for (auto it = diagnostics_.rbegin(); it != diagnostics_.rend(); ++it) {
        if (it->severity == Severity::Error || it->severity == Severity::Fatal) {
            return *it;
        }
    }
    return std::nullopt;
}

const Diagnostic* DiagnosticCollector::lastError() const {
    for (auto it = diagnostics_.rbegin(); it != diagnostics_.rend(); ++it) {
        if (it->severity == Severity::Error || it->severity == Severity::Fatal) {
            return &(*it);
        }
    }
    return nullptr;
}

void DiagnosticCollector::addSink(std::shared_ptr<IDiagnosticSink> sink) {
    if (sink) sinks_.push_back(std::move(sink));
}

void DiagnosticCollector::clearSinks() {
    sinks_.clear();
}

} // namespace oreo
