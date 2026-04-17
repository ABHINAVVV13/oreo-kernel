// SPDX-License-Identifier: LGPL-2.1-or-later

// logging_sink.h — Pluggable diagnostic sink interface.
//
// A DiagnosticCollector can forward each reported Diagnostic to one or more
// registered IDiagnosticSink instances, in addition to its internal buffer.
// Sinks are invoked inline from report(); they MUST be cheap and must never
// let exceptions escape — DiagnosticCollector wraps every sink call in
// try/catch and silently swallows any throw. The interface itself is marked
// noexcept to make this contract explicit at the declaration site.
//
// Typical use: wire up stderr loggers, telemetry forwarders, or IDE bridges
// without coupling the kernel core to any specific logging framework.

#ifndef OREO_LOGGING_SINK_H
#define OREO_LOGGING_SINK_H

#pragma once

#include "diagnostic.h"

#include <memory>

namespace oreo {

class IDiagnosticSink {
public:
    virtual ~IDiagnosticSink() = default;

    // Called by DiagnosticCollector::report() after the diagnostic has
    // been recorded locally. Must not throw — any exception is caught
    // and silently discarded by the collector.
    virtual void onReport(const Diagnostic&) noexcept = 0;
};

} // namespace oreo

#endif // OREO_LOGGING_SINK_H
