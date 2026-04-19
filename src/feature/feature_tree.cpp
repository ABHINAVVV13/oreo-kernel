// SPDX-License-Identifier: LGPL-2.1-or-later

// feature_tree.cpp — Parametric feature tree replay engine.
//
// The replay algorithm:
//   1. For each feature in order (top to bottom)
//   2. If suppressed → skip, pass through previous shape
//   3. If cached and not dirty → use cache
//   4. Resolve element references using cached shapes from earlier features
//   5. Execute the feature (calls into geometry operations)
//   6. If execution fails → mark as broken, continue with previous shape
//   7. Cache the result
//   8. Continue to next feature

#include "feature_tree.h"
#include "core/diagnostic.h"
#include "core/schema.h"
#include "core/units.h"
#include "feature/feature_schema.h"
#include "feature/param_json.h"
#include "naming/element_map.h"

#include <TopExp.hxx>
#include <Standard_Failure.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace oreo {

// ── Feature management ───────────────────────────────────────

bool FeatureTree::addFeature(const Feature& feature) {
    if (findIndex(feature.id) >= 0) {
        // Core invariant: featureId must be unique within a tree. The
        // C ABI checks at the boundary, but any direct C++ caller
        // (fromJSON replay, Workspace::fork, merge resolver) can hit
        // this path too. Fail-closed with a diagnostic instead of
        // silently double-inserting, which would corrupt dirty_ and
        // cache_ which both key on id.
        ctx_->diag().error(ErrorCode::INVALID_STATE,
            "FeatureTree::addFeature rejected duplicate id '"
            + feature.id + "'");
        return false;
    }
    features_.push_back(feature);
    dirty_[feature.id] = true;
    return true;
}

bool FeatureTree::insertFeature(int index, const Feature& feature) {
    if (findIndex(feature.id) >= 0) {
        ctx_->diag().error(ErrorCode::INVALID_STATE,
            "FeatureTree::insertFeature rejected duplicate id '"
            + feature.id + "'");
        return false;
    }
    if (index < 0) index = 0;
    if (index > static_cast<int>(features_.size())) index = static_cast<int>(features_.size());
    features_.insert(features_.begin() + index, feature);
    markDirtyFrom(index);
    return true;
}

bool FeatureTree::removeFeature(const std::string& featureId) {
    int idx = findIndex(featureId);
    if (idx < 0) return false;
    features_.erase(features_.begin() + idx);
    cache_.erase(featureId);
    dirty_.erase(featureId);
    allocatorSnapshotsBefore_.erase(featureId);
    markDirtyFrom(idx);
    return true;
}

void FeatureTree::updateParameter(const std::string& featureId,
                                  const std::string& paramName,
                                  const ParamValue& value) {
    int idx = findIndex(featureId);
    if (idx < 0) return;
    features_[idx].params[paramName] = value;
    markDirtyFrom(idx);
}

void FeatureTree::suppressFeature(const std::string& featureId, bool suppress) {
    int idx = findIndex(featureId);
    if (idx < 0) return;
    features_[idx].suppressed = suppress;
    markDirtyFrom(idx);
}

bool FeatureTree::moveFeature(const std::string& featureId, int newIndex) {
    int oldIdx = findIndex(featureId);
    if (oldIdx < 0) return false;
    const int n = static_cast<int>(features_.size());
    if (newIndex < 0)  newIndex = 0;
    if (newIndex >= n) newIndex = n - 1;
    if (newIndex == oldIdx) return true;

    Feature moved = std::move(features_[oldIdx]);
    features_.erase(features_.begin() + oldIdx);
    features_.insert(features_.begin() + newIndex, std::move(moved));

    // Everything from min(old, new) onward might depend on the new
    // ordering — mark dirty so replay rebuilds them.
    markDirtyFrom(std::min(oldIdx, newIndex));
    return true;
}

int FeatureTree::replaceReference(const std::string& oldFeatureId,
                                  const std::string& newFeatureId) {
    if (oldFeatureId.empty() || newFeatureId.empty()) return 0;
    int rewrites = 0;
    int firstChanged = static_cast<int>(features_.size());
    for (int i = 0; i < static_cast<int>(features_.size()); ++i) {
        bool changedThis = false;
        for (auto& kv : features_[i].params) {
            if (auto* r = std::get_if<ElementRef>(&kv.second)) {
                if (r->featureId == oldFeatureId) {
                    r->featureId = newFeatureId;
                    ++rewrites;
                    changedThis = true;
                }
            } else if (auto* lst = std::get_if<std::vector<ElementRef>>(&kv.second)) {
                for (auto& er : *lst) {
                    if (er.featureId == oldFeatureId) {
                        er.featureId = newFeatureId;
                        ++rewrites;
                        changedThis = true;
                    }
                }
            }
        }
        if (changedThis && i < firstChanged) firstChanged = i;
    }
    if (rewrites > 0 && firstChanged < static_cast<int>(features_.size())) {
        markDirtyFrom(firstChanged);
    }
    return rewrites;
}

