// SPDX-License-Identifier: LGPL-2.1-or-later

// part_studio.cpp — Pure-function execute() + v2 JSON round-trip.
// See docs/part-studio-as-function.md for the surface design.

#include "part_studio.h"
#include "config.h"
#include "param_json.h"
#include "core/diagnostic.h"
#include "core/schema.h"

#include <nlohmann/json.hpp>

namespace oreo {

namespace {

// Schema tag for the PartStudio envelope. Top-level "_schema" field.
constexpr const char* kPartStudioType = "oreo.part_studio";

// Serialise a ConfigSchema to JSON: an array of input specs.
nlohmann::json configSchemaToJson(const ConfigSchema& schema) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& spec : schema.inputs()) {
        nlohmann::json entry;
        entry["name"]    = spec.name;
        entry["type"]    = paramTypeToString(spec.type);
        entry["default"] = paramValueToJson(spec.defaultValue);
        if (!spec.description.empty())
            entry["description"] = spec.description;
        if (spec.minInclusive) entry["min"] = *spec.minInclusive;
        if (spec.maxInclusive) entry["max"] = *spec.maxInclusive;
        arr.push_back(entry);
    }
    return arr;
}

// Deserialise a ConfigSchema. On any malformed entry, emits a
// diagnostic on ctx, rejects the input, and returns false. The schema
// is rebuilt from scratch — callers pass a fresh schema.
bool configSchemaFromJson(KernelContext& ctx,
                          const nlohmann::json& arr,
                          ConfigSchema& out) {
    if (!arr.is_array()) {
        ctx.diag().error(ErrorCode::DESERIALIZE_FAILED,
                         "PartStudio configSchema must be a JSON array");
        return false;
    }
    out.clear();
    for (std::size_t i = 0; i < arr.size(); ++i) {
        const auto& e = arr[i];
        if (!e.is_object()) {
            ctx.diag().error(ErrorCode::DESERIALIZE_FAILED,
                             "configSchema[" + std::to_string(i)
                             + "] must be an object");
            return false;
        }
        ConfigInputSpec spec;
        spec.name = e.value("name", std::string{});
        std::string typeStr = e.value("type", std::string{});
        bool typeOk = false;
        spec.type = paramTypeFromString(typeStr, &typeOk);
        if (!typeOk) {
            ctx.diag().error(ErrorCode::DESERIALIZE_FAILED,
                             "configSchema[" + std::to_string(i)
                             + "]: unknown type '" + typeStr + "'");
            return false;
        }
        if (e.contains("default")) {
            // Strict decode at the persistence trust boundary.
            //
            // Feature-tree params are already strict (feature_tree.cpp
            // fromJSON routes through paramValueFromJsonStrict) but the
            // legacy paramValueFromJson path used here was still lenient:
            // a malformed geometry default (e.g. a point missing 'z',
            // or a `{"type":"point"}` shape for a Vec-typed input) would
            // silently become a zero-ish value and pass addInput's
            // later type-match check. The audit flagged this as the
            // last silent-coercion surface in the kernel.
            //
            // The strict path rejects type-tag mismatches, NaN/Inf,
            // zero-magnitude directions, and float→int coercion.
            // addInput still runs its own bounds/dup-name checks after
            // the strict decode succeeds.
            std::string reason;
            auto decoded = paramValueFromJsonStrict(e["default"], spec.type, &reason);
            if (!decoded) {
                ctx.diag().error(ErrorCode::DESERIALIZE_FAILED,
                                 "configSchema[" + std::to_string(i)
                                 + "] (input '" + spec.name + "', type "
                                 + typeStr + "): default is malformed: "
                                 + reason);
                return false;
            }
            spec.defaultValue = std::move(*decoded);
        } else {
            // Construct a type-matched zero-ish default. Schema validation
            // at addInput() time will catch mismatches.
            switch (spec.type) {
                case ParamType::Double: spec.defaultValue = 0.0; break;
                case ParamType::Int:    spec.defaultValue = 0;   break;
                case ParamType::Bool:   spec.defaultValue = false;break;
                case ParamType::String: spec.defaultValue = std::string{}; break;
                default:
                    // Variant's default-constructed alternative is `double 0.0`,
                    // which won't match any non-Double type. Fail fast.
                    ctx.diag().error(ErrorCode::DESERIALIZE_FAILED,
                                     "configSchema[" + std::to_string(i)
                                     + "]: 'default' is required for type "
                                     + typeStr);
                    return false;
            }
        }
        spec.description = e.value("description", std::string{});
        if (e.contains("min") && e["min"].is_number()) {
            spec.minInclusive = e["min"].get<double>();
        }
        if (e.contains("max") && e["max"].is_number()) {
            spec.maxInclusive = e["max"].get<double>();
        }
        // addInput revalidates the default + bounds + duplicate-name check.
        int rc = out.addInput(ctx, spec);
        if (rc != static_cast<int>(ErrorCode::OK)) {
            return false;
        }
    }
    return true;
}

} // anonymous namespace

