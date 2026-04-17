// SPDX-License-Identifier: LGPL-2.1-or-later

// part_studio.cpp — JSON round-trip for the PartStudio aggregate.

#include "part_studio.h"
#include "core/diagnostic.h"
#include "core/schema.h"

#include <nlohmann/json.hpp>

namespace oreo {

namespace {

// Top-level schema name registered in SchemaRegistry. Same numeric
// version as the FeatureTree contract — they evolve together.
constexpr const char* kPartStudioType = "oreo.part_studio";

} // anonymous namespace

std::string PartStudio::toJSON() const {
    using nlohmann::json;
    json doc;
    doc["_schema"]  = kPartStudioType;
    doc["_version"] = schema::FEATURE_TREE.toJSON();
    doc["name"]     = name_;
    // documentId is mostly informational on the wire — load reuses
    // the receiving ctx's id. We still record it so a server can
    // detect cross-document paste.
    doc["documentId"] = std::to_string(documentId());
    // Embed the FeatureTree JSON as a parsed sub-object so the file
    // stays human-readable without a quoted-string-inside-string.
    doc["tree"] = json::parse(tree_.toJSON());
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

        // Two formats supported:
        //  (a) PartStudio envelope: { _schema, _version, name, documentId, tree }
        //  (b) Bare FeatureTree:    { _schema=oreo.feature_tree, ... } or
        //                            { features: [...] }  (legacy unkeyed)
        // Sniff (a) first, fall back to (b) for back-compat.
        std::string treeJson;
        std::string name;
        if (doc.contains("tree") && doc["tree"].is_object()) {
            // Form (a)
            name     = doc.value("name", std::string());
            treeJson = doc["tree"].dump();
        } else {
            // Form (b)
            treeJson = jsonStr;
        }

        auto tr = FeatureTree::fromJSON(*ctx, treeJson);
        if (!tr.ok) {
            out.error = "Embedded FeatureTree failed to parse: " + tr.error;
            return out;
        }

        out.studio = std::make_unique<PartStudio>(ctx, name);
        // Move the parsed features into the studio's tree. We rebuild
        // because FeatureTree owns a ctx-bound state; re-adding is
        // cheap and guarantees the studio's tree is bound to OUR ctx.
        for (const auto& f : tr.tree.features()) {
            out.studio->tree().addFeature(f);
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