const Feature* FeatureTree::getFeature(const std::string& id) const {
    int idx = findIndex(id);
    if (idx < 0) return nullptr;
    return &features_[idx];
}

// ── Replay engine ────────────────────────────────────────────

NamedShape FeatureTree::replay() {
    // Mark everything dirty and replay from scratch
    for (auto& f : features_) {
        dirty_[f.id] = true;
    }
    cache_.clear();
    allocatorSnapshotsBefore_.clear();
    // Reset tags for deterministic replay within the same document. Use
    // resetCounterOnly() so the documentId (and therefore the encoded tag
    // prefixes) is preserved across replay — a full reset() would zero the
    // documentId and flip subsequent tags back to single-doc mode.
    ctx_->tags().resetCounterOnly();
    ctx_->diag().clear();
    return replayFrom(features_.empty() ? "" : features_[0].id);
}

NamedShape FeatureTree::replayFrom(const std::string& dirtyFromId) {
    int startIdx = 0;
    if (!dirtyFromId.empty()) {
        startIdx = findIndex(dirtyFromId);
        if (startIdx < 0) startIdx = 0;
    }

    NamedShape currentShape;

    // Use cached shape from just before the dirty start
    if (startIdx > 0) {
        for (int i = startIdx - 1; i >= 0; --i) {
            if (!features_[i].suppressed && features_[i].status == FeatureStatus::OK) {
                auto it = cache_.find(features_[i].id);
                if (it != cache_.end()) {
                    currentShape = it->second;
                    break;
                }
            }
        }
    }

    if (startIdx == 0) {
        ctx_->tags().resetCounterOnly();
    } else {
        auto snapIt = allocatorSnapshotsBefore_.find(features_[startIdx].id);
        if (snapIt != allocatorSnapshotsBefore_.end()) {
            ctx_->tags().restoreV2(snapIt->second, /*force=*/true);
        } else {
            // No trustworthy allocator boundary exists; fall back to a
            // full replay to preserve deterministic identities.
            startIdx = 0;
            currentShape = {};
            cache_.clear();
            allocatorSnapshotsBefore_.clear();
            ctx_->tags().resetCounterOnly();
        }
    }

    // Mark everything from startIdx onward as dirty
    markDirtyFrom(startIdx);

    // Create the reference resolver lambda
    auto resolver = [this](const ElementRef& ref) -> ResolvedRef {
        return resolveReference(ref);
    };

    // Replay each feature from startIdx onward
    for (int i = startIdx; i < static_cast<int>(features_.size()); ++i) {
        auto& feature = features_[i];
        allocatorSnapshotsBefore_[feature.id] = ctx_->tags().snapshotV2();

        if (feature.suppressed) {
            feature.status = FeatureStatus::Suppressed;
            cache_[feature.id] = currentShape;
            dirty_[feature.id] = false;
            continue;
        }

        // Check if we can use cache (not dirty)
        if (!dirty_[feature.id]) {
            auto it = cache_.find(feature.id);
            if (it != cache_.end()) {
                currentShape = it->second;
                continue;
            }
        }

        // If a transformer is installed (e.g. from PartStudio::execute
        // resolving ConfigRef placeholders), run it first. The stored
        // feature is NOT mutated — we dispatch on the transformed copy.
        // status + errorMessage are mirrored back after dispatch so
        // getBrokenFeatures() reflects the actual execution outcome.
        //
        // executeFeatureWith produces the OperationResult via the
        // factory API (OperationResult has a private default ctor), so
        // we avoid declaring an uninitialised `result` local.
        auto executeFeatureWith =
            [&](Feature& target) -> OperationResult<NamedShape> {
                return executeFeature(*ctx_, target, currentShape, resolver);
            };

        if (featureTransformer_) {
            Feature transformed = featureTransformer_(feature);
            if (transformed.status == FeatureStatus::BrokenReference ||
                transformed.status == FeatureStatus::ExecutionFailed) {
                feature.status       = transformed.status;
                feature.errorMessage = transformed.errorMessage;
                ctx_->diag().error(ErrorCode::INVALID_INPUT,
                                   "Feature '" + feature.id + "' (type="
                                   + feature.type + "): "
                                   + transformed.errorMessage);
                cache_[feature.id] = currentShape;
                dirty_[feature.id] = false;
                continue;
            }
            auto result = executeFeatureWith(transformed);
            feature.status       = transformed.status;
            feature.errorMessage = transformed.errorMessage;

            if (result.ok() && feature.status == FeatureStatus::OK
                && !result.value().isNull()) {
                currentShape = result.value();
                cache_[feature.id] = currentShape;
            } else {
                if (feature.status == FeatureStatus::OK) {
                    feature.status = FeatureStatus::ExecutionFailed;
                    feature.errorMessage = result.ok()
                        ? "Operation returned null shape"
                        : result.errorMessage();
                }
                cache_[feature.id] = currentShape;
            }
        } else {
            auto result = executeFeatureWith(feature);
            if (result.ok() && feature.status == FeatureStatus::OK
                && !result.value().isNull()) {
                currentShape = result.value();
                cache_[feature.id] = currentShape;
            } else {
                if (feature.status == FeatureStatus::OK) {
                    feature.status = FeatureStatus::ExecutionFailed;
                    feature.errorMessage = result.ok()
                        ? "Operation returned null shape"
                        : result.errorMessage();
                }
                cache_[feature.id] = currentShape;
            }
        }

        dirty_[feature.id] = false;
    }

    return currentShape;
}

