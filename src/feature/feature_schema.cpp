// SPDX-License-Identifier: LGPL-2.1-or-later

// feature_schema.cpp — Schema definitions for all built-in feature types
// plus the validator that runs before executeFeature().

#include "feature_schema.h"
#include "core/diagnostic.h"

#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace oreo {

namespace {

constexpr double kVecZeroEpsilon = 1e-12;

bool isFinite(double v) {
    return std::isfinite(v);
}

bool isPntFinite(const gp_Pnt& p) {
    return isFinite(p.X()) && isFinite(p.Y()) && isFinite(p.Z());
}

bool isVecFinite(const gp_Vec& v) {
    return isFinite(v.X()) && isFinite(v.Y()) && isFinite(v.Z());
}

bool isDirFinite(const gp_Dir& d) {
    return isFinite(d.X()) && isFinite(d.Y()) && isFinite(d.Z());
}

bool isAx1Valid(const gp_Ax1& ax) {
    return isPntFinite(ax.Location()) && isDirFinite(ax.Direction());
}

bool isAx2Valid(const gp_Ax2& ax) {
    return isPntFinite(ax.Location()) && isDirFinite(ax.Direction())
           && isDirFinite(ax.XDirection()) && isDirFinite(ax.YDirection());
}

bool isPlnValid(const gp_Pln& pln) {
    return isAx2Valid(pln.Position().Ax2());
}

// Sub-shape refKinds: those whose element is a strict sub-shape of the
// producing feature's output (a face, an edge, a vertex). A whole-shape
// ROOT ref cannot satisfy these, because the resolver would hand back
// the producing feature's entire cached solid regardless of the stated
// elementType — and then the consumer (fillet edges, shell faces, hole
// face) would see a solid instead of a sub-shape.
bool isSubShapeRefKind(const std::string& kind) {
    return kind == "Face" || kind == "Edge" || kind == "Vertex";
}

// Validate a single ElementRef instance. shapeKindAllowed = "" means any.
//
// Special case: elementName == "ROOT" is the whole-shape sentinel (see
// feature_tree.cpp:resolveReference). It resolves to the producing
// feature's entire cached NamedShape, so it is ONLY valid when the
// consuming spec accepts whole-shape input — i.e. refKind is empty or
// one of the whole-shape kinds (Solid/Shape/Wire). Face/Edge/Vertex
// specs MUST reject ROOT refs or the resolver would silently supply a
// whole solid in place of a sub-shape, turning a face-only consumer
// (hole, shell, draft, face-filter) into a consumer of the entire body.
// Prior to 2026-04-19 the schema only matched elementType as a string,
// which let ElementRef{"F1","ROOT","Face"} pass a face-only check —
// audit finding #P2.
bool checkElementRef(const ElementRef& ref, const std::string& shapeKindAllowed,
                     std::string& reasonOut) {
    if (ref.featureId.empty()) {
        reasonOut = "ElementRef has empty featureId";
        return false;
    }
    if (ref.elementName.empty()) {
        reasonOut = "ElementRef has empty elementName";
        return false;
    }
    if (ref.elementName == "ROOT" && isSubShapeRefKind(shapeKindAllowed)) {
        reasonOut = "ElementRef uses whole-shape sentinel elementName=\"ROOT\" "
                    "but spec requires a sub-shape of kind '" + shapeKindAllowed
                  + "'. Whole-shape refs are only valid on specs that accept "
                    "an entire body (empty/Solid/Shape/Wire refKind).";
        return false;
    }
    if (!shapeKindAllowed.empty() && ref.elementType != shapeKindAllowed) {
        reasonOut = "ElementRef.elementType is '" + ref.elementType
                  + "' but spec requires '" + shapeKindAllowed + "'";
        return false;
    }
    return true;
}

// Set feature status + emit diagnostic. Returns false for chaining.
bool fail(KernelContext& ctx, Feature& feature, ErrorCode code,
          const std::string& message) {
    feature.status = FeatureStatus::ExecutionFailed;
    feature.errorMessage = message;
    ctx.diag().error(code, message);
    return false;
}

// Build "Feature 'Extrude' (id=F3): direction: <reason>" prefix.
std::string ctxPrefix(const Feature& feature, const std::string& paramName,
                      const std::string& reason) {
    std::ostringstream oss;
    oss << "Feature '" << feature.type << "'";
    if (!feature.id.empty()) oss << " (id=" << feature.id << ")";
    oss << ": " << paramName << ": " << reason;
    return oss.str();
}

} // anonymous namespace

