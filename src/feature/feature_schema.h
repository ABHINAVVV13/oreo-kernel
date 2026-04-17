// SPDX-License-Identifier: LGPL-2.1-or-later

// feature_schema.h — Declarative schemas for every Feature type.
//
// Each schema enumerates the parameters a feature requires (plus type,
// numeric range, ElementRef kind, and optionality). validateFeature()
// runs the schema before executeFeature() dispatches, so an ill-formed
// feature is rejected with a structured diagnostic instead of crashing
// inside std::variant::get or producing degenerate OCCT input.
//
// The registry is process-wide and immutable — feature types are an
// internal taxonomy, not user-extensible state.

#ifndef OREO_FEATURE_SCHEMA_H
#define OREO_FEATURE_SCHEMA_H

#include "feature.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace oreo {

// One discriminator per std::variant alternative in ParamValue. Kept in
// sync with the variant order in feature.h — checkParamType() relies on
// the integer values matching variant::index().
enum class ParamType {
    Double      = 0,
    Int         = 1,
    Bool        = 2,
    String      = 3,
    Vec         = 4,
    Pnt         = 5,
    Dir         = 6,
    Ax1         = 7,
    Ax2         = 8,
    Pln         = 9,
    ElemRef     = 10,
    ElemRefList = 11,
};

const char* paramTypeName(ParamType t);

// Descriptor for a single parameter of a feature.
struct ParamSpec {
    std::string name;
    ParamType   type;
    bool        required = true;

    // Numeric range (Double / Int only). nullopt means unbounded on that side.
    std::optional<double> minInclusive;
    std::optional<double> maxInclusive;
    bool nonZero = false;  // Double: |v| > epsilon

    // Vec constraints (Vec only).
    bool vecComponentsPositive = false;  // each x,y,z > 0
    bool vecNonZero            = true;   // |v| > epsilon (default: required)

    // ElementRef constraints (ElemRef / ElemRefList only).
    // refKind: "Face", "Edge", "Vertex" or "" for any.
    std::string refKind;
    bool        listNonEmpty = true;  // ElemRefList: list must have >=1 entry

    std::string description;
};

// Descriptor for a feature type.
struct FeatureSchema {
    std::string type;                     // "Extrude", "Fillet", ...
    std::vector<ParamSpec> params;
    bool requiresBaseShape = true;        // false for primitives (MakeBox etc.)
    std::string description;

    // Find a ParamSpec by name. Returns nullptr if not present.
    const ParamSpec* find(const std::string& name) const;
};

// Process-wide registry. Singleton; built lazily on first access.
class FeatureSchemaRegistry {
public:
    static const FeatureSchemaRegistry& instance();

    // Schema for a feature type. nullptr if the type is unknown.
    const FeatureSchema* find(const std::string& type) const;

    // All registered feature types, sorted lexicographically.
    std::vector<std::string> registeredTypes() const;

private:
    FeatureSchemaRegistry();
    std::map<std::string, FeatureSchema> byType_;
};

// Validate `feature` against the registered schema for `feature.type`.
//
// On the first violation, sets feature.status = ExecutionFailed,
// populates feature.errorMessage, reports a diagnostic to ctx.diag(),
// and returns false. On success returns true and leaves feature.status
// untouched (executeFeature still resets it).
//
// Failure modes:
//   - Unknown feature type             → ErrorCode::INVALID_INPUT
//   - Missing required parameter       → ErrorCode::INVALID_INPUT
//   - Wrong variant index for a param  → ErrorCode::INVALID_INPUT
//   - Numeric NaN/Inf or out of range  → ErrorCode::OUT_OF_RANGE
//   - Vec with wrong sign / zero       → ErrorCode::INVALID_INPUT
//   - ElementRef.elementType mismatch  → ErrorCode::INVALID_INPUT
//   - ElementRef list empty when       → ErrorCode::INVALID_INPUT
//     listNonEmpty=true
//
// Unknown extra parameters produce a Warning, not a failure, so a v1.1
// feature persisted with extra fields can still be loaded by v1.0.
bool validateFeature(KernelContext& ctx, Feature& feature);

} // namespace oreo

#endif // OREO_FEATURE_SCHEMA_H