void FeatureTree::setRollbackIndex(int index) {
    if (index < -1) index = -1;
    const int last = static_cast<int>(features_.size()) - 1;
    if (index > last) index = last;
    rollbackIndex_ = index;
}

NamedShape FeatureTree::replayToRollback() {
    NamedShape result = replay();
    if (rollbackIndex_ < 0 || features_.empty()) return result;
    if (rollbackIndex_ >= static_cast<int>(features_.size())) return result;
    return getShapeAt(features_[rollbackIndex_].id);
}

// ── Shape access ─────────────────────────────────────────────

NamedShape FeatureTree::getShapeAt(const std::string& featureId) const {
    auto it = cache_.find(featureId);
    if (it != cache_.end()) return it->second;
    return {};
}

NamedShape FeatureTree::getFinalShape() const {
    if (features_.empty()) return {};
    // Walk backwards to find the last non-suppressed feature with a cached result
    for (int i = static_cast<int>(features_.size()) - 1; i >= 0; --i) {
        auto it = cache_.find(features_[i].id);
        if (it != cache_.end() && !it->second.isNull()) {
            return it->second;
        }
    }
    return {};
}

NamedShape FeatureTree::rollbackTo(const std::string& featureId) const {
    int idx = findIndex(featureId);
    if (idx <= 0) return {};

    // Return the cached shape from the feature just before this one
    for (int i = idx - 1; i >= 0; --i) {
        auto it = cache_.find(features_[i].id);
        if (it != cache_.end() && !it->second.isNull()) {
            return it->second;
        }
    }
    return {};
}

// ── Diagnostics ──────────────────────────────────────────────

std::vector<const Feature*> FeatureTree::getBrokenFeatures() const {
    std::vector<const Feature*> broken;
    for (auto& f : features_) {
        if (f.status == FeatureStatus::BrokenReference ||
            f.status == FeatureStatus::ExecutionFailed) {
            broken.push_back(&f);
        }
    }
    return broken;
}

bool FeatureTree::hasBrokenFeatures() const {
    return !getBrokenFeatures().empty();
}

// ── Serialization ────────────────────────────────────────────
// Uses nlohmann/json for both serialization and deserialization.
// Geometric types (gp_Pnt, gp_Vec, gp_Dir, gp_Ax1, gp_Ax2, gp_Pln)
// are tagged with a "type" field so they round-trip without loss.

// Encoding of ParamValue ⇄ JSON is centralised in feature/param_json.{h,cpp}.
// PartStudio, Workspace, and MergeResult use the same helpers so the
// wire format cannot drift between subsystems.

