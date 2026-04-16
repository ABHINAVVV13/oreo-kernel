// kernel_context.cpp — KernelContext implementation.
//
// OCCT init uses std::call_once for thread-safe one-time initialization.
// No process-global mutable state except the once_flag.

#include "kernel_context.h"

#include <OSD.hxx>
#include <Precision.hxx>

#include <atomic>
#include <cmath>
#include <mutex>
#include <sstream>

namespace oreo {

// Thread-safe one-time OCCT initialization
static std::once_flag s_occtOnceFlag;
static bool s_occtInitialized = false;

KernelContext::KernelContext(const KernelConfig& config)
    : tolerance(config.tolerance)
    , units(config.units)
{
    // Set up document-derived tag allocation
    uint32_t docId = config.documentId;
    if (docId == 0 && !config.documentUUID.empty()) {
        docId = TagAllocator::documentIdFromString(config.documentUUID);
    }
    if (docId != 0) {
        tags.setDocumentId(docId);
    }
    if (config.tagSeed > 0) {
        tags.seed(config.tagSeed);
    }

    static std::atomic<int> contextCounter{0};
    std::ostringstream ss;
    ss << "ctx-" << ++contextCounter;
    id_ = ss.str();

    if (config.initOCCT) {
        initOCCT();
    }
}

std::shared_ptr<KernelContext> KernelContext::create(const KernelConfig& config) {
    return std::shared_ptr<KernelContext>(new KernelContext(config));
}

void KernelContext::initOCCT() {
    std::call_once(s_occtOnceFlag, []() {
        OSD::SetSignal(false);
        s_occtInitialized = true;
    });
}

bool KernelContext::isOCCTInitialized() {
    return s_occtInitialized;
}

void KernelContext::beginOperation() {
    // NOTE: This clears ALL diagnostics in this context.
    // For composed operations, use DiagnosticScope instead.
    diag.clear();
}

double computeBooleanFuzzy(const KernelContext& ctx, double bboxSquareExtent) {
    return ctx.tolerance.booleanFuzzyFactor
           * std::sqrt(bboxSquareExtent)
           * Precision::Confusion();
}

} // namespace oreo
