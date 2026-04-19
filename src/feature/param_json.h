// SPDX-License-Identifier: LGPL-2.1-or-later

// param_json.h — Shared ParamValue ⇄ JSON helpers.
//
// The FeatureTree, PartStudio, Workspace, and MergeResult wire formats
// all encode the same ParamValue variant (geometric tagged objects,
// scalars, element refs, config refs). This header centralises the two
// conversion functions so the four subsystems stay bit-identical on
// round-trip without copy-pasted tables.

#ifndef OREO_PARAM_JSON_H
#define OREO_PARAM_JSON_H

#include "feature.h"
#include "feature_schema.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace oreo {

// Convert a ParamValue to a JSON value. Geometric types carry a "type"
// discriminator tag so decode is unambiguous. See part-studio-as-function.md
// §4.3 for the canonical encoding.
nlohmann::json paramValueToJson(const ParamValue& val);

// Inverse, best-effort. Unknown / malformed input returns an empty
// string ParamValue (callers should treat that as an error at the
// surrounding layer). Historically lenient — prefer paramValueFromJsonStrict
// at trust boundaries (fromJSON loaders) so a malformed document does not
// silently become different valid geometry.
ParamValue paramValueFromJson(const nlohmann::json& val);

// Strict, schema-guided decode. Validates that `val` matches the exact
// shape required by `expected` and returns the decoded ParamValue on
// success. On failure returns nullopt; if `reasonOut` is non-null, a
// human-readable explanation is written there.
//
// Rejection rules (matched to paramValueToJson's canonical encoding):
//   * Double         → JSON number, not bool; Inf/NaN rejected.
//   * Int            → JSON integer; floats and bool rejected.
//   * Bool           → JSON bool only.
//   * String         → JSON string only.
//   * Vec/Pnt/Dir    → object with type tag AND every x/y/z numeric.
//                      Missing fields are NOT silently defaulted.
//   * Ax1            → object with type tag, px/py/pz/dx/dy/dz all numeric.
//   * Ax2            → object with type tag, px/py/pz/dx/dy/dz/xx/xy/xz
//                      all numeric.
//   * Pln            → object with type tag, px/py/pz/dx/dy/dz numeric.
//   * ElementRef     → object with featureId (non-empty) + elementName +
//                      elementType as strings.
//   * ElemRefList    → array of ElementRef-shaped objects (may be empty
//                      at the decode level; listNonEmpty is enforced by
//                      validateFeature downstream).
//   * ConfigRef      → object with type=="configRef" and non-empty name.
//
// Every caller at a trust boundary (FeatureTree::fromJSON, etc.) must
// use this path. The loose paramValueFromJson exists only for call sites
// that already validated the payload via a higher-level contract.
std::optional<ParamValue> paramValueFromJsonStrict(
    const nlohmann::json& val,
    ParamType expectedType,
    std::string* reasonOut);

// Convert a ParamType to/from its string tag. Used in ConfigSchema
// serialisation, which lists declared input types by name.
const char* paramTypeToString(ParamType t);

// Returns ParamType::Double on unknown / empty input and sets *ok=false.
// Callers must check *ok and fail-closed.
ParamType  paramTypeFromString(const std::string& s, bool* ok);

} // namespace oreo

#endif // OREO_PARAM_JSON_H
