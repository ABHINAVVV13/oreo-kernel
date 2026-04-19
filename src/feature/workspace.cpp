// SPDX-License-Identifier: LGPL-2.1-or-later

// workspace.cpp — Workspace (branch) lifecycle + JSON round-trip.

#include "workspace.h"
#include "param_json.h"
#include "core/diagnostic.h"
#include "core/schema.h"

#include <nlohmann/json.hpp>

#include <unordered_set>

namespace oreo {

namespace {

constexpr const char* kWorkspaceType = "oreo.workspace";

// Serialise a feature list (the same encoding FeatureTree.toJSON uses
// for its `features` array). Kept separate so the base snapshot and
// the live tree share formatting without round-tripping through
// FeatureTree twice.
nlohmann::json featuresToJson(const std::vector<Feature>& feats) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& f : feats) {
        nlohmann::json jf;
        jf["id"]         = f.id;
        jf["type"]       = f.type;
        jf["suppressed"] = f.suppressed;
        nlohmann::json params = nlohmann::json::object();
        for (auto& [name, val] : f.params) {
            params[name] = paramValueToJson(val);
        }
        jf["params"] = params;
        arr.push_back(jf);
    }
    return arr;
}

bool featuresFromJson(KernelContext& ctx,
                      const nlohmann::json& arr,
                      std::vector<Feature>& out,
                      std::string* errOut) {
    auto fail = [&](const std::string& msg) {
        ctx.diag().error(ErrorCode::DESERIALIZE_FAILED, msg);
        if (errOut) *errOut = msg;
        return false;
    };
    if (!arr.is_array()) {
        return fail("Workspace: features list must be a JSON array");
    }
    out.clear();
    // Reject duplicate feature ids — the live + base snapshots both
    // feed through this helper, and a duplicate would corrupt any
    // downstream cache/dirty bookkeeping keyed on feature id (the
    // same defence the FeatureTree::fromJSON path applies).
    std::unordered_set<std::string> seenIds;
    seenIds.reserve(arr.size());
    for (std::size_t i = 0; i < arr.size(); ++i) {
        const auto& jf = arr[i];
        if (!jf.is_object()) {
            return fail("Workspace: feature[" + std::to_string(i)
                        + "] must be an object");
        }
        Feature f;
        f.id         = jf.value("id", std::string{});
        f.type       = jf.value("type", std::string{});
        f.suppressed = jf.value("suppressed", false);
        if (f.id.empty() || f.type.empty()) {
            return fail("Workspace: feature[" + std::to_string(i)
                        + "] missing id/type");
        }
        if (!seenIds.insert(f.id).second) {
            return fail("Workspace: duplicate feature id '" + f.id
                        + "' (feature ids must be unique)");
        }
        if (jf.contains("params") && jf["params"].is_object()) {
            // Schema-guided strict decode at the trust boundary. Unknown
            // feature types fall back to the lenient path so forward-compat
            // documents with newer types still round-trip; known types
            // enforce per-param shape (no silent defaults on missing
            // geometry fields, no float→int coercion, etc).
            const auto& reg = FeatureSchemaRegistry::instance();
            const auto* sch = reg.find(f.type);
            for (auto& [key, val] : jf["params"].items()) {
                const ParamSpec* spec = sch ? sch->find(key) : nullptr;
                if (spec) {
                    std::string reason;
                    auto decoded = paramValueFromJsonStrict(val, spec->type, &reason);
                    if (!decoded) {
                        return fail("Workspace feature '" + f.id + "' param '"
                                    + key + "' (expected "
                                    + paramTypeToString(spec->type) + "): "
                                    + reason);
                    }
                    f.params[key] = std::move(*decoded);
                } else {
                    f.params[key] = paramValueFromJson(val);
                }
            }
        }
        out.push_back(std::move(f));
    }
    return true;
}

} // anonymous namespace

Workspace::Workspace(std::shared_ptr<KernelContext> ctx, std::string name)
    : ctx_(std::move(ctx)),
      tree_(ctx_),
      name_(std::move(name)) {}

std::unique_ptr<Workspace> Workspace::fork(std::string newName) const {
    auto child = std::make_unique<Workspace>(ctx_, std::move(newName));
    child->parentName_ = name_;

    // Copy every feature into the child's tree. Using addFeature avoids
    // exposing the tree's internals; it re-hashes into the child's own
    // cache/dirty maps so replay on the child is deterministic. Source
    // tree is assumed to have unique ids (enforced by addFeature at
    // insertion time), so the [[nodiscard]] return is always true here —
    // we still check it so a silent invariant break would surface.
    for (const auto& f : tree_.features()) {
        if (!child->tree_.addFeature(f)) {
            ctx_->diag().error(ErrorCode::INTERNAL_ERROR,
                "Workspace::fork: child tree rejected id '" + f.id
                + "' — parent tree invariant violated");
            break;
        }
    }
    child->tree_.setRollbackIndex(tree_.rollbackIndex());

    // Base snapshot: the parent's feature set at the moment of fork.
    child->baseSnapshot_               = tree_.features();  // vector copy
    child->baseSnapshotRollbackIndex_  = tree_.rollbackIndex();
    return child;
}