// ═════════════════════════════════════════════════════════════════
// PartStudio::execute
// ═════════════════════════════════════════════════════════════════

PartStudioOutputs PartStudio::execute(const ConfigValue& config) {
    PartStudioOutputs out;

    // Pre-flight: validate config against the declared schema. A bad
    // config is a hard fail — no feature dispatch happens because a
    // downstream ConfigRef resolving against invalid state would hide
    // the root cause.
    int cfgRc = validateConfig(*ctx_, schema_, config);
    if (cfgRc != static_cast<int>(ErrorCode::OK)) {
        out.status = PartStudioOutputs::Status::Failed;
        return out;
    }

    // RAII guard for the FeatureTransformer. tree_.replay() can throw
    // from an OCCT op or a quota trip — without this guard, a failed
    // execute would leave the transformer installed, and a later
    // non-config replay (or a second execute with a different config)
    // would capture either a stale `config` reference or the wrong
    // lambda. The guard's destructor runs on every exit path, including
    // exception unwind.
    struct TransformerScope {
        FeatureTree& tree;
        ~TransformerScope() { tree.setFeatureTransformer(nullptr); }
    } scope{tree_};

    tree_.setFeatureTransformer(
        [this, &config](const Feature& f) {
            return resolveConfigRefs(f, schema_, config);
        });

    // Full replay — configs can invalidate any subset of cached
    // results, and trying to track that would require hashing every
    // config value against each feature's ConfigRef use. Full replay
    // is deterministic + simple; incremental replay stays available
    // for internal callers who know config isn't in play (tree_.replayFrom).
    out.finalShape = tree_.replay();

    // Collect broken features.
    out.brokenFeatures = tree_.getBrokenFeatures();

    // Compute status.
    if (out.finalShape.isNull()) {
        out.status = PartStudioOutputs::Status::Failed;
    } else if (!out.brokenFeatures.empty()) {
        out.status = PartStudioOutputs::Status::Partial;
    } else {
        out.status = PartStudioOutputs::Status::Ok;
    }

    // Named outputs: today no built-in feature type is an Export, so
    // namedParts stays empty. The map exists on the ABI so a future
    // Export feature can populate it without a wire-format bump.
    // (Additive-only: readers that don't know Export still produce a
    // finalShape from whichever feature was last.)
    return out;
}

// ═════════════════════════════════════════════════════════════════
// JSON round-trip (v2 envelope)
// ═════════════════════════════════════════════════════════════════

std::string PartStudio::toJSON() const {
    using nlohmann::json;
    json doc;
    doc["_schema"]  = kPartStudioType;
    doc["_version"] = schema::PART_STUDIO.toJSON();
    doc["_kernelVersion"] = schema::KERNEL_VERSION;
    doc["name"]     = name_;
    doc["documentId"] = std::to_string(documentId());
    // Embed the FeatureTree JSON as a parsed sub-object so the file
    // stays human-readable without a quoted-string-inside-string.
    doc["tree"] = json::parse(tree_.toJSON());
    doc["configSchema"] = configSchemaToJson(schema_);
    return doc.dump();
}

