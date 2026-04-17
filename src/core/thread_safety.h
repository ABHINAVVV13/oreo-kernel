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

#include <atomic>
#include <thread>
#include <shared_mutex>
#include <mutex>
#include <cstdio>
#include <cstdlib>

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
    // On non-Clang compilers (MSVC, GCC), these annotations are intentionally
    // no-ops. MSVC has no equivalent to Clang's thread safety analysis.
    // For MSVC builds, rely on code review and the documented contracts above
    // to enforce thread safety invariants.
    #define OREO_IMMUTABLE
    #define OREO_CONTEXT_BOUND
    #define OREO_NOT_THREAD_SAFE
    #define OREO_GUARDED_BY(x)
    #define OREO_REQUIRES(x)
#endif

namespace oreo {

// ─── ContextThreadGuard (TS-1 / TS-3) ────────────────────────────
//
// Runtime enforcement of "one KernelContext per thread" contract in
// debug builds. In release builds, all operations compile to no-ops.
//
// Usage:
//   class KernelContext {
//       ContextThreadGuard guard_;
//   public:
//       void someMethod() {
//           OREO_CHECK_THREAD(guard_);
//           ...
//       }
//   };
//
// The first call to checkOrClaim() claims ownership for the calling
// thread. Any subsequent call from a different thread aborts with a
// diagnostic pointing at the offending __FILE__/__LINE__.

#ifdef _DEBUG
class ContextThreadGuard {
    mutable std::atomic<std::thread::id> owner_{};
public:
    ContextThreadGuard() = default;
    ContextThreadGuard(const ContextThreadGuard&) = delete;
    ContextThreadGuard& operator=(const ContextThreadGuard&) = delete;

    void checkOrClaim(const char* file, int line) const {
        std::thread::id expected{};
        std::thread::id current = std::this_thread::get_id();
        // First access claims the guard for this thread.
        if (owner_.compare_exchange_strong(expected, current)) return;
        // Already claimed — must be same thread.
        if (owner_.load() != current) {
            std::fprintf(stderr, "THREAD-SAFETY VIOLATION at %s:%d\n", file, line);
            std::abort();
        }
    }
    // Manually release ownership (e.g. when transferring to another thread).
    void release() noexcept { owner_.store(std::thread::id{}); }
};
#define OREO_CHECK_THREAD(guard) (guard).checkOrClaim(__FILE__, __LINE__)
#else
class ContextThreadGuard {
public:
    ContextThreadGuard() = default;
    ContextThreadGuard(const ContextThreadGuard&) = delete;
    ContextThreadGuard& operator=(const ContextThreadGuard&) = delete;

    void checkOrClaim(const char*, int) const noexcept {}
    void release() noexcept {}
};
#define OREO_CHECK_THREAD(guard) ((void)0)
#endif

// ─── ContextSharedMutex (TS-2) ───────────────────────────────────
//
// Opt-in reader/writer synchronization primitive for callers who
// implement one-writer-many-reader access patterns over shared state.
// The kernel itself still prefers "one context per thread"; this type
// is for advanced callers that deliberately share state.
//
// Usage:
//   ContextSharedMutex mtx;
//   // Readers:
//   {
//       ContextSharedMutex::SharedLock lock(mtx);
//       ... read-only work ...
//   }
//   // Writers:
//   {
//       ContextSharedMutex::WriteLock lock(mtx);
//       ... mutating work ...
//   }

class ContextSharedMutex {
    std::shared_mutex mtx_;
public:
    ContextSharedMutex() = default;
    ContextSharedMutex(const ContextSharedMutex&) = delete;
    ContextSharedMutex& operator=(const ContextSharedMutex&) = delete;

    class SharedLock {
        std::shared_lock<std::shared_mutex> l;
    public:
        explicit SharedLock(ContextSharedMutex& m) : l(m.mtx_) {}
        SharedLock(const SharedLock&) = delete;
        SharedLock& operator=(const SharedLock&) = delete;
    };

    class WriteLock {
        std::unique_lock<std::shared_mutex> l;
    public:
        explicit WriteLock(ContextSharedMutex& m) : l(m.mtx_) {}
        WriteLock(const WriteLock&) = delete;
        WriteLock& operator=(const WriteLock&) = delete;
    };
};

} // namespace oreo

#endif // OREO_THREAD_SAFETY_H
