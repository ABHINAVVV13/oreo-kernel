// feature_flags.h — MISSING-12: runtime feature toggle registry.
//
// FeatureFlags is a process-wide, mutex-guarded map of flag name -> bool.
// Intended uses:
//
//   - Gate experimental code paths without a recompile.
//   - Let integration tests flip behaviour per run via OREO_FEATURE_* env
//     vars.
//   - Let the hosting application persist user preferences that the kernel
//     consults at operation time.
//
// Flags are queried with isEnabled(); missing flags are treated as disabled
// (false). Iteration over a stable view is available via snapshot().
//
// Environment overlay:
//   loadFromEnvironment() walks the process environment and registers any
//   variable whose name starts with OREO_FEATURE_. The suffix (after the
//   prefix) becomes the flag name, lowercased. Values "1", "true", "yes",
//   "on" enable; "0", "false", "no", "off" disable; other values are
//   ignored so a stray env var doesn't silently flip behaviour.

#ifndef OREO_FEATURE_FLAGS_H
#define OREO_FEATURE_FLAGS_H

#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

namespace oreo {

class FeatureFlags {
public:
    static FeatureFlags& instance();

    // Returns true only if the flag is registered and set to true. Unknown
    // flags return false — the safe default for a feature gate.
    bool isEnabled(const std::string& flag) const;

    // Register or update a flag. Persists until clear() or process exit.
    void set(const std::string& flag, bool enabled);

    // Remove a flag. Subsequent isEnabled() calls return false.
    void clear(const std::string& flag);

    // Stable copy of all currently-registered flags.
    std::unordered_map<std::string, bool> snapshot() const;

    // Scan the process environment for OREO_FEATURE_* variables and apply
    // them. See header banner for the accepted value syntax.
    //
    // On Windows the scan uses GetEnvironmentStringsW; on POSIX it walks the
    // `environ` pointer. Safe to call repeatedly — later env values override
    // earlier programmatic set() calls for the same flag.
    void loadFromEnvironment();

    // Apply a single env var to a flag name. Used by loadFromEnvironment()
    // and also exposed for callers that want to opt in explicitly without
    // scanning the whole environment. Returns true if the env var was
    // present and parsed into a boolean.
    bool loadFlag(const char* envName, const char* flagName);

private:
    mutable std::mutex mtx_;
    std::unordered_map<std::string, bool> flags_;
};

inline bool featureEnabled(const std::string& flag) {
    return FeatureFlags::instance().isEnabled(flag);
}

} // namespace oreo

#endif // OREO_FEATURE_FLAGS_H