PartStudio::FromJsonResult PartStudio::fromJSON(
    std::shared_ptr<KernelContext> ctx, const std::string& jsonStr) {
    FromJsonResult out;
    if (!ctx) {
        out.error = "PartStudio::fromJSON requires a non-null context";
        return out;
    }
    try {
        auto doc = nlohmann::json::parse(jsonStr);
        if (!doc.is_object()) {
            out.error = "PartStudio JSON root must be an object";
            return out;
        }

        // Three supported shapes:
        //   (a) v2 PartStudio envelope: {_schema, _version, name, documentId, configSchema, tree}
        //   (b) v1 PartStudio envelope: same, minus configSchema
        //   (c) bare FeatureTree JSON:  {features: [...]} or {_schema: "oreo.feature_tree", ...}
        std::string treeJson;
        std::string name;
        nlohmann::json configSchemaJson = nlohmann::json::array();
        bool haveStudioEnvelope = doc.contains("tree") && doc["tree"].is_object();

        // Fail-closed schema gate. The v2 envelope carries a _schema /
        // _version pair; if either is wrong or the major version is
        // beyond what this build can handle, abort. The SchemaRegistry
        // rejects unknown types, type/name mismatches, and future-major
        // documents; see src/core/schema.cpp:canLoad / migrate.
        //
        // Three shapes (analogous to FeatureTree::fromJSON gate):
        //   (A) no header at all       — pre-versioning legacy, accept.
        //   (B) _schema only            — early type-tagged legacy.
        //   (C) full _schema+_version   — enforce type + canLoad().
        //
        // Form (c) (bare FeatureTree) is allowed without a PartStudio
        // header because FeatureTree::fromJSON does its own schema gate
        // when the embedded tree is loaded below.
        if (haveStudioEnvelope) {
            const bool hasSchema  = doc.contains("_schema");
            const bool hasVersion = doc.contains("_version");
            if (hasSchema && !hasVersion) {
                // Shape (B): legacy type tag only.
                if (!doc["_schema"].is_string()) {
                    out.error = "PartStudio envelope has non-string _schema";
                    return out;
                }
                const std::string schemaStr = doc["_schema"].get<std::string>();
                if (schemaStr != kPartStudioType) {
                    out.error = "PartStudio envelope has wrong _schema: expected '"
                              + std::string(kPartStudioType) + "', got '"
                              + schemaStr + "'";
                    return out;
                }
            } else if (hasSchema && hasVersion) {
                // Shape (C): full header.
                auto header = SchemaRegistry::readHeader(doc);
                if (!header.valid) {
                    out.error = "PartStudio envelope has malformed _schema/_version header";
                    return out;
                }
                if (header.type != kPartStudioType) {
                    out.error = "PartStudio envelope has wrong _schema: expected '"
                              + std::string(kPartStudioType) + "', got '"
                              + header.type + "'";
                    return out;
                }
                SchemaRegistry reg;  // registers PART_STUDIO in the ctor
                if (!reg.canLoad(kPartStudioType, header.version)) {
                    out.error = "PartStudio version " + header.version.toString()
                              + " is not loadable by this kernel (current: "
                              + reg.currentVersion(kPartStudioType).toString() + ")";
                    return out;
                }
                if (header.kernelVersionPresent &&
                    header.kernelVersion != schema::KERNEL_VERSION) {
                    // Informational; a kernel-version mismatch is not fatal
                    // (only the schema version gates loading) but we surface
                    // it so tooling can warn.
                    ctx->diag().warning(ErrorCode::INVALID_STATE,
                        "PartStudio was saved by kernel " + header.kernelVersion
                        + ", loaded by kernel " + std::string(schema::KERNEL_VERSION));
                }
            } else if (!hasSchema && hasVersion) {
                out.error = "PartStudio envelope has _version but no _schema";
                return out;
            }
            // else: shape (A), no header, accept.
            name     = doc.value("name", std::string());
            treeJson = doc["tree"].dump();
            if (doc.contains("configSchema")) {
                configSchemaJson = doc["configSchema"];
            }
        } else {
            // Form (c) — bare FeatureTree. Schema gate runs inside
            // FeatureTree::fromJSON below.
            treeJson = jsonStr;
        }

        auto tr = FeatureTree::fromJSON(*ctx, treeJson);
        if (!tr.ok) {
            out.error = "Embedded FeatureTree failed to parse: " + tr.error;
            return out;
        }

        out.studio = std::make_unique<PartStudio>(ctx, name);
        // Rebuild the config schema first — if it parses then the
        // studio has its inputs declared before we copy features that
        // may reference them.
        if (!configSchemaFromJson(*ctx, configSchemaJson,
                                  out.studio->schema_)) {
            out.error = "PartStudio configSchema failed to parse";
            out.studio.reset();
            return out;
        }
        // Move the parsed features into the studio's tree. We rebuild
        // because FeatureTree owns a ctx-bound state; re-adding is
        // cheap and guarantees the studio's tree is bound to OUR ctx.
        // Source tree was itself constructed via fromJSON with a seenIds
        // dedup pass, so addFeature's [[nodiscard]] return is always true
        // here — we still check so a silent invariant break surfaces.
        for (const auto& f : tr.tree.features()) {
            if (!out.studio->tree().addFeature(f)) {
                out.error = "PartStudio fromJSON: studio tree rejected id '"
                          + f.id + "' (duplicate id in re-built tree)";
                out.studio.reset();
                return out;
            }
        }
        out.studio->tree().setRollbackIndex(tr.tree.rollbackIndex());
        out.ok = true;
        return out;
    } catch (const nlohmann::json::exception& e) {
        out.error = std::string("JSON parse failure: ") + e.what();
        return out;
    } catch (const std::exception& e) {
        out.error = std::string("Exception during PartStudio::fromJSON: ") + e.what();
        return out;
    } catch (...) {
        out.error = "Unknown exception during PartStudio::fromJSON";
        return out;
    }
}

} // namespace oreo