// ─── JSON ────────────────────────────────────────────────────────

std::string Workspace::toJSON() const {
    using nlohmann::json;
    json doc;
    doc["_schema"]       = kWorkspaceType;
    doc["_version"]      = schema::WORKSPACE.toJSON();
    doc["_kernelVersion"]= schema::KERNEL_VERSION;
    doc["name"]          = name_;
    doc["parentName"]    = parentName_;
    doc["documentId"]    = std::to_string(documentId());
    doc["features"]      = featuresToJson(tree_.features());
    doc["rollbackIndex"] = tree_.rollbackIndex();
    doc["baseSnapshot"]  = featuresToJson(baseSnapshot_);
    doc["baseRollbackIndex"] = baseSnapshotRollbackIndex_;
    return doc.dump();
}

Workspace::FromJsonResult Workspace::fromJSON(
    std::shared_ptr<KernelContext> ctx, const std::string& jsonStr) {
    FromJsonResult out;
    if (!ctx) {
        out.error = "Workspace::fromJSON requires a non-null context";
        return out;
    }
    try {
        auto doc = nlohmann::json::parse(jsonStr);
        if (!doc.is_object()) {
            out.error = "Workspace JSON root must be an object";
            return out;
        }
        // Fail-closed schema gate. Three admissible shapes (see
        // FeatureTree::fromJSON for the same pattern):
        //   (A) no header at all       — legacy, accept.
        //   (B) _schema only           — pre-versioning type tag.
        //   (C) full _schema+_version  — enforce type + canLoad().
        const bool hasSchema  = doc.contains("_schema");
        const bool hasVersion = doc.contains("_version");
        if (hasSchema && !hasVersion) {
            if (!doc["_schema"].is_string()) {
                out.error = "Workspace JSON has non-string _schema";
                return out;
            }
            const std::string schemaStr = doc["_schema"].get<std::string>();
            if (schemaStr != kWorkspaceType) {
                out.error = "Workspace JSON has wrong _schema: expected '"
                          + std::string(kWorkspaceType) + "', got '"
                          + schemaStr + "'";
                return out;
            }
        } else if (hasSchema && hasVersion) {
            auto header = SchemaRegistry::readHeader(doc);
            if (!header.valid) {
                out.error = "Workspace JSON has malformed _schema/_version header";
                return out;
            }
            if (header.type != kWorkspaceType) {
                out.error = "Workspace JSON has wrong _schema: expected '"
                          + std::string(kWorkspaceType)
                          + "', got '" + header.type + "'";
                return out;
            }
            SchemaRegistry reg;
            if (!reg.canLoad(kWorkspaceType, header.version)) {
                out.error = "Workspace version " + header.version.toString()
                          + " is not loadable by this kernel (current: "
                          + reg.currentVersion(kWorkspaceType).toString() + ")";
                return out;
            }
            if (header.kernelVersionPresent &&
                header.kernelVersion != schema::KERNEL_VERSION) {
                ctx->diag().warning(ErrorCode::INVALID_STATE,
                    "Workspace was saved by kernel " + header.kernelVersion
                    + ", loaded by kernel " + std::string(schema::KERNEL_VERSION));
            }
        } else if (!hasSchema && hasVersion) {
            out.error = "Workspace JSON has _version but no _schema";
            return out;
        }
        // else: shape (A), no header, accept.
        std::string name = doc.value("name", std::string{});
        out.workspace = std::make_unique<Workspace>(ctx, name);
        out.workspace->parentName_ = doc.value("parentName", std::string{});

        if (doc.contains("features")) {
            std::vector<Feature> feats;
            std::string reason;
            if (!featuresFromJson(*ctx, doc["features"], feats, &reason)) {
                out.error = reason.empty()
                    ? std::string("Workspace features failed to parse")
                    : reason;
                out.workspace.reset();
                return out;
            }
            for (const auto& f : feats) {
                if (!out.workspace->tree_.addFeature(f)) {
                    out.error = "Workspace fromJSON: duplicate feature id '"
                              + f.id + "' in features array";
                    out.workspace.reset();
                    return out;
                }
            }
            out.workspace->tree_.setRollbackIndex(doc.value("rollbackIndex", -1));
        }
        if (doc.contains("baseSnapshot")) {
            std::vector<Feature> baseFeats;
            std::string reason;
            if (!featuresFromJson(*ctx, doc["baseSnapshot"], baseFeats, &reason)) {
                out.error = reason.empty()
                    ? std::string("Workspace baseSnapshot failed to parse")
                    : reason;
                out.workspace.reset();
                return out;
            }
            out.workspace->baseSnapshot_ = std::move(baseFeats);
        }
        out.workspace->baseSnapshotRollbackIndex_ =
            doc.value("baseRollbackIndex", -1);
        out.ok = true;
        return out;
    } catch (const nlohmann::json::exception& e) {
        out.error = std::string("JSON parse failure: ") + e.what();
        return out;
    } catch (const std::exception& e) {
        out.error = std::string("Exception during Workspace::fromJSON: ") + e.what();
        return out;
    } catch (...) {
        out.error = "Unknown exception during Workspace::fromJSON";
        return out;
    }
}

} // namespace oreo