const char* paramTypeName(ParamType t) {
    switch (t) {
        case ParamType::Double:      return "Double";
        case ParamType::Int:         return "Int";
        case ParamType::Bool:        return "Bool";
        case ParamType::String:      return "String";
        case ParamType::Vec:         return "Vec";
        case ParamType::Pnt:         return "Pnt";
        case ParamType::Dir:         return "Dir";
        case ParamType::Ax1:         return "Ax1";
        case ParamType::Ax2:         return "Ax2";
        case ParamType::Pln:         return "Pln";
        case ParamType::ElemRef:     return "ElementRef";
        case ParamType::ElemRefList: return "ElementRefList";
        case ParamType::ConfigRef:   return "ConfigRef";
    }
    return "Unknown";
}

const ParamSpec* FeatureSchema::find(const std::string& name) const {
    for (auto& p : params) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

// ─── Registry construction ────────────────────────────────────

namespace {

// Convenience builders so the registration table stays readable.
ParamSpec dbl(const std::string& name,
              std::optional<double> minInc = std::nullopt,
              std::optional<double> maxInc = std::nullopt) {
    ParamSpec s;
    s.name = name; s.type = ParamType::Double;
    s.minInclusive = minInc; s.maxInclusive = maxInc;
    return s;
}
ParamSpec dblPositive(const std::string& name) {
    ParamSpec s = dbl(name, std::nullopt, std::nullopt);
    // "must be > 0" — represented as min=0 + nonZero so 0 is rejected.
    s.minInclusive = 0.0;
    s.nonZero = true;
    return s;
}
ParamSpec dblNonZero(const std::string& name) {
    ParamSpec s = dbl(name);
    s.nonZero = true;
    return s;
}
ParamSpec intMin(const std::string& name, int minVal) {
    ParamSpec s;
    s.name = name; s.type = ParamType::Int;
    s.minInclusive = static_cast<double>(minVal);
    return s;
}
ParamSpec boolOptional(const std::string& name) {
    ParamSpec s;
    s.name = name; s.type = ParamType::Bool;
    s.required = false;
    return s;
}
ParamSpec vec(const std::string& name, bool componentsPositive = false) {
    ParamSpec s;
    s.name = name; s.type = ParamType::Vec;
    s.vecComponentsPositive = componentsPositive;
    s.vecNonZero = true;
    return s;
}
ParamSpec pnt(const std::string& name) {
    ParamSpec s; s.name = name; s.type = ParamType::Pnt; return s;
}
ParamSpec dir(const std::string& name) {
    ParamSpec s; s.name = name; s.type = ParamType::Dir; return s;
}
ParamSpec ax1(const std::string& name) {
    ParamSpec s; s.name = name; s.type = ParamType::Ax1; return s;
}
ParamSpec ax2(const std::string& name) {
    ParamSpec s; s.name = name; s.type = ParamType::Ax2; return s;
}
ParamSpec elemRef(const std::string& name, const std::string& kind = "") {
    ParamSpec s;
    s.name = name; s.type = ParamType::ElemRef; s.refKind = kind;
    return s;
}
ParamSpec elemRefList(const std::string& name, const std::string& kind,
                      bool nonEmpty = true) {
    ParamSpec s;
    s.name = name; s.type = ParamType::ElemRefList;
    s.refKind = kind; s.listNonEmpty = nonEmpty;
    return s;
}

} // anonymous namespace

FeatureSchemaRegistry::FeatureSchemaRegistry() {
    auto add = [this](FeatureSchema s) {
        byType_[s.type] = std::move(s);
    };

    // ── Primitives (no base shape required) ──────────────────
    add({"MakeBox",
         { vec("dimensions", /*componentsPositive=*/true) },
         /*requiresBaseShape=*/false,
         "Axis-aligned box from corner with positive dx/dy/dz."});

    add({"MakeCylinder",
         { dblPositive("radius"), dblPositive("height") },
         false,
         "Right circular cylinder along +Z."});

    add({"MakeSphere",
         { dblPositive("radius") },
         false,
         "Sphere centred at origin."});

    // ── Extended primitives (complete the legacy-only primitive set) ──
    //
    // radius2 == 0 is legal for a point cone (OCCT accepts this); ltx ≥ 0
    // covers the degenerate wedge / pyramid. Schema permissiveness lets
    // OCCT reject impossible geometry with a proper GeomResult failure
    // rather than pre-emptively excluding valid edge cases.
    add({"MakeCone",
         { dblPositive("radius1"),
           dbl("radius2", 0.0),
           dblPositive("height") },
         false,
         "Right circular cone along +Z. radius2==0 yields a point cone."});

    add({"MakeTorus",
         { dblPositive("majorRadius"), dblPositive("minorRadius") },
         false,
         "Torus in the XY plane. majorRadius > minorRadius is required "
         "for a non-self-intersecting torus but is enforced by OCCT, not "
         "the schema."});

    add({"MakeWedge",
         { vec("dimensions", /*componentsPositive=*/true),
           dbl("ltx", 0.0) },
         false,
         "Axis-aligned wedge. dimensions = (dx, dy, dz); ltx is the top-face "
         "extent along X (0 gives a pyramid with the top edge collapsed)."});

    // ── Core operations ──────────────────────────────────────
    add({"Extrude",
         { vec("direction") },
         true,
         "Linear extrusion of a face/wire by a non-zero vector."});

    // angle is signed; range [-2π, 2π] catches obvious rad/deg mistakes.
    add({"Revolve",
         { ax1("axis"), dbl("angle", -2.0 * M_PI, 2.0 * M_PI) },
         true,
         "Revolve a profile around an axis. Angle in radians."});

    add({"Fillet",
         { elemRefList("edges", "Edge"), dblPositive("radius") },
         true,
         "Round selected edges with a constant radius."});

    add({"Chamfer",
         { elemRefList("edges", "Edge"), dblPositive("distance") },
         true,
         "Chamfer selected edges with a constant setback."});

    add({"BooleanUnion",
         { elemRef("tool") },
         true,
         "Union of base shape with the referenced tool shape."});

    add({"BooleanSubtract",
         { elemRef("tool") },
         true,
         "Subtract the referenced tool shape from the base shape."});

    add({"BooleanIntersect",
         { elemRef("tool") },
         true,
         "Intersection of the base shape with the referenced tool shape."});

    // ── Multi-input producers (no base-shape dependency) ────
    //
    // Loft and Sweep consume explicit profile/path references and ignore
    // `currentShape`, so requiresBaseShape=false — the tree may place them
    // as the first feature or downstream of any prior output without
    // inheriting the prior's geometry. executeFeature still feeds their
    // result into the running NamedShape so a Fillet placed after a Loft
    // works exactly the way a user expects.
    //
    // Profiles use an empty refKind because the reference target may be
    // either a Wire (sketch output, makeFaceFromWire input) or a Face
    // (a prior feature's face sub-shape). Loft accepts both under OCCT's
    // BRepOffsetAPI_ThruSections, so forcing "Wire" here would reject
    // legitimate inputs. The execution path resolves via resolveRef and
    // lets OCCT enforce geometric suitability.
    add({"Loft",
         { elemRefList("profiles", /*refKind=*/""),
           boolOptional("makeSolid") },
         /*requiresBaseShape=*/false,
         "Loft through >=2 profile cross-sections. makeSolid=true returns a "
         "closed solid, false returns a ruled surface. Defaults to true."});

    add({"Sweep",
         { elemRef("profile"), elemRef("path") },
         false,
         "Sweep a profile along a path. Both must be wires or wire-convertible."});

    add({"Shell",
         { elemRefList("faces", "Face"), dblNonZero("thickness") },
         true,
         "Hollow body with the listed faces removed; thickness sign sets direction."});

    add({"Mirror",
         { ax2("plane") },
         true,
         "Mirror the body across the given coordinate frame."});

    add({"LinearPattern",
         { vec("direction"), intMin("count", 1), dbl("spacing", 0.0) },
         true,
         "Linear array of count copies along direction at spacing."});

    add({"CircularPattern",
         { ax1("axis"), intMin("count", 1),
           dbl("angle", -2.0 * M_PI, 2.0 * M_PI) },
         true,
         "Circular array of count copies sweeping a total angle (radians)."});

    // Draft angle is degrees per oreo_geometry.h; ±89° to stay safely inside
    // OCCT's solver envelope (90° flips face direction).
    add({"Draft",
         { elemRefList("faces", "Face"),
           dbl("angle", -89.0, 89.0),
           dir("pullDirection") },
         true,
         "Draft selected faces by an angle (degrees) along pullDirection."});

    add({"Hole",
         { elemRef("face", "Face"), pnt("center"),
           dblPositive("diameter"), dblPositive("depth") },
         true,
         "Drill a circular blind hole into the selected face."});

    add({"Pocket",
         { elemRef("profile"), dblPositive("depth") },
         true,
         "Subtract a profile sketch swept to depth from the body."});

    add({"Offset",
         { dblNonZero("distance") },
         true,
         "Offset every face of the solid by signed distance."});

    add({"Thicken",
         { dblNonZero("thickness") },
         true,
         "Give a shell or face the given (signed) thickness."});

    add({"SplitBody",
         { pnt("point"), dir("normal") },
         true,
         "Split the body with a plane defined by point + normal."});

    add({"VariableFillet",
         { elemRef("edge", "Edge"),
           dblPositive("startRadius"), dblPositive("endRadius") },
         true,
         "Variable-radius fillet on a single edge."});

    add({"MakeFace",
         {},
         true,
         "Make a planar face from the current wire base."});

    add({"Combine",
         { elemRefList("shapes", "") },
         true,
         "Union the base with all referenced shapes (compound build)."});

    add({"Rib",
         { dir("direction"), dblPositive("thickness"), elemRef("profile") },
         true,
         "Rib (strengthening fin) extruded from a profile sketch."});
}

const FeatureSchemaRegistry& FeatureSchemaRegistry::instance() {
    static const FeatureSchemaRegistry kInstance;
    return kInstance;
}

const FeatureSchema* FeatureSchemaRegistry::find(const std::string& type) const {
    auto it = byType_.find(type);
    if (it == byType_.end()) return nullptr;
    return &it->second;
}

std::vector<std::string> FeatureSchemaRegistry::registeredTypes() const {
    std::vector<std::string> out;
    out.reserve(byType_.size());
    for (auto& kv : byType_) out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    return out;
}

// ─── Validation ───────────────────────────────────────────────

namespace {

bool validateNumeric(KernelContext& ctx, Feature& feature,
                     const ParamSpec& spec, const ParamValue& v) {
    double x = 0.0;
    if (spec.type == ParamType::Double) {
        x = std::get<double>(v);
        if (!isFinite(x)) {
            return fail(ctx, feature, ErrorCode::OUT_OF_RANGE,
                        ctxPrefix(feature, spec.name, "value is NaN/Inf"));
        }
    } else { // Int
        x = static_cast<double>(std::get<int>(v));
    }

    if (spec.minInclusive && x < *spec.minInclusive) {
        std::ostringstream oss;
        oss << x << " < min " << *spec.minInclusive;
        return fail(ctx, feature, ErrorCode::OUT_OF_RANGE,
                    ctxPrefix(feature, spec.name, oss.str()));
    }
    if (spec.maxInclusive && x > *spec.maxInclusive) {
        std::ostringstream oss;
        oss << x << " > max " << *spec.maxInclusive;
        return fail(ctx, feature, ErrorCode::OUT_OF_RANGE,
                    ctxPrefix(feature, spec.name, oss.str()));
    }
    if (spec.nonZero && std::fabs(x) <= kVecZeroEpsilon) {
        return fail(ctx, feature, ErrorCode::OUT_OF_RANGE,
                    ctxPrefix(feature, spec.name, "value must be non-zero"));
    }
    return true;
}

bool validateOne(KernelContext& ctx, Feature& feature,
                 const ParamSpec& spec, const ParamValue& v) {
    // Variant index must match spec.type (see ParamType enum).
    if (v.index() != static_cast<size_t>(spec.type)) {
        // Special-case unresolved ConfigRef so callers debugging
        // PartStudio::execute get a message that points at the actual
        // cause ("the studio didn't resolve your config placeholder")
        // instead of the generic "expected Double, got index 12".
        if (v.index() == static_cast<size_t>(ParamType::ConfigRef)) {
            const auto& cr = std::get<ConfigRef>(v);
            std::string reason = std::string("unresolved ConfigRef '") + cr.name
                               + "' (expected " + paramTypeName(spec.type)
                               + "). resolveConfigRefs() must run before "
                                 "validateFeature — this is a PartStudio "
                                 "execute-path invariant violation.";
            return fail(ctx, feature, ErrorCode::INVALID_INPUT,
                        ctxPrefix(feature, spec.name, reason));
        }
        std::string reason = std::string("expected ") + paramTypeName(spec.type)
                           + ", got variant index " + std::to_string(v.index());
        return fail(ctx, feature, ErrorCode::INVALID_INPUT,
                    ctxPrefix(feature, spec.name, reason));
    }

    switch (spec.type) {
        case ParamType::Double:
        case ParamType::Int:
            return validateNumeric(ctx, feature, spec, v);

        case ParamType::Bool:
        case ParamType::String:
            return true;

        case ParamType::Vec: {
            const auto& vec = std::get<gp_Vec>(v);
            if (!isVecFinite(vec)) {
                return fail(ctx, feature, ErrorCode::OUT_OF_RANGE,
                            ctxPrefix(feature, spec.name,
                                      "vector has NaN/Inf component"));
            }
            if (spec.vecNonZero && vec.Magnitude() <= kVecZeroEpsilon) {
                return fail(ctx, feature, ErrorCode::INVALID_INPUT,
                            ctxPrefix(feature, spec.name,
                                      "vector magnitude is zero"));
            }
            if (spec.vecComponentsPositive) {
                if (vec.X() <= 0.0 || vec.Y() <= 0.0 || vec.Z() <= 0.0) {
                    std::ostringstream oss;
                    oss << "all components must be > 0; got ("
                        << vec.X() << ", " << vec.Y() << ", " << vec.Z() << ")";
                    return fail(ctx, feature, ErrorCode::OUT_OF_RANGE,
                                ctxPrefix(feature, spec.name, oss.str()));
                }
            }
            return true;
        }

        case ParamType::Pnt:
            if (!isPntFinite(std::get<gp_Pnt>(v))) {
                return fail(ctx, feature, ErrorCode::OUT_OF_RANGE,
                            ctxPrefix(feature, spec.name,
                                      "point has NaN/Inf coordinate"));
            }
            return true;

        case ParamType::Dir:
            if (!isDirFinite(std::get<gp_Dir>(v))) {
                return fail(ctx, feature, ErrorCode::OUT_OF_RANGE,
                            ctxPrefix(feature, spec.name,
                                      "direction has NaN/Inf component"));
            }
            return true;

        case ParamType::Ax1:
            if (!isAx1Valid(std::get<gp_Ax1>(v))) {
                return fail(ctx, feature, ErrorCode::OUT_OF_RANGE,
                            ctxPrefix(feature, spec.name,
                                      "axis has NaN/Inf in location or direction"));
            }
            return true;

        case ParamType::Ax2:
            if (!isAx2Valid(std::get<gp_Ax2>(v))) {
                return fail(ctx, feature, ErrorCode::OUT_OF_RANGE,
                            ctxPrefix(feature, spec.name,
                                      "ax2 frame has NaN/Inf in location or directions"));
            }
            return true;

        case ParamType::Pln:
            if (!isPlnValid(std::get<gp_Pln>(v))) {
                return fail(ctx, feature, ErrorCode::OUT_OF_RANGE,
                            ctxPrefix(feature, spec.name,
                                      "plane has NaN/Inf in frame"));
            }
            return true;

        case ParamType::ElemRef: {
            std::string reason;
            const auto& ref = std::get<ElementRef>(v);
            if (!checkElementRef(ref, spec.refKind, reason)) {
                return fail(ctx, feature, ErrorCode::INVALID_INPUT,
                            ctxPrefix(feature, spec.name, reason));
            }
            return true;
        }

        case ParamType::ElemRefList: {
            const auto& refs = std::get<std::vector<ElementRef>>(v);
            if (spec.listNonEmpty && refs.empty()) {
                return fail(ctx, feature, ErrorCode::INVALID_INPUT,
                            ctxPrefix(feature, spec.name,
                                      "list must contain at least one reference"));
            }
            for (size_t i = 0; i < refs.size(); ++i) {
                std::string reason;
                if (!checkElementRef(refs[i], spec.refKind, reason)) {
                    std::ostringstream oss;
                    oss << "[" << i << "] " << reason;
                    return fail(ctx, feature, ErrorCode::INVALID_INPUT,
                                ctxPrefix(feature, spec.name, oss.str()));
                }
            }
            return true;
        }

        case ParamType::ConfigRef:
            // ConfigRef is a placeholder — spec.type is never declared as
            // ConfigRef by a FeatureSchema (feature schemas describe
            // concrete geometry params), so reaching this case means the
            // caller registered a bogus schema. Fail loudly.
            return fail(ctx, feature, ErrorCode::INTERNAL_ERROR,
                        ctxPrefix(feature, spec.name,
                                  "feature schema declared ConfigRef as a "
                                  "parameter type — not supported"));
    }
    return true;
}

} // anonymous namespace

bool validateFeature(KernelContext& ctx, Feature& feature) {
    const auto& reg = FeatureSchemaRegistry::instance();
    const FeatureSchema* schema = reg.find(feature.type);
    if (!schema) {
        return fail(ctx, feature, ErrorCode::INVALID_INPUT,
                    "Unknown feature type: '" + feature.type + "'");
    }

    // Required params present?
    for (auto& spec : schema->params) {
        auto it = feature.params.find(spec.name);
        if (it == feature.params.end()) {
            if (spec.required) {
                return fail(ctx, feature, ErrorCode::INVALID_INPUT,
                            ctxPrefix(feature, spec.name,
                                      "required parameter is missing"));
            }
            continue;
        }
        if (!validateOne(ctx, feature, spec, it->second)) return false;
    }

    // Unknown extras → Warning only (forward compat).
    for (auto& kv : feature.params) {
        if (!schema->find(kv.first)) {
            ctx.diag().warning(ErrorCode::INVALID_INPUT,
                ctxPrefix(feature, kv.first,
                          "parameter is not declared in the schema (ignored)"));
        }
    }

    return true;
}

} // namespace oreo
