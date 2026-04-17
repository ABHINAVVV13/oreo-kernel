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

#include <cstddef>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// SCH-9: Allow CMake to override the kernel version via a
// target_compile_definitions(-D OREO_KERNEL_VERSION_STRING="...") macro.
// TODO(integration): wire this macro in CMakeLists.txt via
//     target_compile_definitions(oreo-kernel PUBLIC
//         OREO_KERNEL_VERSION_STRING="${PROJECT_VERSION}")
// so the literal stays in sync with the project version automatically.
#ifndef OREO_KERNEL_VERSION_STRING
#define OREO_KERNEL_VERSION_STRING "0.2.0"
#endif

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

    // SCH-5: strict integer validation — do not silently coerce floats.
    static SchemaVersion fromJSON(const nlohmann::json& j) {
        if (!j.contains("major") || !j.contains("minor") || !j.contains("patch"))
            throw std::invalid_argument("SchemaVersion missing required fields");
        if (!j.at("major").is_number_integer())
            throw std::runtime_error("SchemaVersion: 'major' must be integer");
        if (!j.at("minor").is_number_integer())
            throw std::runtime_error("SchemaVersion: 'minor' must be integer");
        if (!j.at("patch").is_number_integer())
            throw std::runtime_error("SchemaVersion: 'patch' must be integer");
        return {j.at("major").get<int>(), j.at("minor").get<int>(), j.at("patch").get<int>()};
    }
};

// ─── Current schema versions for each type ───────────────────

namespace schema {
    // Kernel version (SCH-9: compile-time overridable via OREO_KERNEL_VERSION_STRING).
    constexpr const char* KERNEL_VERSION = OREO_KERNEL_VERSION_STRING;

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

    // Register a migration from one version to another.
    // SCH-3: throws std::logic_error if a migration for (type, from) is
    // already registered — migration registration happens at ctor time
    // where throwing is safe and duplicates are always a programming bug.
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

    // SCH-2: Configure the maximum number of migration steps walked by
    // migrate() / canLoad() before aborting with a cycle/depth error.
    // Default is 256 — bumped up from the legacy hardcoded 100.
    void setMaxMigrationSteps(size_t steps) noexcept { maxMigrationSteps_ = steps; }
    size_t maxMigrationSteps() const noexcept { return maxMigrationSteps_; }

    // SCH-8: Introspection helpers for tooling / debugging.
    // registeredTypes(): every type that has a current version entry.
    // versionsFor(type): every `from` version registered as a migration
    //                    source for `type` (order of registration).
    // unregisterMigration: drop a specific (type, from) migration; returns
    //                      true if removed, false if no such migration.
    std::vector<std::string> registeredTypes() const;
    std::vector<SchemaVersion> versionsFor(const std::string& type) const;
    bool unregisterMigration(const std::string& type, const SchemaVersion& from);

    // Add schema version headers to serialized JSON.
    // SCH-6: if `data` already contains `_schema` / `_version` / `_kernelVersion`
    // fields, they must match the new values or this throws. Silent
    // overwrites are no longer allowed.
    static nlohmann::json addHeader(const std::string& type,
                                    SchemaVersion version,
                                    nlohmann::json data);

    // Extract and validate schema version from JSON
    struct HeaderInfo {
        std::string type;
        SchemaVersion version;
        bool valid;
        // SCH-7: populated when `_kernelVersion` is present in the header.
        // Callers can compare against `schema::KERNEL_VERSION` and warn.
        std::string kernelVersion;
        bool kernelVersionPresent = false;
    };
    static HeaderInfo readHeader(const nlohmann::json& data);

    // SCH-7: Records the `_kernelVersion` string read from the most
    // recent migrate() call. Empty when no header has been inspected
    // yet or the header omitted the field. (canLoad() only receives a
    // SchemaVersion, not the full JSON, so it cannot update this.)
    // Callers can query after load to compare against
    // schema::KERNEL_VERSION and warn.
    const std::string& loadedFromKernelVersion() const noexcept {
        return loadedFromKernelVersion_;
    }

    // SCH-10: Round-trip test helper. Runs `sampleData` through migrate()
    // from `from` to `to`, then shape-checks the result:
    //   - has `_schema` matching `type`
    //   - has `_version` matching `to`
    // Returns true on success, false on any validation failure. A
    // human-readable reason is written to `reasonOut` when non-null.
    bool testMigration(const std::string& type,
                       SchemaVersion from,
                       SchemaVersion to,
                       const nlohmann::json& sampleData,
                       std::string* reasonOut = nullptr) const;

private:
    std::map<std::string, SchemaVersion> currentVersions_;

    // SCH-4: Linear scan over migrations_ per step is O(n). Keep the
    // vector for ordered introspection (registration order matters for
    // debugging) and mirror into migrationMap_ for O(1) lookup at
    // migrate() time. Composite key = type + "#" + from.toString()
    // avoids the need for a std::hash<SchemaVersion> specialization.
    std::vector<Migration> migrations_;
    std::unordered_map<std::string, MigrationFn> migrationMap_;
    std::unordered_map<std::string, SchemaVersion> migrationTo_;

    // SCH-2: configurable migration-step cap (was hardcoded 100).
    size_t maxMigrationSteps_ = 256;

    // SCH-7: most recent `_kernelVersion` seen in a header (mutable so
    // const migrate() can update it).
    mutable std::string loadedFromKernelVersion_;

    // SCH-4: build composite key used by migrationMap_ / migrationTo_.
    static std::string composeKey(const std::string& type, const SchemaVersion& from) {
        return type + "#" + from.toString();
    }
};

} // namespace oreo

#endif // OREO_SCHEMA_H
