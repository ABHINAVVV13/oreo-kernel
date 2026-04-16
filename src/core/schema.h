// schema.h — Versioned schemas for all persisted kernel objects.
//
// Every serializable type in oreo-kernel has a schema version.
// When loading old documents, the SchemaRegistry handles migration
// from older versions to the current version.
//
// Version format: major.minor.patch
//   major: breaking changes (requires migration)
//   minor: additive changes (backward compatible)
//   patch: bug fixes (no data format change)

#ifndef OREO_SCHEMA_H
#define OREO_SCHEMA_H

#include "thread_safety.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace oreo {

// ─── Schema Version ──────────────────────────────────────────

struct OREO_IMMUTABLE SchemaVersion {
    int major = 1;
    int minor = 0;
    int patch = 0;

    // Check if 'other' can be loaded by this version.
    // Compatible if: same major, other.minor <= this.minor
    bool canLoad(const SchemaVersion& other) const {
        return major == other.major && other.minor <= minor;
    }

    bool operator==(const SchemaVersion& o) const {
        return major == o.major && minor == o.minor && patch == o.patch;
    }
    bool operator!=(const SchemaVersion& o) const { return !(*this == o); }
    bool operator<(const SchemaVersion& o) const {
        if (major != o.major) return major < o.major;
        if (minor != o.minor) return minor < o.minor;
        return patch < o.patch;
    }

    std::string toString() const {
        return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    }

    static SchemaVersion parse(const std::string& s);

    nlohmann::json toJSON() const {
        return {{"major", major}, {"minor", minor}, {"patch", patch}};
    }

    static SchemaVersion fromJSON(const nlohmann::json& j) {
        return {j.value("major", 1), j.value("minor", 0), j.value("patch", 0)};
    }
};

// ─── Current schema versions for each type ───────────────────

namespace schema {
    // Kernel version
    constexpr const char* KERNEL_VERSION = "0.2.0";

    // Serializable types
    constexpr SchemaVersion FEATURE_TREE   = {1, 0, 0};
    constexpr SchemaVersion FEATURE        = {1, 0, 0};
    constexpr SchemaVersion ELEMENT_MAP    = {1, 0, 0};
    constexpr SchemaVersion NAMED_SHAPE    = {1, 0, 0};
    constexpr SchemaVersion TOLERANCE      = {1, 0, 0};
    constexpr SchemaVersion UNIT_SYSTEM    = {1, 0, 0};
    constexpr SchemaVersion KERNEL_CONTEXT = {1, 0, 0};

    // Type name constants for registry
    constexpr const char* TYPE_FEATURE_TREE = "oreo.feature_tree";
    constexpr const char* TYPE_FEATURE = "oreo.feature";
    constexpr const char* TYPE_ELEMENT_MAP = "oreo.element_map";
    constexpr const char* TYPE_NAMED_SHAPE = "oreo.named_shape";
}

// ─── Migration function type ─────────────────────────────────

using MigrationFn = std::function<nlohmann::json(const nlohmann::json&)>;

struct Migration {
    std::string type;
    SchemaVersion from;
    SchemaVersion to;
    MigrationFn migrate;
};

// ─── Schema Registry ─────────────────────────────────────────

class OREO_CONTEXT_BOUND SchemaRegistry {
public:
    SchemaRegistry();

    // Register a migration from one version to another
    void registerMigration(const std::string& type,
                          SchemaVersion from, SchemaVersion to,
                          MigrationFn migrator);

    // Migrate a document object to the current version.
    // The JSON must have "schema" and "version" fields.
    // Returns migrated JSON, or throws if migration path not found.
    nlohmann::json migrate(const std::string& type,
                          const nlohmann::json& data) const;

    // Check if a version is loadable (compatible or migratable)
    bool canLoad(const std::string& type, SchemaVersion version) const;

    // Get current version for a type
    SchemaVersion currentVersion(const std::string& type) const;

    // Add schema version headers to serialized JSON
    static nlohmann::json addHeader(const std::string& type,
                                    SchemaVersion version,
                                    nlohmann::json data);

    // Extract and validate schema version from JSON
    struct HeaderInfo {
        std::string type;
        SchemaVersion version;
        bool valid;
    };
    static HeaderInfo readHeader(const nlohmann::json& data);

private:
    std::map<std::string, SchemaVersion> currentVersions_;
    std::vector<Migration> migrations_;
};

} // namespace oreo

#endif // OREO_SCHEMA_H
