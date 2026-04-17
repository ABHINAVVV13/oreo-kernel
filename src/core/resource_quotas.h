// resource_quotas.h — Per-context resource limits.
//
// ResourceQuotas lets callers bound the worst-case resource consumption of
// a KernelContext. Zero means "no limit" so that default-constructed quotas
// behave like the previous unbounded behaviour for all fields except
// maxDiagnostics (which defaults to 10000 to protect against pathological
// logs from adversarial input).
//
// Some limits are hard (enforced at the edge of the kernel) and some are
// advisory (used by subsystems willing to co-operate). See each field.

#ifndef OREO_RESOURCE_QUOTAS_H
#define OREO_RESOURCE_QUOTAS_H

#include <cstdint>

namespace oreo {

struct ResourceQuotas {
    // Maximum number of diagnostics retained in the context's collector.
    // Enforced via DiagnosticCollector::setMaxDiagnostics at context
    // construction. Default 10k is generous for a single document session
    // but small enough to bound memory if a feature replay goes pathological.
    uint64_t maxDiagnostics = 10000;

    // Maximum number of top-level geometry operations permitted per context.
    // 0 = unlimited. Advisory: each subsystem that chooses to honour this
    // quota should poll its current op count against this value.
    uint64_t maxOperations = 0;

    // Soft ceiling on total memory held by kernel data structures
    // associated with this context (named shapes, element maps, caches).
    // 0 = unlimited. Advisory: subsystems are expected to approximate
    // their own footprint and stop allocating before exceeding this bound.
    uint64_t maxContextMemoryBytes = 0;

    // Total CPU time (milliseconds) the context may consume before
    // operations must abort early. 0 = unlimited. Advisory: operations
    // that accept a cancellation token can also consult this budget
    // and self-abort.
    uint64_t cpuTimeBudgetMs = 0;
};

} // namespace oreo

#endif // OREO_RESOURCE_QUOTAS_H
