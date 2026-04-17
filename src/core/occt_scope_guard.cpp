// occt_scope_guard.cpp — implementation for OcctStaticGuard.
//
// Keeps OCCT headers and the per-context mutex registry out of the
// public header. The registry is a process-wide std::unordered_map
// from opaque context keys to mutexes, guarded by a brief lock.

#include "occt_scope_guard.h"

#include "kernel_context.h"

#include <Interface_Static.hxx>

#include <mutex>
#include <string>
#include <unordered_map>

namespace oreo {

// ── Per-context mutex registry (OG-1) ────────────────────────
//
// We intern one std::mutex per opaque key. Using unique_ptr because
// std::mutex is neither copyable nor movable, and rehashing the map
// on growth would invalidate stored mutex references. We never remove
// entries — a mutex might still be held or queued on, and the registry
// would otherwise need reference counting we don't want to pay for.

namespace {

std::mutex& registryMutex_() {
    static std::mutex m;
    return m;
}

std::unordered_map<const void*, std::unique_ptr<std::mutex>>& registry_() {
    static std::unordered_map<const void*, std::unique_ptr<std::mutex>> m;
    return m;
}

} // namespace

std::mutex& occtStaticMutexFor(const void* key) {
    std::lock_guard<std::mutex> lock(registryMutex_());
    auto& reg = registry_();
    auto it = reg.find(key);
    if (it == reg.end()) {
        auto inserted = reg.emplace(key, std::make_unique<std::mutex>());
        it = inserted.first;
    }
    return *it->second;
}

std::mutex& occtStaticMutex() {
    // Back-compat: all legacy default-constructed guards share the same
    // "nullptr key" slot.
    return occtStaticMutexFor(nullptr);
}

// ── OcctStaticGuard ──────────────────────────────────────────

OcctStaticGuard::OcctStaticGuard(const KernelContext& ctx)
    : lock_(occtStaticMutexFor(static_cast<const void*>(&ctx))) {}

OcctStaticGuard::OcctStaticGuard(const void* contextKey)
    : lock_(occtStaticMutexFor(contextKey)) {}

OcctStaticGuard::OcctStaticGuard()
    : lock_(occtStaticMutexFor(nullptr)) {}

OcctStaticGuard::~OcctStaticGuard() {
    // (OG-4) Restore each saved setting; every step is try/catch because
    // OCCT can throw Standard_Failure from SetCVal/SetIVal/SetRVal (bad
    // values, unknown keys, readonly statics). We must not let any of
    // those escape a destructor.
    for (auto& s : savedSettings_) {
        try {
            if (std::holds_alternative<std::string>(s.value)) {
                const std::string& v = std::get<std::string>(s.value);
                Interface_Static::SetCVal(s.key.c_str(), v.c_str());
            } else if (std::holds_alternative<int>(s.value)) {
                Interface_Static::SetIVal(s.key.c_str(), std::get<int>(s.value));
            } else {
                Interface_Static::SetRVal(s.key.c_str(), std::get<double>(s.value));
            }
        } catch (const std::exception& e) {
            try {
                restoreFailures_.push_back(
                    std::string("Failed to restore ") + s.key + ": " + e.what());
            } catch (...) {
                // If we can't even record the failure, swallow silently
                // — we're already unwinding a destructor.
            }
        } catch (...) {
            try {
                restoreFailures_.push_back(
                    std::string("Failed to restore ") + s.key + ": unknown exception");
            } catch (...) {
                // swallow
            }
        }
    }

    // (OG-2) For keys that were originally unset, we already pushed a
    // warning message into restoreFailures_ at save time — nothing
    // further to do here. OCCT has no "unset" operation, so the empty
    // string / zero we wrote in the loop above is the best we can do.
}

// ── Save helpers ─────────────────────────────────────────────

// De-dup note:
//   savedKeys_ tracks which keys we've already snapshotted. A repeated
//   set(K, ...) after an earlier set(K, ...) must NOT record a second
//   snapshot — that would capture the intermediate value and, on restore
//   (which walks savedSettings_ in insertion order), leave the process
//   at the intermediate value instead of the true original. First-wins
//   preserves the invariant "destructor restores original state."

void OcctStaticGuard::saveString_(const char* key) {
    const std::string keyStr(key ? key : "");
    if (!savedKeys_.insert(keyStr).second) {
        // Already saved earlier — keep the original snapshot; skip.
        return;
    }
    // IsSet tells us whether the key has been Init'd at all; CVal returns
    // empty string for unset keys, which is ambiguous with "set to empty".
    const bool wasSet = Interface_Static::IsSet(key) != Standard_False;
    const char* current = Interface_Static::CVal(key);
    if (!wasSet) {
        unsetKeys_.emplace_back(keyStr);
        try {
            restoreFailures_.emplace_back(
                "Key '" + keyStr +
                "' was originally unset; OCCT cannot truly unset it on restore");
        } catch (...) {
            // best-effort
        }
    }
    savedSettings_.push_back({
        keyStr,
        std::string(current ? current : "")
    });
}

void OcctStaticGuard::saveInt_(const char* key) {
    const std::string keyStr(key ? key : "");
    if (!savedKeys_.insert(keyStr).second) return;
    const bool wasSet = Interface_Static::IsSet(key) != Standard_False;
    const int current = Interface_Static::IVal(key);
    if (!wasSet) {
        unsetKeys_.emplace_back(keyStr);
        try {
            restoreFailures_.emplace_back(
                "Key '" + keyStr +
                "' was originally unset (int); restore will write 0, not unset");
        } catch (...) {
            // best-effort
        }
    }
    savedSettings_.push_back({ keyStr, current });
}

void OcctStaticGuard::saveDouble_(const char* key) {
    const std::string keyStr(key ? key : "");
    if (!savedKeys_.insert(keyStr).second) return;
    const bool wasSet = Interface_Static::IsSet(key) != Standard_False;
    const double current = Interface_Static::RVal(key);
    if (!wasSet) {
        unsetKeys_.emplace_back(keyStr);
        try {
            restoreFailures_.emplace_back(
                "Key '" + keyStr +
                "' was originally unset (double); restore will write 0.0, not unset");
        } catch (...) {
            // best-effort
        }
    }
    savedSettings_.push_back({ keyStr, current });
}

// ── Setters (OG-3) ───────────────────────────────────────────

void OcctStaticGuard::set(const char* key, const char* value) {
    saveString_(key);
    Interface_Static::SetCVal(key, value ? value : "");
}

void OcctStaticGuard::set(const char* key, const std::string& value) {
    saveString_(key);
    Interface_Static::SetCVal(key, value.c_str());
}

void OcctStaticGuard::set(const char* key, int value) {
    saveInt_(key);
    Interface_Static::SetIVal(key, value);
}

void OcctStaticGuard::set(const char* key, double value) {
    saveDouble_(key);
    Interface_Static::SetRVal(key, value);
}

} // namespace oreo
