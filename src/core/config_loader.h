// SPDX-License-Identifier: LGPL-2.1-or-later

// config_loader.h — CC-10: overlay runtime config from env / JSON file.
//
// ConfigLoader mutates a KernelConfig in place by reading values from
// the process environment (applyEnvOverlay) and from a JSON file on
// disk (applyJsonFile). Both overlays are non-destructive: fields not
// present in the source are left untouched, letting the caller stack
// defaults + file + env in that order for typical "config.json with env
// overrides" workflows.
//
// Recognised environment variables:
//   OREO_LINEAR_PRECISION      → tolerance.linearPrecision (double)
//   OREO_ANGULAR_PRECISION     → tolerance.angularPrecision (double)
//   OREO_BOOLEAN_FUZZY_FACTOR  → tolerance.booleanFuzzyFactor (double)
//   OREO_MIN_EDGE_LENGTH       → tolerance.minEdgeLength (double)
//   OREO_TAG_SEED              → tagSeed (int64_t)
//   OREO_DOCUMENT_ID           → documentId (uint32_t, read as int64)
//   OREO_DOCUMENT_UUID         → documentUUID (string)
//   OREO_INIT_OCCT             → initOCCT (bool: "1"/"0"/"true"/"false")
//
// applyJsonFile accepts the same keys at the top level of a JSON object
// (e.g. {"linearPrecision": 1e-8, "tagSeed": 1000}).

#ifndef OREO_CONFIG_LOADER_H
#define OREO_CONFIG_LOADER_H

#include <cstdint>
#include <optional>
#include <string>

namespace oreo {

struct KernelConfig; // fwd decl; full definition lives in kernel_context.h

class ConfigLoader {
public:
    // Overlay KernelConfig fields from the process environment.
    // Unknown / empty variables are ignored. Malformed values trigger
    // no exception (best-effort overlay) — callers that need stricter
    // diagnostics should validate KernelConfig after the call.
    static void applyEnvOverlay(KernelConfig& config);

    // Overlay KernelConfig fields from a JSON file on disk. Throws
    // std::runtime_error if the file cannot be opened or the payload is
    // not a JSON object. Individual field parse failures are logged via
    // exceptions from nlohmann::json where it throws — callers should
    // wrap applyJsonFile in try/catch when dealing with user-supplied
    // files.
    static void applyJsonFile(KernelConfig& config, const std::string& path);

    // Low-level env helpers. Return std::nullopt when the variable is
    // unset, empty, or fails to parse into the requested type.
    static std::optional<double> envDouble(const char* name);
    static std::optional<int64_t> envInt(const char* name);
    static std::optional<std::string> envString(const char* name);
    static std::optional<bool> envBool(const char* name);
};

} // namespace oreo

#endif // OREO_CONFIG_LOADER_H
