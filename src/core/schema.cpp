// schema.cpp — Fail-closed SchemaRegistry implementation.
// Unknown types, missing headers, bad versions, and missing migrations
// all produce structured errors, never silent acceptance.

#include "schema.h"

#include <algorithm>
#include <set>
#include <sstream>
#include <stdexcept>

namespace oreo {

SchemaVersion SchemaVersion::parse(const std::string& s) {
    if (s.empty()) {
        throw std::invalid_argument("Cannot parse schema version from empty string");
    }

    SchemaVersion v = {0, 0, 0};
    int pos = 0;
    int field = 0;

    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '.') {
            if (pos >= (int)i) {
                throw std::invalid_argument(
                    "Malformed schema version (empty segment): '" + s + "'");
            }
            std::string segment = s.substr(pos, i - pos);
            // Validate that segment contains only digits
            for (char c : segment) {
                if (c < '0' || c > '9') {
                    throw std::invalid_argument(
                        "Malformed schema version (non-digit character): '" + s + "'");
                }
            }
            // SCH-1: std::stoi throws std::out_of_range for values beyond
            // INT_MAX (e.g. "999999999999"). Previously this propagated as
            // an uncaught std::out_of_range; wrap to translate into the
            // domain-specific std::invalid_argument.
            int val = 0;
            try {
                val = std::stoi(segment);
            } catch (const std::invalid_argument&) {
                throw std::invalid_argument(
                    "Malformed schema version (invalid integer): '" + s + "'");
            } catch (const std::out_of_range&) {
                throw std::invalid_argument(
                    "Malformed schema version (integer overflow): '" + s + "'");
            }
            if (field == 0) v.major = val;
            else if (field == 1) v.minor = val;
            else if (field == 2) v.patch = val;
            else {
                throw std::invalid_argument(
                    "Malformed schema version (too many segments): '" + s + "'");
            }
            pos = static_cast<int>(i) + 1;
            ++field;
        }
    }

    // Require exactly 3 segments: major.minor.patch
    if (field != 3) {
        throw std::invalid_argument(
            "Malformed schema version (expected major.minor.patch): '" + s + "'");
    }
    return v;
}

SchemaRegistry::SchemaRegistry() {
    // Register current versions for ALL known types
    currentVersions_[schema::TYPE_FEATURE_TREE] = schema::FEATURE_TREE;
    currentVersions_[schema::TYPE_FEATURE] = schema::FEATURE;
    currentVersions_[schema::TYPE_ELEMENT_MAP] = schema::ELEMENT_MAP;
    currentVersions_[schema::TYPE_NAMED_SHAPE] = schema::NAMED_SHAPE;
    // Also register types that were previously missing
    currentVersions_["oreo.tolerance"] = schema::TOLERANCE;
    currentVersions_["oreo.unit_system"] = schema::UNIT_SYSTEM;
    currentVersions_["oreo.kernel_context"] = schema::KERNEL_CONTEXT;
}

namespace {
// Local helper: matches a Migration by (type, from) — used by both
// registerMigration's duplicate check and unregisterMigration's find.
inline bool sameFrom(const Migration& m, const std::string& type, const SchemaVersion& from) {
    return m.type == type && m.from == from;
}
} // namespace

void SchemaRegistry::registerMigration(const std::string& type,
                                       SchemaVersion from, SchemaVersion to,
                                       MigrationFn migrator) {
    // SCH-3: reject duplicate (type, from) migrations loudly at
    // registration time. Silent overwrites mask real bugs.
    for (const auto& existing : migrations_) {
        if (sameFrom(existing, type, from)) {
            throw std::logic_error(
                "Duplicate migration for type '" + type
                + "' from version " + from.toString());
        }
    }

    migrations_.push_back({type, from, to, std::move(migrator)});
    // SCH-4: also mirror into the lookup map for O(1) migrate() lookup.
    const auto& justAdded = migrations_.back();
    const std::string key = composeKey(type, from);
    migrationMap_[key] = justAdded.migrate;
    migrationTo_[key] = to;
}

