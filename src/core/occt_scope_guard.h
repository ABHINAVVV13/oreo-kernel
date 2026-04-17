// occt_scope_guard.h — RAII guards for OCCT process-global settings.
//
// OCCT's Interface_Static stores settings globally per-process. When two
// threads or two contexts use *different* settings (e.g. STEP unit), they
// corrupt each other. This guard saves and restores settings on scope exit
// and serialises concurrent mutations at a *per-context* granularity.
//
// ── Concurrency model (OG-1) ─────────────────────────────────
//
//   Callers using the SAME KernelContext (identified by an opaque void*
//   key) are serialised against each other: two guards over the same
//   context cannot be live simultaneously on different threads. Two guards
//   over DIFFERENT contexts CAN run in parallel — they lock separate
//   per-context mutexes.
//
//   Note: OCCT's Interface_Static is *truly* process-global, so even
//   different contexts racing on the same key is unsafe at the OCCT layer.
//   Callers that set overlapping keys on different contexts should still
//   coordinate externally. The per-context mutex only protects against
//   *identity-level* races (two scopes on the same context).
//
// ── Usage ────────────────────────────────────────────────────
//
//   {
//       OcctStaticGuard guard(ctx);            // key = &ctx
//       guard.set("write.step.unit", "MM");    // string
//       guard.set("write.step.precision", 6);  // int
//       guard.set("write.step.angle.tol", 1e-6); // double
//       // ... do STEP export ...
//   }  // Settings restored, mutex released here
//
//   // Legacy zero-arg ctor keeps a single global key for back-compat:
//   {
//       OcctStaticGuard guard;   // key = nullptr → shared global slot
//       guard.set("xstep.cascade.unit", "MM");
//   }
//
// ── Limitations ──────────────────────────────────────────────
//
//   (OG-2) If Interface_Static::CVal/IVal/RVal returned something
//   indicating the key was originally unset, we cannot truly unset it
//   on restore — OCCT has no "remove" API. We emit a warning diagnostic
//   via restoreFailures() instead, and the caller can inspect it.
//
//   (OG-4) The destructor never throws: any OCCT restore that throws is
//   caught and recorded in restoreFailures(). If the host wants to know
//   whether the destructor cleanly reverted state, it must capture the
//   guard by name and inspect restoreFailures() before it goes out of scope.
//
//   (OG-5) The per-context mutex is held for the FULL lifetime of the
//   guard. Keep the scope short: avoid file I/O, network calls, or any
//   slow work between guard construction and destruction. The kernel's
//   STEP I/O paths take seconds to complete — design around that budget.

#ifndef OREO_OCCT_SCOPE_GUARD_H
#define OREO_OCCT_SCOPE_GUARD_H

#include "thread_safety.h"

#include <mutex>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

namespace oreo {

class KernelContext;

// ── Per-context mutex registry (OG-1) ────────────────────────
//
// Implementation detail: we intern one mutex per opaque context key in
// a process-wide map, guarded by a brief registry lock. The registry
// grows monotonically — we never erase mutexes, because a guard might
// still be queued up waiting on one even after its "last" user destroys
// the context. The memory overhead is one mutex per context that has
// ever held a guard; for realistic workloads (dozens of contexts, not
// millions) this is negligible.
std::mutex& occtStaticMutexFor(const void* key);

// Back-compat: legacy zero-arg callers share a single "nullptr" mutex.
std::mutex& occtStaticMutex();

// ── RAII guard ───────────────────────────────────────────────

class OREO_NOT_THREAD_SAFE OcctStaticGuard {
public:
    // Explicit per-context constructor. Takes the context by reference
    // and uses its address as the opaque key.
    explicit OcctStaticGuard(const KernelContext& ctx);

    // Opaque-key constructor. Useful when the caller has no KernelContext
    // handy but still wants per-identity serialisation.
    explicit OcctStaticGuard(const void* contextKey);

    // Back-compat default constructor — shares the legacy global mutex
    // with all other default-constructed guards in the process.
    OcctStaticGuard();

    // Destructor (OG-4): never throws. Each restore is tried under a
    // local try/catch; failures are appended to restoreFailures_.
    ~OcctStaticGuard();

    // ── Setters (OG-3) ───────────────────────────────────────
    // Each setter saves the old value (if any) before overwriting.

    // String-valued OCCT static
    void set(const char* key, const char* value);
    void set(const char* key, const std::string& value);

    // Integer-valued OCCT static (Interface_Static::IVal / SetIVal)
    void set(const char* key, int value);

    // Real-valued OCCT static (Interface_Static::RVal / SetRVal)
    void set(const char* key, double value);

    // ── Diagnostics (OG-2, OG-4) ─────────────────────────────

    // Keys that were originally unset (CVal/IVal/RVal returned a sentinel).
    // On restore we can only write an empty string / zero, which is NOT
    // equivalent to "unset" in OCCT semantics.
    const std::vector<std::string>& unsetKeys() const { return unsetKeys_; }

    // Messages collected during destruction for keys whose restore threw.
    // Consult this AFTER the guard's scope if you care about clean teardown.
    const std::vector<std::string>& restoreFailures() const { return restoreFailures_; }

    // No copy/move — the lock is non-movable and the key/registry pairing
    // is tied to this object's lifetime.
    OcctStaticGuard(const OcctStaticGuard&) = delete;
    OcctStaticGuard& operator=(const OcctStaticGuard&) = delete;
    OcctStaticGuard(OcctStaticGuard&&) = delete;
    OcctStaticGuard& operator=(OcctStaticGuard&&) = delete;

private:
    // Tagged record of a saved setting (OG-3).
    struct Saved {
        std::string key;
        std::variant<std::string, int, double> value;
    };

    std::unique_lock<std::mutex> lock_;
    std::vector<Saved> savedSettings_;
    // Set of keys already saved, so a repeated set() on the same key never
    // records a second (post-write) snapshot. Without this de-dup, restore
    // order would leave the intermediate value, not the original. Keeping
    // the first snapshot only is sufficient because the original is what
    // we must restore regardless of how many times the caller overwrites.
    std::unordered_set<std::string> savedKeys_;
    std::vector<std::string> unsetKeys_;
    std::vector<std::string> restoreFailures_;

    // Helpers — implemented in .cpp so Interface_Static headers stay out
    // of this public header. Each helper is a no-op if `key` was already
    // saved earlier in this guard's lifetime (first-wins semantics).
    void saveString_(const char* key);
    void saveInt_(const char* key);
    void saveDouble_(const char* key);
};

} // namespace oreo

#endif // OREO_OCCT_SCOPE_GUARD_H
