// SPDX-License-Identifier: LGPL-2.1-or-later

// feature.h — Feature definition and parameter types for parametric CAD.
//
// A Feature represents a single modeling operation in a feature tree.
// Each feature has:
//   - A unique ID
//   - A type (Extrude, Fillet, Hole, etc.)
//   - Parameters (dimensions, directions, element references)
//   - Status (ok, broken, suppressed)
//
// Features reference elements from earlier features by MappedName.
// At replay time, these references are resolved to actual sub-shapes.

#ifndef OREO_FEATURE_H
#define OREO_FEATURE_H

#include "core/kernel_context.h"
#include "core/operation_result.h"
#include "core/diagnostic_scope.h"
#include "naming/named_shape.h"

#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <functional>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace oreo {

// ═══════════════════════════════════════════════════════════════
// Element Reference — a stable reference to a sub-shape
// ═══════════════════════════════════════════════════════════════

struct ElementRef {
    std::string featureId;    // Which feature produced this element
    std::string elementName;  // MappedName string (e.g., "Face3;:H1a;:M")
    std::string elementType;  // "Face", "Edge", "Vertex"

    bool isNull() const { return featureId.empty() || elementName.empty(); }
};

// ═══════════════════════════════════════════════════════════════
// ConfigRef — a placeholder for a value supplied by a Part Studio
// configuration at execute time. See docs/part-studio-as-function.md.
//
// Resolution happens in resolveConfigRefs() before executeFeature /
// validateFeature sees the feature. By the time dispatch runs, every
// param has been replaced by its concrete value — ConfigRef never
// reaches the geometry layer.
// ═══════════════════════════════════════════════════════════════

struct ConfigRef {
    std::string name;  // must match a ConfigInputSpec.name in the studio

    bool isNull() const { return name.empty(); }
};

// ═══════════════════════════════════════════════════════════════
// Parameter Value — type-safe variant for feature parameters
//
// IMPORTANT: the variant alternative order MUST stay in lock-step with
// the ParamType enum in feature_schema.h. validateOne() compares
// v.index() to the numeric ParamType value to reject mismatched params.
// Appending a new alternative is safe; reordering existing ones is a
// break.
// ═══════════════════════════════════════════════════════════════

using ParamValue = std::variant<
    double,                     // 0  — ParamType::Double
    int,                        // 1  — ParamType::Int
    bool,                       // 2  — ParamType::Bool
    std::string,                // 3  — ParamType::String
    gp_Vec,                     // 4  — ParamType::Vec
    gp_Pnt,                     // 5  — ParamType::Pnt
    gp_Dir,                     // 6  — ParamType::Dir
    gp_Ax1,                     // 7  — ParamType::Ax1
    gp_Ax2,                     // 8  — ParamType::Ax2
    gp_Pln,                     // 9  — ParamType::Pln
    ElementRef,                 // 10 — ParamType::ElemRef
    std::vector<ElementRef>,    // 11 — ParamType::ElemRefList
    ConfigRef                   // 12 — ParamType::ConfigRef
>;

// ═══════════════════════════════════════════════════════════════
// Feature Status
// ═══════════════════════════════════════════════════════════════

enum class FeatureStatus {
    OK,                // Executed successfully
    BrokenReference,   // An element reference could not be resolved
    ExecutionFailed,   // OCCT operation failed
    Suppressed,        // User-suppressed (skipped during replay)
    NotExecuted,       // Not yet executed (tree not replayed)
};

// ═══════════════════════════════════════════════════════════════
// Feature — a single operation in the feature tree
// ═══════════════════════════════════════════════════════════════

struct Feature {
    std::string id;                              // Unique ID (e.g., "F1", UUID)
    std::string type;                            // "Extrude", "Fillet", "Hole", etc.
    std::map<std::string, ParamValue> params;    // Named parameters
    bool suppressed = false;                     // User can suppress features
    FeatureStatus status = FeatureStatus::NotExecuted;
    std::string errorMessage;                    // Error detail if status != OK

    // Convenience: get a param as a specific type (throws if wrong type)
    template<typename T>
    const T& get(const std::string& name) const {
        return std::get<T>(params.at(name));
    }

    template<typename T>
    T getOr(const std::string& name, const T& defaultVal) const {
        auto it = params.find(name);
        if (it == params.end()) return defaultVal;
        if (auto* p = std::get_if<T>(&it->second)) return *p;
        return defaultVal;
    }
};

// ═══════════════════════════════════════════════════════════════
// Feature Executor — resolves refs and runs the operation
// ═══════════════════════════════════════════════════════════════

// Resolve an ElementRef to an actual OCCT sub-shape.
// Uses the cached NamedShape from the referenced feature.
struct ResolvedRef {
    TopoDS_Shape shape;       // The actual sub-shape (or whole shape on ROOT ref)
    IndexedName indexedName;  // The IndexedName in the element map
    bool resolved = false;    // Whether resolution succeeded

    // For whole-shape refs (elementName == "ROOT"), `namedShape` holds
    // the full cached NamedShape of the referenced feature so Boolean/
    // Combine-style dispatches can preserve the tool's element map +
    // identity instead of fabricating a fresh one (which would lose
    // naming history across the operation).
    //
    // `isWholeShape` is true iff this ref resolved through the ROOT
    // path. For sub-shape refs it is false and `namedShape` is empty.
    NamedShape namedShape;
    bool isWholeShape = false;
};

// Callback type for resolving element references during replay
using RefResolver = std::function<ResolvedRef(const ElementRef& ref)>;

// Execute a single feature given the current shape and a reference resolver.
// Returns OperationResult<NamedShape> (or failure, with error in feature.status).
// Context-aware version (preferred)
OperationResult<NamedShape> executeFeature(KernelContext& ctx,
                          Feature& feature,
                          const NamedShape& currentShape,
                          const RefResolver& resolver);

} // namespace oreo

#endif // OREO_FEATURE_H