nlohmann::json SchemaRegistry::migrate(const std::string& type,
                                       const nlohmann::json& data) const {
    auto header = readHeader(data);

    // SCH-7: capture kernel version from header (if present) so callers
    // can compare against schema::KERNEL_VERSION post-load.
    if (header.kernelVersionPresent) {
        loadedFromKernelVersion_ = header.kernelVersion;
    } else {
        loadedFromKernelVersion_.clear();
    }

    // FAIL-CLOSED: Missing header is an error, not silently accepted
    if (!header.valid) {
        throw std::runtime_error(
            "Schema header missing from serialized data for type '" + type + "'. "
            "Expected '_schema' and '_version' fields.");
    }

    // FAIL-CLOSED: Type mismatch
    if (header.type != type) {
        throw std::runtime_error(
            "Schema type mismatch: expected '" + type + "' but got '" + header.type + "'");
    }

    SchemaVersion current = currentVersion(type);

    // Already compatible
    if (current.canLoad(header.version)) return data;

    // FAIL-CLOSED: Future major version we can't load
    if (header.version.major > current.major) {
        throw std::runtime_error(
            "Cannot load future schema version " + header.version.toString()
            + " for type '" + type + "' (current: " + current.toString() + ")");
    }

    // Find migration chain (with cycle detection)
    nlohmann::json result = data;
    SchemaVersion ver = header.version;
    std::set<std::string> visited;

    // SCH-2: use the configurable cap instead of a hardcoded 100.
    size_t stepsRemaining = maxMigrationSteps_;
    while (ver != current && stepsRemaining-- > 0) {
        auto key = ver.toString();
        if (visited.count(key)) {
            throw std::runtime_error(
                "Cyclic schema migration detected at version " + key
                + " for type '" + type + "'");
        }
        visited.insert(key);

        // SCH-4: O(1) lookup via composite-keyed map.
        const std::string mk = composeKey(type, ver);
        auto mit = migrationMap_.find(mk);
        if (mit == migrationMap_.end()) {
            throw std::runtime_error(
                "No migration path from version " + ver.toString()
                + " to " + current.toString() + " for type '" + type + "'");
        }
        auto tit = migrationTo_.find(mk);
        if (tit == migrationTo_.end()) {
            // Should be impossible — migrationMap_ and migrationTo_ are
            // written together in registerMigration(). Treat as invariant
            // violation.
            throw std::logic_error(
                "Migration target missing for key '" + mk + "' (registry invariant violated)");
        }
        result = mit->second(result);
        ver = tit->second;
        if (result.contains("_version")) {
            result["_version"] = ver.toJSON();
        }
    }

    // SCH-2: if we exhausted the cap without reaching `current`, report.
    if (ver != current) {
        throw std::runtime_error(
            "Migration step limit reached (" + std::to_string(maxMigrationSteps_)
            + ") for type '" + type + "' before reaching version "
            + current.toString() + " (last version: " + ver.toString() + ")");
    }

    return result;
}

bool SchemaRegistry::canLoad(const std::string& type, SchemaVersion version) const {
    auto it = currentVersions_.find(type);
    if (it == currentVersions_.end()) return false;
    if (it->second.canLoad(version)) return true;

    // Check migration path (with cycle detection)
    SchemaVersion ver = version;
    std::set<std::string> visited;
    // SCH-2: use the configurable cap.
    size_t stepsRemaining = maxMigrationSteps_;
    while (ver != it->second && stepsRemaining-- > 0) {
        auto key = ver.toString();
        if (visited.count(key)) return false;  // cycle — no valid path
        visited.insert(key);

        // SCH-4: O(1) lookup via composite-keyed map.
        const std::string mk = composeKey(type, ver);
        auto tit = migrationTo_.find(mk);
        if (tit == migrationTo_.end()) return false;
        ver = tit->second;
    }
    return ver == it->second;
}

SchemaVersion SchemaRegistry::currentVersion(const std::string& type) const {
    auto it = currentVersions_.find(type);
    if (it != currentVersions_.end()) return it->second;
    // FAIL-CLOSED: unknown type is an error
    throw std::runtime_error("Unknown schema type: '" + type + "'");
}

