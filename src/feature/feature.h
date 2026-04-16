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
// Parameter Value — type-safe variant for feature parameters
// ═══════════════════════════════════════════════════════════════

using ParamValue = std::variant<
    double,
    int,
    bool,
    std::string,
    gp_Vec,
    gp_Pnt,
    gp_Dir,
    gp_Ax1,
    gp_Ax2,
    gp_Pln,
    ElementRef,
    std::vector<ElementRef>
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
    TopoDS_Shape shape;       // The actual sub-shape
    IndexedName indexedName;  // The IndexedName in the element map
    bool resolved = false;    // Whether resolution succeeded
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
