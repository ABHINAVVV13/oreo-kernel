// config_loader.cpp — CC-10 implementation.
//
// Env lookup uses std::getenv (MSVC emits C4996 on this; silence via
// _CRT_SECURE_NO_WARNINGS locally so we don't alter the whole TU's
// warning policy). File loads read a JSON object via nlohmann::json and
// dispatch each recognised key into KernelConfig.

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "config_loader.h"

#include "kernel_context.h" // KernelConfig full definition

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace oreo {

// ─── Env helpers ─────────────────────────────────────────────

std::optional<std::string> ConfigLoader::envString(const char* name) {
    if (!name || *name == '\0') return std::nullopt;
    const char* raw = std::getenv(name);
    if (!raw || *raw == '\0') return std::nullopt;
    return std::string(raw);
}

std::optional<double> ConfigLoader::envDouble(const char* name) {
    auto s = envString(name);
    if (!s) return std::nullopt;
    try {
        size_t idx = 0;
        double v = std::stod(*s, &idx);
        // Tolerate trailing whitespace but reject trailing junk.
        while (idx < s->size() && std::isspace(static_cast<unsigned char>((*s)[idx]))) ++idx;
        if (idx != s->size()) return std::nullopt;
        return v;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<int64_t> ConfigLoader::envInt(const char* name) {
    auto s = envString(name);
    if (!s) return std::nullopt;
    try {
        size_t idx = 0;
        int64_t v = static_cast<int64_t>(std::stoll(*s, &idx, 10));
        while (idx < s->size() && std::isspace(static_cast<unsigned char>((*s)[idx]))) ++idx;
        if (idx != s->size()) return std::nullopt;
        return v;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<bool> ConfigLoader::envBool(const char* name) {
    auto s = envString(name);
    if (!s) return std::nullopt;
    std::string t = *s;
    std::transform(t.begin(), t.end(), t.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (t == "1" || t == "true" || t == "yes" || t == "on") return true;
    if (t == "0" || t == "false" || t == "no" || t == "off") return false;
    return std::nullopt;
}

// ─── Env overlay ─────────────────────────────────────────────

void ConfigLoader::applyEnvOverlay(KernelConfig& config) {
    if (auto v = envDouble("OREO_LINEAR_PRECISION"))
        config.tolerance.linearPrecision = *v;
    if (auto v = envDouble("OREO_ANGULAR_PRECISION"))
        config.tolerance.angularPrecision = *v;
    if (auto v = envDouble("OREO_BOOLEAN_FUZZY_FACTOR"))
        config.tolerance.booleanFuzzyFactor = *v;
    if (auto v = envDouble("OREO_MIN_EDGE_LENGTH"))
        config.tolerance.minEdgeLength = *v;

    if (auto v = envInt("OREO_TAG_SEED"))
        config.tagSeed = *v;
    if (auto v = envInt("OREO_DOCUMENT_ID"))
        // KernelConfig::documentId is uint64_t. envInt returns int64_t;
        // reinterpret as unsigned to preserve the full 64-bit value when
        // callers encode high bits via a two's-complement representation.
        config.documentId = static_cast<uint64_t>(*v);

    if (auto v = envString("OREO_DOCUMENT_UUID"))
        config.documentUUID = *v;

    if (auto v = envBool("OREO_INIT_OCCT"))
        config.initOCCT = *v;
}

// ─── JSON file overlay ───────────────────────────────────────

namespace {

// Pull a double from `j[key]` into `out` if present and numeric.
template <class T>
void maybeAssignNumber(const nlohmann::json& j, const char* key, T& out) {
    auto it = j.find(key);
    if (it == j.end()) return;
    if (!it->is_number()) return;
    out = it->get<T>();
}

void maybeAssignString(const nlohmann::json& j, const char* key, std::string& out) {
    auto it = j.find(key);
    if (it == j.end()) return;
    if (!it->is_string()) return;
    out = it->get<std::string>();
}

void maybeAssignBool(const nlohmann::json& j, const char* key, bool& out) {
    auto it = j.find(key);
    if (it == j.end()) return;
    if (!it->is_boolean()) return;
    out = it->get<bool>();
}

} // namespace

void ConfigLoader::applyJsonFile(KernelConfig& config, const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        throw std::runtime_error("ConfigLoader: cannot open '" + path + "'");
    }
    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        throw std::runtime_error(
            "ConfigLoader: failed to parse '" + path + "': " + e.what());
    }
    if (!j.is_object()) {
        throw std::runtime_error(
            "ConfigLoader: top-level JSON in '" + path + "' must be an object");
    }

    // Flat schema aligned with KernelConfig fields. Accept both bare
    // tolerance keys and a nested "tolerance" object for readability.
    maybeAssignNumber(j, "linearPrecision",     config.tolerance.linearPrecision);
    maybeAssignNumber(j, "angularPrecision",    config.tolerance.angularPrecision);
    maybeAssignNumber(j, "booleanFuzzyFactor",  config.tolerance.booleanFuzzyFactor);
    maybeAssignNumber(j, "minEdgeLength",       config.tolerance.minEdgeLength);

    auto tolIt = j.find("tolerance");
    if (tolIt != j.end() && tolIt->is_object()) {
        maybeAssignNumber(*tolIt, "linearPrecision",    config.tolerance.linearPrecision);
        maybeAssignNumber(*tolIt, "angularPrecision",   config.tolerance.angularPrecision);
        maybeAssignNumber(*tolIt, "booleanFuzzyFactor", config.tolerance.booleanFuzzyFactor);
        maybeAssignNumber(*tolIt, "minEdgeLength",      config.tolerance.minEdgeLength);
    }

    maybeAssignNumber(j, "tagSeed",    config.tagSeed);
    // documentId is uint64_t — preserve the full 64 bits from the JSON
    // integer. JSON-RFC-8259 integers are unbounded but nlohmann::json
    // stores them as int64_t / uint64_t; accept either representation.
    {
        auto it = j.find("documentId");
        if (it != j.end()) {
            if (it->is_number_unsigned()) {
                config.documentId = it->get<uint64_t>();
            } else if (it->is_number_integer()) {
                config.documentId = static_cast<uint64_t>(it->get<int64_t>());
            }
            // Non-integer number values (e.g. 1.5) are silently ignored,
            // consistent with maybeAssignNumber's is_number() gating.
        }
    }
    maybeAssignString(j, "documentUUID", config.documentUUID);
    maybeAssignBool(j, "initOCCT", config.initOCCT);
}

} // namespace oreo
