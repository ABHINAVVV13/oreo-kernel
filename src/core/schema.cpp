// schema.cpp — Fail-closed SchemaRegistry implementation.
// Unknown types, missing headers, bad versions, and missing migrations
// all produce structured errors, never silent acceptance.

#include "schema.h"

#include <set>
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
            int val = std::stoi(segment);
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

void SchemaRegistry::registerMigration(const std::string& type,
                                       SchemaVersion from, SchemaVersion to,
                                       MigrationFn migrator) {
    migrations_.push_back({type, from, to, std::move(migrator)});
}

nlohmann::json SchemaRegistry::migrate(const std::string& type,
                                       const nlohmann::json& data) const {
    auto header = readHeader(data);

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

    int maxSteps = 100;
    while (ver != current && maxSteps-- > 0) {
        auto key = ver.toString();
        if (visited.count(key)) {
            throw std::runtime_error(
                "Cyclic schema migration detected at version " + key
                + " for type '" + type + "'");
        }
        visited.insert(key);

        bool found = false;
        for (auto& m : migrations_) {
            if (m.type == type && m.from == ver) {
                result = m.migrate(result);
                ver = m.to;
                if (result.contains("_version")) {
                    result["_version"] = ver.toJSON();
                }
                found = true;
                break;
            }
        }
        if (!found) {
            throw std::runtime_error(
                "No migration path from version " + ver.toString()
                + " to " + current.toString() + " for type '" + type + "'");
        }
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
    int maxSteps = 100;
    while (ver != it->second && maxSteps-- > 0) {
        auto key = ver.toString();
        if (visited.count(key)) return false;  // cycle — no valid path
        visited.insert(key);

        bool found = false;
        for (auto& m : migrations_) {
            if (m.type == type && m.from == ver) {
                ver = m.to;
                found = true;
                break;
            }
        }
        if (!found) return false;
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
    data["_schema"] = type;
    data["_version"] = version.toJSON();
    data["_kernelVersion"] = schema::KERNEL_VERSION;
    return data;
}

SchemaRegistry::HeaderInfo SchemaRegistry::readHeader(const nlohmann::json& data) {
    HeaderInfo info;
    info.valid = false;
    if (data.contains("_schema") && data["_schema"].is_string()
        && data.contains("_version") && data["_version"].is_object()) {
        info.type = data["_schema"].get<std::string>();
        info.version = SchemaVersion::fromJSON(data["_version"]);
        info.valid = true;
    }
    return info;
}

} // namespace oreo
