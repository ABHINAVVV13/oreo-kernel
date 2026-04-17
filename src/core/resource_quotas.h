// SPDX-License-Identifier: LGPL-2.1-or-later

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

    // ── Subsystem hard limits (enforced at the kernel boundary) ────
    //
    // Each of these is checked before the corresponding subsystem
    // allocates a large buffer or accepts a large input. A value of 0
    // means "no limit" — defaults below are sized for a 64 GB cloud
    // worker handling commercial-CAD-size models. Override per request
    // for stricter sandbox tenants.

    // Maximum total triangles a tessellated mesh may contain. Above
    // this, exportGLB / tessellate fail with RESOURCE_EXHAUSTED before
    // allocating output buffers. Default 50M triangles ≈ 600 MB for
    // POSITION+NORMAL+indices. 0 = unlimited.
    uint64_t maxMeshTriangles = 50ULL * 1000 * 1000;

    // Maximum total vertices a tessellated mesh may contain. glTF uses
    // 32-bit indices so the absolute hard ceiling is 2^32-1; the
    // default leaves headroom for face-group splits. 0 = unlimited.
    uint64_t maxMeshVertices  = 100ULL * 1000 * 1000;

    // Maximum size of an exported GLB byte buffer. Capped at slightly
    // under UINT32_MAX so the spec-mandated uint32 byteOffset fields
    // cannot silently truncate. 0 = unlimited (subject to the
    // hard UINT32_MAX limit which is always enforced).
    uint64_t maxGlbBytes      = 3ULL * 1024 * 1024 * 1024; // 3 GiB

    // Maximum size of a STEP file accepted by importStep. Above this,
    // the import fails with RESOURCE_EXHAUSTED before invoking OCCT's
    // STEP reader (which is unbounded). 0 = unlimited.
    uint64_t maxStepBytes     = 512ULL * 1024 * 1024;     // 512 MiB

    // Maximum size of a buffer produced by serialize(). Computed before
    // emit; 0 = unlimited. Bounded so a pathological feature tree
    // cannot OOM the host.
    uint64_t maxSerializeBytes = 1ULL * 1024 * 1024 * 1024; // 1 GiB

    // Maximum number of features allowed in a single FeatureTree.
    // addFeature / insertFeature / fromJSON refuse to grow beyond this.
    // 0 = unlimited.
    uint64_t maxFeatures      = 10000;
};

} // namespace oreo

#endif // OREO_RESOURCE_QUOTAS_H
