// occt_scope_guard.h — RAII guards for OCCT process-global settings.
//
// OCCT's Interface_Static stores settings globally. When two threads
// or two contexts use different settings (e.g., STEP unit), they can
// corrupt each other. This guard saves and restores settings on scope exit.
//
// Usage:
//   {
//       OcctStaticGuard guard;
//       guard.set("write.step.unit", "MM");
//       // ... do STEP export ...
//   }  // Original settings restored here

#ifndef OREO_OCCT_SCOPE_GUARD_H
#define OREO_OCCT_SCOPE_GUARD_H

#include "thread_safety.h"

#include <Interface_Static.hxx>

#include <mutex>
#include <string>
#include <vector>

namespace oreo {

// Process-wide mutex for OCCT Interface_Static access.
// All STEP I/O operations must hold this mutex.
inline std::mutex& occtStaticMutex() {
    static std::mutex mtx;
    return mtx;
}

// RAII guard that locks the OCCT static mutex and restores settings on exit.
class OREO_NOT_THREAD_SAFE OcctStaticGuard {
public:
    OcctStaticGuard() : lock_(occtStaticMutex()) {}

    ~OcctStaticGuard() {
        // Restore all saved settings
        for (auto& [key, val] : savedSettings_) {
            Interface_Static::SetCVal(key.c_str(), val.c_str());
        }
    }

    // Set a string setting (saves the old value for restoration)
    void set(const char* key, const char* value) {
        // Save current value
        const char* current = Interface_Static::CVal(key);
        if (current) {
            savedSettings_.emplace_back(key, current);
        } else {
            savedSettings_.emplace_back(key, "");
        }
        Interface_Static::SetCVal(key, value);
    }

    // No copy/move
    OcctStaticGuard(const OcctStaticGuard&) = delete;
    OcctStaticGuard& operator=(const OcctStaticGuard&) = delete;

private:
    std::lock_guard<std::mutex> lock_;
    std::vector<std::pair<std::string, std::string>> savedSettings_;
};

} // namespace oreo

#endif // OREO_OCCT_SCOPE_GUARD_H