std::string FeatureTree::toJSON() const {
    nlohmann::json doc;
    nlohmann::json arr = nlohmann::json::array();

    for (auto& f : features_) {
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

    doc["features"] = arr;
    doc["rollbackIndex"] = rollbackIndex_;

    // Schema headers
    doc["_schema"] = schema::TYPE_FEATURE_TREE;
    doc["_version"] = schema::FEATURE_TREE.toJSON();
    doc["_kernelVersion"] = schema::KERNEL_VERSION;

    // Units from the context
    if (ctx_) {
        doc["units"] = {
            {"length", unit_convert::lengthUnitName(ctx_->units().documentLength)},
            {"angle", unit_convert::angleUnitName(ctx_->units().documentAngle)}
        };
    }

    return doc.dump();
}

FeatureTreeFromJsonResult FeatureTree::fromJSON(const std::string& json) {
    FeatureTreeFromJsonResult out;
    try {
        auto doc = nlohmann::json::parse(json);
        if (!doc.is_object()) {
            out.error = "FeatureTree JSON root must be an object";
            return out;
        }
        // Fail-closed schema gate. Three admissible shapes:
        //   (A) no header at all       — pre-versioning legacy, accept.
        //   (B) _schema but no _version — pre-versioning legacy with only
        //       a type tag (observed in the wild from early kernel
        //       snapshots). Accept if the type matches; reject a wrong type.
        //   (C) both _schema and _version — full header; type must match
        //       AND version must be loadable (current-major, or migratable).
        //
        // The "fail-closed" half is shapes that DO carry a header but
        // don't match — those cannot be silently treated as "maybe for
        // us." A FeatureTree loader that cheerfully swallows an
        // oreo.workspace blob is exactly what the audit flagged.
        const bool hasSchema  = doc.contains("_schema");
        const bool hasVersion = doc.contains("_version");
        if (hasSchema && !hasVersion) {
            // Shape (B): legacy header. Match the type; no version check.
            if (!doc["_schema"].is_string()) {
                out.error = "FeatureTree JSON has non-string _schema";
                return out;
            }
            const std::string schemaStr = doc["_schema"].get<std::string>();
            if (schemaStr != schema::TYPE_FEATURE_TREE) {
                out.error = "FeatureTree JSON has wrong _schema: expected '"
                          + std::string(schema::TYPE_FEATURE_TREE)
                          + "', got '" + schemaStr + "'";
                return out;
            }
            // no _version: accept as pre-versioning legacy.
        } else if (hasSchema && hasVersion) {
            // Shape (C): full header.
            auto header = SchemaRegistry::readHeader(doc);
            if (!header.valid) {
                out.error = "FeatureTree JSON has malformed _schema/_version header";
                return out;
            }
            if (header.type != schema::TYPE_FEATURE_TREE) {
                out.error = "FeatureTree JSON has wrong _schema: expected '"
                          + std::string(schema::TYPE_FEATURE_TREE)
                          + "', got '" + header.type + "'";
                return out;
            }
            SchemaRegistry reg;
            if (!reg.canLoad(schema::TYPE_FEATURE_TREE, header.version)) {
                out.error = "FeatureTree version " + header.version.toString()
                          + " is not loadable by this kernel (current: "
                          + reg.currentVersion(schema::TYPE_FEATURE_TREE).toString()
                          + ")";
                return out;
            }
        } else if (!hasSchema && hasVersion) {
            out.error = "FeatureTree JSON has _version but no _schema";
            return out;
        }
        // else: shape (A), no header at all, accept.
        if (!doc.contains("features") || !doc["features"].is_array()) {
            out.error = "FeatureTree JSON missing required 'features' array";
            return out;
        }
        out.tree.setRollbackIndex(doc.value("rollbackIndex", -1));

        // Track seen ids so we can reject duplicates before they
        // poison cache_ / dirty_ / allocatorSnapshotsBefore_ (all
        // keyed on feature id — a duplicate would alias two
        // entries into one map slot and corrupt updates / replays).
        std::unordered_set<std::string> seenIds;
        seenIds.reserve(doc["features"].size());

        for (auto& jf : doc["features"]) {
            if (!jf.is_object()) {
                out.error = "Each entry in 'features' must be an object";
                out.tree = FeatureTree{};
                return out;
            }
            Feature f;
            f.id         = jf.value("id", "");
            f.type       = jf.value("type", "");
            f.suppressed = jf.value("suppressed", false);

            if (f.id.empty()) {
                out.error = "Feature is missing required 'id' field";
                out.tree = FeatureTree{};
                return out;
            }
            if (f.type.empty()) {
                out.error = "Feature '" + f.id + "' is missing required 'type' field";
                out.tree = FeatureTree{};
                return out;
            }
            if (!seenIds.insert(f.id).second) {
                out.error = "Duplicate feature id '" + f.id
                          + "' in FeatureTree JSON — ids must be unique";
                out.tree = FeatureTree{};
                return out;
            }

            if (jf.contains("params") && jf["params"].is_object()) {
                // Schema-guided strict decode. The schema registry knows
                // the declared ParamType for every (featureType, paramName)
                // pair; feed each JSON value through paramValueFromJsonStrict
                // so a malformed document cannot silently coerce a missing
                // field into a default (the legacy paramValueFromJson path
                // would do exactly that). Unknown params are kept, but
                // with a lenient decode so forward-compat documents still
                // load — validateFeature will then reject unused extras
                // as warnings per the schema's own contract.
                const auto& reg = FeatureSchemaRegistry::instance();
                const auto* sch = reg.find(f.type);
                for (auto& [key, val] : jf["params"].items()) {
                    const ParamSpec* spec = sch ? sch->find(key) : nullptr;
                    if (spec) {
                        std::string reason;
                        auto decoded = paramValueFromJsonStrict(val, spec->type, &reason);
                        if (!decoded) {
                            out.error = "Feature '" + f.id + "' param '" + key
                                      + "' (expected " + paramTypeToString(spec->type)
                                      + "): " + reason;
                            out.tree = FeatureTree{};
                            return out;
                        }
                        f.params[key] = std::move(*decoded);
                    } else {
                        // Unknown type or unknown param name — keep the
                        // lenient decode so future fields survive round-trip.
                        // validateFeature will flag unexpected params.
                        f.params[key] = paramValueFromJson(val);
                    }
                }
            } else if (jf.contains("params") && !jf["params"].is_null()) {
                out.error = "Feature '" + f.id + "': 'params' must be an object";
                out.tree = FeatureTree{};
                return out;
            }
            if (!out.tree.addFeature(f)) {
                // seenIds already caught duplicates above; if addFeature
                // rejects here, the invariant is broken inside the tree.
                // Fail closed with the captured diagnostic.
                out.error = "FeatureTree::addFeature rejected id '"
                          + f.id + "' during fromJSON replay";
                out.tree = FeatureTree{};
                return out;
            }
        }
        out.ok = true;
        return out;
    } catch (const nlohmann::json::exception& e) {
        out.tree  = FeatureTree{};
        out.error = std::string("JSON parse failure: ") + e.what();
        return out;
    } catch (const Standard_Failure& e) {
        out.tree  = FeatureTree{};
        out.error = std::string("OCCT exception during fromJSON: ") + e.GetMessageString();
        return out;
    } catch (const std::exception& e) {
        out.tree  = FeatureTree{};
        out.error = std::string("Exception during fromJSON: ") + e.what();
        return out;
    } catch (...) {
        out.tree  = FeatureTree{};
        out.error = "Unknown exception during fromJSON";
        return out;
    }
}

FeatureTreeFromJsonResult FeatureTree::fromJSON(KernelContext& ctx,
                                                  const std::string& json) {
    auto out = fromJSON(json);
    // Quota check on feature count.
    const std::uint64_t maxF = ctx.quotas().maxFeatures;
    if (out.ok && maxF > 0
        && static_cast<std::uint64_t>(out.tree.featureCount()) > maxF) {
        ctx.diag().error(ErrorCode::RESOURCE_EXHAUSTED,
            std::string("FeatureTree has ") + std::to_string(out.tree.featureCount())
            + " features — exceeds context quota maxFeatures="
            + std::to_string(maxF));
        out.ok    = false;
        out.error = "Exceeds quota maxFeatures";
        out.tree  = FeatureTree{};
    }
    if (!out.ok) {
        ctx.diag().error(ErrorCode::DESERIALIZE_FAILED,
            std::string("FeatureTree::fromJSON failed: ") + out.error);
    }
    return out;
}

// ── Private helpers ──────────────────────────────────────────

int FeatureTree::findIndex(const std::string& id) const {
    for (int i = 0; i < static_cast<int>(features_.size()); ++i) {
        if (features_[i].id == id) return i;
    }
    return -1;
}

void FeatureTree::markDirtyFrom(int index) {
    for (int i = index; i < static_cast<int>(features_.size()); ++i) {
        dirty_[features_[i].id] = true;
    }
}

ResolvedRef FeatureTree::resolveReference(const ElementRef& ref) const {
    ResolvedRef result;
    result.resolved = false;

    if (ref.isNull()) return result;

    // Suppressed features are a special case: during replay we do cache
    // the pass-through shape under their id (so callers observing the
    // timeline see the correct "visible" shape at that step), but a
    // downstream ElementRef against a suppressed feature MUST NOT
    // resolve — the ref was stable against the suppressed feature's
    // OWN topology, and binding it to the upstream pass-through shape
    // would silently re-target the same MappedName onto unrelated
    // geometry. Fail closed: the downstream feature gets
    // BrokenReference, which the user can fix by unsuppressing or by
    // retargeting the ref.
    int refIdx = findIndex(ref.featureId);
    if (refIdx >= 0 && features_[refIdx].suppressed) return result;

    // Find the cached shape for the referenced feature
    auto it = cache_.find(ref.featureId);
    if (it == cache_.end() || it->second.isNull()) return result;

    const auto& shape = it->second;

    // ─── Whole-shape reference ────────────────────────────────────
    //
    // Before v2, only Face / Edge / Vertex resolved. Boolean / Combine /
    // any "tool" or "profile" style feature that wants to consume
    // another feature's ENTIRE body had no way to say so — the only
    // legal elementName patterns were sub-shape lookups. This was the
    // reason feature-tree BooleanUnion / Subtract / Intersect could
    // not be exercised end-to-end.
    //
    // The whole-shape contract:
    //   ElementRef{ featureId, elementName="ROOT",
    //               elementType="Solid" | "Shape" | "Wire" | "" }
    //
    // On such a ref we return the cached NamedShape's top-level shape,
    // with a synthetic IndexedName("Shape", 1) so downstream code still
    // sees a real identity.
    //
    // elementType="Face"|"Edge"|"Vertex" is rejected here as a
    // defence-in-depth against a bypass of the schema validator: a
    // ROOT ref declaring a sub-shape type is contradictory (ROOT
    // returns the whole body, not a sub-shape), and silently handing
    // back the whole solid to a face/edge/vertex-expecting consumer
    // is the exact trap the 2026-04-19 audit flagged. The schema
    // validator (feature_schema.cpp:checkElementRef) rejects the same
    // combination earlier for a clearer diagnostic; this is the
    // belt-and-suspenders check for any caller that reaches the
    // resolver without going through validateFeature first.
    if (ref.elementName == "ROOT") {
        if (ref.elementType == "Face" ||
            ref.elementType == "Edge" ||
            ref.elementType == "Vertex") {
            return result;  // refuse — ROOT can't be a sub-shape
        }
        result.shape         = shape.shape();
        result.namedShape    = shape;
        result.isWholeShape  = true;
        // Represent the whole-shape identity as IndexedName("Shape", 1).
        // Consumers that care about sub-shape semantics inspect the
        // actual TopoDS_Shape; the IndexedName is informational.
        result.indexedName   = IndexedName("Shape", 1);
        result.resolved      = !result.shape.IsNull();
        return result;
    }

    if (!shape.elementMap()) return result;

    // Look up the MappedName in the element map
    MappedName mappedName(ref.elementName);
    IndexedName idx = shape.elementMap()->getIndexedName(mappedName);

    if (idx.isNull()) {
        // Try as a plain IndexedName (e.g., "Face3")
        // Parse the element type and index from the name
        std::string name = ref.elementName;
        std::string type;
        int index = 0;
        for (size_t i = 0; i < name.size(); ++i) {
            if (std::isdigit(name[i])) {
                type = name.substr(0, i);
                index = std::stoi(name.substr(i));
                break;
            }
        }
        if (!type.empty() && index > 0) {
            idx = IndexedName(type, index);
        }
    }

    if (idx.isNull()) return result;

    // Get the actual sub-shape by index
    TopAbs_ShapeEnum shapeType = TopAbs_SHAPE;
    if (idx.type() == "Face") shapeType = TopAbs_FACE;
    else if (idx.type() == "Edge") shapeType = TopAbs_EDGE;
    else if (idx.type() == "Vertex") shapeType = TopAbs_VERTEX;

    if (shapeType == TopAbs_SHAPE) return result;

    TopTools_IndexedMapOfShape subShapes;
    TopExp::MapShapes(shape.shape(), shapeType, subShapes);

    if (idx.index() < 1 || idx.index() > subShapes.Extent()) return result;

    result.shape = subShapes(idx.index());
    result.indexedName = idx;
    result.resolved = true;
    return result;
}

} // namespace oreo
