// thread_safety.h — Thread safety contract annotations for oreo-kernel.
//
// These macros document and enforce thread safety guarantees.
// They produce no runtime code — they are documentation that
// compilers and static analyzers can check.
//
// Thread safety levels:
//
//   OREO_IMMUTABLE
//     Safe to share across threads without synchronization.
//     The object is never mutated after construction.
//     Examples: SchemaVersion, UnitSystem (after init), TolerancePolicy (after init)
//
//   OREO_CONTEXT_BOUND
//     Safe within one KernelContext. Must not be shared across contexts
//     or accessed from multiple threads without external synchronization.
//     Each thread should have its own KernelContext.
//     Examples: KernelContext, DiagnosticCollector, TagAllocator,
//               FeatureTree, NamedShape
//
//   OREO_NOT_THREAD_SAFE
//     Must be externally synchronized if accessed from multiple threads.
//     In practice, these types should only be used within a single
//     worker process or behind a mutex.
//     Examples: OCCT operations (OCCT has global static state),
//               Feature tree replay (mutates cache)
//
// Rules:
//   1. One KernelContext per thread. Never share a context across threads.
//   2. OCCT operations must not run concurrently in the same process
//      without worker process isolation (OCCT internal caches are not thread-safe).
//   3. FeatureTree::replay() must not be called concurrently on the same tree.
//   4. NamedShape objects are safe to read concurrently but not to mutate.

#ifndef OREO_THREAD_SAFETY_H
#define OREO_THREAD_SAFETY_H

// Annotations — no runtime effect, documentation only.
// Can be replaced with Clang Thread Safety Analysis attributes if desired:
//   https://clang.llvm.org/docs/ThreadSafetyAnalysis.html

#if defined(__clang__)
    #define OREO_IMMUTABLE         [[clang::annotate("oreo:immutable")]]
    #define OREO_CONTEXT_BOUND     [[clang::annotate("oreo:context_bound")]]
    #define OREO_NOT_THREAD_SAFE   [[clang::annotate("oreo:not_thread_safe")]]
    #define OREO_GUARDED_BY(x)     __attribute__((guarded_by(x)))
    #define OREO_REQUIRES(x)       __attribute__((requires_capability(x)))
#else
    #define OREO_IMMUTABLE
    #define OREO_CONTEXT_BOUND
    #define OREO_NOT_THREAD_SAFE
    #define OREO_GUARDED_BY(x)
    #define OREO_REQUIRES(x)
#endif

#endif // OREO_THREAD_SAFETY_H
