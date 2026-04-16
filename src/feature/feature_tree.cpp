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
#include "core/oreo_error.h"
#include "core/schema.h"
#include "core/units.h"
#include "naming/element_map.h"

#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <sstream>

namespace oreo {

// ── Feature management ───────────────────────────────────────

void FeatureTree::addFeature(const Feature& feature) {
    features_.push_back(feature);
    dirty_[feature.id] = true;
}

void FeatureTree::insertFeature(int index, const Feature& feature) {
    if (index < 0) index = 0;
    if (index > static_cast<int>(features_.size())) index = static_cast<int>(features_.size());
    features_.insert(features_.begin() + index, feature);
    markDirtyFrom(index);
}

bool FeatureTree::removeFeature(const std::string& featureId) {
    int idx = findIndex(featureId);
    if (idx < 0) return false;
    features_.erase(features_.begin() + idx);
    cache_.erase(featureId);
    dirty_.erase(featureId);
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
    // Reset tags for deterministic replay
    ctx_->tags.reset();
    ctx_->diag.clear();
    return replayFrom(features_.empty() ? "" : features_[0].id);
}

NamedShape FeatureTree::replayFrom(const std::string& dirtyFromId) {
    int startIdx = 0;
    if (!dirtyFromId.empty()) {
        startIdx = findIndex(dirtyFromId);
        if (startIdx < 0) startIdx = 0;
    }

    // Mark everything from startIdx onward as dirty
    markDirtyFrom(startIdx);

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

    // Create the reference resolver lambda
    auto resolver = [this](const ElementRef& ref) -> ResolvedRef {
        return resolveReference(ref);
    };

    // Replay each feature from startIdx onward
    for (int i = startIdx; i < static_cast<int>(features_.size()); ++i) {
        auto& feature = features_[i];

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

        // Execute the feature using the tree's context
        auto result = executeFeature(*ctx_, feature, currentShape, resolver);

        if (result.ok() && feature.status == FeatureStatus::OK && !result.value().isNull()) {
            currentShape = result.value();
            cache_[feature.id] = currentShape;
        } else {
            // Feature failed — keep the previous shape and mark as broken
            if (feature.status == FeatureStatus::OK) {
                feature.status = FeatureStatus::ExecutionFailed;
                feature.errorMessage = result.ok() ? "Operation returned null shape" : result.errorMessage();
            }
            // Cache the current (unchanged) shape so downstream features
            // can still reference elements from before the broken feature
            cache_[feature.id] = currentShape;
        }

        dirty_[feature.id] = false;
    }

    return currentShape;
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

namespace {

// Convert a ParamValue to a nlohmann::json value.
// Geometric types get a discriminated-union object with a "type" field.
nlohmann::json paramToJson(const ParamValue& val) {
    return std::visit([](auto&& arg) -> nlohmann::json {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, double>) {
            return arg;
        } else if constexpr (std::is_same_v<T, int>) {
            return arg;
        } else if constexpr (std::is_same_v<T, bool>) {
            return arg;
        } else if constexpr (std::is_same_v<T, std::string>) {
            return arg;  // nlohmann::json handles escaping
        } else if constexpr (std::is_same_v<T, gp_Pnt>) {
            return {{"type", "point"},
                    {"x", arg.X()}, {"y", arg.Y()}, {"z", arg.Z()}};
        } else if constexpr (std::is_same_v<T, gp_Vec>) {
            return {{"type", "vec"},
                    {"x", arg.X()}, {"y", arg.Y()}, {"z", arg.Z()}};
        } else if constexpr (std::is_same_v<T, gp_Dir>) {
            return {{"type", "dir"},
                    {"x", arg.X()}, {"y", arg.Y()}, {"z", arg.Z()}};
        } else if constexpr (std::is_same_v<T, gp_Ax1>) {
            auto loc = arg.Location();
            auto dir = arg.Direction();
            return {{"type", "ax1"},
                    {"px", loc.X()}, {"py", loc.Y()}, {"pz", loc.Z()},
                    {"dx", dir.X()}, {"dy", dir.Y()}, {"dz", dir.Z()}};
        } else if constexpr (std::is_same_v<T, gp_Ax2>) {
            auto loc = arg.Location();
            auto dir = arg.Direction();
            auto xdir = arg.XDirection();
            return {{"type", "ax2"},
                    {"px", loc.X()},  {"py", loc.Y()},  {"pz", loc.Z()},
                    {"dx", dir.X()},  {"dy", dir.Y()},  {"dz", dir.Z()},
                    {"xx", xdir.X()}, {"xy", xdir.Y()}, {"xz", xdir.Z()}};
        } else if constexpr (std::is_same_v<T, gp_Pln>) {
            auto loc = arg.Location();
            auto axis = arg.Axis().Direction();
            return {{"type", "pln"},
                    {"px", loc.X()}, {"py", loc.Y()}, {"pz", loc.Z()},
                    {"dx", axis.X()}, {"dy", axis.Y()}, {"dz", axis.Z()}};
        } else if constexpr (std::is_same_v<T, ElementRef>) {
            return {{"featureId",   arg.featureId},
                    {"elementName", arg.elementName},
                    {"elementType", arg.elementType}};
        } else if constexpr (std::is_same_v<T, std::vector<ElementRef>>) {
            nlohmann::json arr = nlohmann::json::array();
            for (auto& r : arg) {
                arr.push_back({{"featureId",   r.featureId},
                               {"elementName", r.elementName},
                               {"elementType", r.elementType}});
            }
            return arr;
        } else {
            return nullptr;
        }
    }, val);
}

// Parse a JSON value back into a ParamValue.
// Dispatches on the "type" tag for geometric objects.
ParamValue jsonToParam(const nlohmann::json& val) {
    if (val.is_number_float()) {
        return val.get<double>();
    } else if (val.is_number_integer()) {
        return val.get<int>();
    } else if (val.is_boolean()) {
        return val.get<bool>();
    } else if (val.is_string()) {
        return val.get<std::string>();
    } else if (val.is_object()) {
        if (val.contains("type") && val["type"].is_string()) {
            std::string t = val["type"].get<std::string>();
            if (t == "point") {
                return gp_Pnt(val.value("x", 0.0),
                              val.value("y", 0.0),
                              val.value("z", 0.0));
            } else if (t == "vec") {
                return gp_Vec(val.value("x", 0.0),
                              val.value("y", 0.0),
                              val.value("z", 0.0));
            } else if (t == "dir") {
                return gp_Dir(val.value("x", 0.0),
                              val.value("y", 1.0),
                              val.value("z", 0.0));
            } else if (t == "ax1") {
                return gp_Ax1(
                    gp_Pnt(val.value("px", 0.0),
                           val.value("py", 0.0),
                           val.value("pz", 0.0)),
                    gp_Dir(val.value("dx", 0.0),
                           val.value("dy", 0.0),
                           val.value("dz", 1.0)));
            } else if (t == "ax2") {
                return gp_Ax2(
                    gp_Pnt(val.value("px", 0.0),
                           val.value("py", 0.0),
                           val.value("pz", 0.0)),
                    gp_Dir(val.value("dx", 0.0),
                           val.value("dy", 0.0),
                           val.value("dz", 1.0)),
                    gp_Dir(val.value("xx", 1.0),
                           val.value("xy", 0.0),
                           val.value("xz", 0.0)));
            } else if (t == "pln") {
                return gp_Pln(
                    gp_Pnt(val.value("px", 0.0),
                           val.value("py", 0.0),
                           val.value("pz", 0.0)),
                    gp_Dir(val.value("dx", 0.0),
                           val.value("dy", 0.0),
                           val.value("dz", 1.0)));
            }
        }
        // No "type" field — try ElementRef
        if (val.contains("featureId")) {
            ElementRef ref;
            ref.featureId   = val.value("featureId", "");
            ref.elementName = val.value("elementName", "");
            ref.elementType = val.value("elementType", "");
            return ref;
        }
        // Unknown object — store as empty string fallback
        return std::string{};
    } else if (val.is_array()) {
        // Array of ElementRef objects
        if (!val.empty() && val[0].is_object() && val[0].contains("featureId")) {
            std::vector<ElementRef> refs;
            for (auto& item : val) {
                ElementRef ref;
                ref.featureId   = item.value("featureId", "");
                ref.elementName = item.value("elementName", "");
                ref.elementType = item.value("elementType", "");
                refs.push_back(ref);
            }
            return refs;
        }
        // Fallback: legacy [x,y,z] arrays → gp_Vec for backwards compatibility
        if (val.size() == 3 && val[0].is_number()) {
            return gp_Vec(val[0].get<double>(),
                          val[1].get<double>(),
                          val[2].get<double>());
        }
        return std::string{};
    }
    // null or unrecognised
    return std::string{};
}

} // anonymous namespace

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
            params[name] = paramToJson(val);
        }
        jf["params"] = params;
        arr.push_back(jf);
    }

    doc["features"] = arr;

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

FeatureTree FeatureTree::fromJSON(const std::string& json) {
    FeatureTree tree;
    try {
        auto doc = nlohmann::json::parse(json);
        if (!doc.contains("features") || !doc["features"].is_array()) return tree;

        for (auto& jf : doc["features"]) {
            Feature f;
            f.id         = jf.value("id", "");
            f.type       = jf.value("type", "");
            f.suppressed = jf.value("suppressed", false);

            if (jf.contains("params") && jf["params"].is_object()) {
                for (auto& [key, val] : jf["params"].items()) {
                    f.params[key] = jsonToParam(val);
                }
            }
            tree.addFeature(f);
        }
    } catch (const nlohmann::json::exception&) {
        // Invalid JSON — return empty tree
    }
    return tree;
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

    // Find the cached shape for the referenced feature
    auto it = cache_.find(ref.featureId);
    if (it == cache_.end() || it->second.isNull()) return result;

    const auto& shape = it->second;
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