nlohmann::json SchemaRegistry::addHeader(const std::string& type,
                                         SchemaVersion version,
                                         nlohmann::json data) {
    // SCH-6: do not silently overwrite existing headers. If the fields
    // already exist AND match the requested values, this is a no-op. If
    // they exist and disagree, throw — silently rewriting a document's
    // schema identity is a recipe for hard-to-diagnose corruption.
    const bool hadSchema = data.contains("_schema");
    const bool hadVersion = data.contains("_version");
    const bool hadKernel = data.contains("_kernelVersion");

    if (hadSchema) {
        if (!data["_schema"].is_string() || data["_schema"].get<std::string>() != type) {
            throw std::runtime_error(
                "addHeader: existing '_schema' differs from requested '" + type + "'");
        }
    }
    if (hadVersion) {
        // Compare via SchemaVersion to tolerate formatting differences.
        if (!data["_version"].is_object()) {
            throw std::runtime_error("addHeader: existing '_version' is not an object");
        }
        SchemaVersion existing;
        try {
            existing = SchemaVersion::fromJSON(data["_version"]);
        } catch (const std::exception& e) {
            throw std::runtime_error(
                std::string("addHeader: existing '_version' is malformed: ") + e.what());
        }
        if (existing != version) {
            throw std::runtime_error(
                "addHeader: existing '_version' " + existing.toString()
                + " differs from requested " + version.toString());
        }
    }
    if (hadKernel) {
        if (!data["_kernelVersion"].is_string()
            || data["_kernelVersion"].get<std::string>() != std::string(schema::KERNEL_VERSION)) {
            throw std::runtime_error(
                "addHeader: existing '_kernelVersion' differs from current '"
                + std::string(schema::KERNEL_VERSION) + "'");
        }
    }

    data["_schema"] = type;
    data["_version"] = version.toJSON();
    data["_kernelVersion"] = schema::KERNEL_VERSION;
    return data;
}

SchemaRegistry::HeaderInfo SchemaRegistry::readHeader(const nlohmann::json& data) {
    HeaderInfo info;
    info.valid = false;
    info.kernelVersionPresent = false;
    if (data.contains("_schema") && data["_schema"].is_string()
        && data.contains("_version") && data["_version"].is_object()) {
        info.type = data["_schema"].get<std::string>();
        info.version = SchemaVersion::fromJSON(data["_version"]);
        info.valid = true;
    }
    // SCH-7: extract kernel version independently — it may be present
    // even when the rest of the header is malformed (informational).
    if (data.contains("_kernelVersion") && data["_kernelVersion"].is_string()) {
        info.kernelVersion = data["_kernelVersion"].get<std::string>();
        info.kernelVersionPresent = true;
    }
    return info;
}

// ─── SCH-8: Introspection helpers ───────────────────────────

std::vector<std::string> SchemaRegistry::registeredTypes() const {
    std::vector<std::string> out;
    out.reserve(currentVersions_.size());
    for (const auto& kv : currentVersions_) out.push_back(kv.first);
    return out;
}

std::vector<SchemaVersion> SchemaRegistry::versionsFor(const std::string& type) const {
    std::vector<SchemaVersion> out;
    for (const auto& m : migrations_) {
        if (m.type == type) out.push_back(m.from);
    }
    return out;
}

bool SchemaRegistry::unregisterMigration(const std::string& type,
                                         const SchemaVersion& from) {
    auto it = std::find_if(migrations_.begin(), migrations_.end(),
        [&](const Migration& m) { return sameFrom(m, type, from); });
    if (it == migrations_.end()) return false;
    migrations_.erase(it);
    const std::string key = composeKey(type, from);
    migrationMap_.erase(key);
    migrationTo_.erase(key);
    return true;
}

// ─── SCH-10: Round-trip test helper ─────────────────────────

bool SchemaRegistry::testMigration(const std::string& type,
                                   SchemaVersion from,
                                   SchemaVersion to,
                                   const nlohmann::json& sampleData,
                                   std::string* reasonOut) const {
    auto setReason = [&](const std::string& s) {
        if (reasonOut) *reasonOut = s;
    };

    // Stamp the sample with the "from" header so migrate() accepts it.
    nlohmann::json stamped = sampleData;
    stamped["_schema"] = type;
    stamped["_version"] = from.toJSON();

    nlohmann::json migrated;
    try {
        migrated = migrate(type, stamped);
    } catch (const std::exception& e) {
        setReason(std::string("migrate() threw: ") + e.what());
        return false;
    }

    // Shape check: must have _schema matching `type` and _version matching `to`.
    if (!migrated.contains("_schema") || !migrated["_schema"].is_string()
        || migrated["_schema"].get<std::string>() != type) {
        setReason("migrated output missing or mismatched '_schema' field");
        return false;
    }
    if (!migrated.contains("_version") || !migrated["_version"].is_object()) {
        setReason("migrated output missing or malformed '_version' field");
        return false;
    }
    SchemaVersion actual;
    try {
        actual = SchemaVersion::fromJSON(migrated["_version"]);
    } catch (const std::exception& e) {
        setReason(std::string("migrated '_version' failed to parse: ") + e.what());
        return false;
    }
    if (actual != to) {
        setReason("migrated output version " + actual.toString()
                  + " does not match target " + to.toString());
        return false;
    }
    setReason("ok");
    return true;
}

} // namespace oreo
