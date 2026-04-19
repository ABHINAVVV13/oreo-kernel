// SPDX-License-Identifier: LGPL-2.1-or-later

// param_json.cpp — Shared ParamValue ⇄ JSON helpers.
//
// This is the single source of truth for encoding ParamValue alternatives
// on the wire. FeatureTree, PartStudio, Workspace, and MergeResult all
// call through here so the four subsystems cannot drift in formatting
// details (e.g. one writes "point" and another "pnt").

#include "param_json.h"

#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <limits>
#include <string>

namespace oreo {

nlohmann::json paramValueToJson(const ParamValue& val) {
    return std::visit([](auto&& arg) -> nlohmann::json {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, double>) {
            return arg;
        } else if constexpr (std::is_same_v<T, int>) {
            return arg;
        } else if constexpr (std::is_same_v<T, bool>) {
            return arg;
        } else if constexpr (std::is_same_v<T, std::string>) {
            return arg;
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
        } else if constexpr (std::is_same_v<T, ConfigRef>) {
            return {{"type", "configRef"},
                    {"name", arg.name}};
        } else {
            return nullptr;
        }
    }, val);
}

ParamValue paramValueFromJson(const nlohmann::json& val) {
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
            } else if (t == "configRef") {
                ConfigRef cr;
                cr.name = val.value("name", std::string{});
                return cr;
            }
        }
        if (val.contains("featureId")) {
            ElementRef ref;
            ref.featureId   = val.value("featureId", "");
            ref.elementName = val.value("elementName", "");
            ref.elementType = val.value("elementType", "");
            return ref;
        }
        return std::string{};
    } else if (val.is_array()) {
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
        if (val.size() == 3 && val[0].is_number()) {
            return gp_Vec(val[0].get<double>(),
                          val[1].get<double>(),
                          val[2].get<double>());
        }
        return std::string{};
    }
    return std::string{};
}

namespace {

// Helpers for the strict decode. Each expects reasonOut != nullptr OR
// the caller doesn't care about the message. They return nullopt / fail
// via the outer function's reason-writing helper.

struct Fail {
    std::string* out;
    bool operator()(std::string msg) const {
        if (out && out->empty()) *out = std::move(msg);
        return false;
    }
};

bool requireNumber(const nlohmann::json& j, const char* name, double& out, std::string* reason) {
    Fail fail{reason};
    if (!j.contains(name)) return fail(std::string("missing field '") + name + "'");
    const auto& v = j.at(name);
    if (!v.is_number() || v.is_boolean()) {
        return fail(std::string("field '") + name + "' must be a number");
    }
    out = v.get<double>();
    if (!std::isfinite(out)) {
        return fail(std::string("field '") + name + "' is NaN or Inf");
    }
    return true;
}

bool expectTypeTag(const nlohmann::json& j, const char* tag, std::string* reason) {
    Fail fail{reason};
    if (!j.is_object()) return fail(std::string("expected ") + tag + " object");
    if (!j.contains("type") || !j["type"].is_string()) {
        return fail(std::string("expected object with 'type' = \"") + tag + "\"");
    }
    if (j["type"].get<std::string>() != tag) {
        return fail(std::string("expected type='") + tag + "', got '" + j["type"].get<std::string>() + "'");
    }
    return true;
}

bool decodeElementRef(const nlohmann::json& item, ElementRef& out, std::string* reason) {
    Fail fail{reason};
    if (!item.is_object()) return fail("ElementRef must be an object");
    if (!item.contains("featureId") || !item["featureId"].is_string()) {
        return fail("ElementRef missing string 'featureId'");
    }
    if (!item.contains("elementName") || !item["elementName"].is_string()) {
        return fail("ElementRef missing string 'elementName'");
    }
    // elementType is advisory — accept empty when absent but require string type when present.
    if (item.contains("elementType") && !item["elementType"].is_string()) {
        return fail("ElementRef 'elementType' must be a string if present");
    }
    out.featureId   = item["featureId"].get<std::string>();
    out.elementName = item["elementName"].get<std::string>();
    out.elementType = item.value("elementType", std::string{});
    if (out.featureId.empty()) return fail("ElementRef 'featureId' must be non-empty");
    // Note: elementName may be empty for whole-shape refs ("" + elementType="Solid");
    // validation of that combination lives in feature_tree.cpp:resolveReference.
    return true;
}

} // anonymous

std::optional<ParamValue> paramValueFromJsonStrict(
    const nlohmann::json& val,
    ParamType expected,
    std::string* reasonOut) {
    Fail fail{reasonOut};

    // ConfigRef is a placeholder variant that can appear in ANY param
    // slot — the studio's resolver substitutes it at execute time.
    // If the JSON carries a ConfigRef tag, decode it as ConfigRef
    // regardless of the declared schema type; validateFeature will
    // re-check that the SUBSTITUTED value matches the schema. Without
    // this early check a `dimensions = ConfigRef{"size"}` persisted by
    // toJSON would fail to round-trip, because the declared type is
    // Vec and the JSON shape is {"type":"configRef","name":"size"}.
    if (val.is_object()
        && val.contains("type")
        && val["type"].is_string()
        && val["type"].get<std::string>() == "configRef") {
        if (!val.contains("name") || !val["name"].is_string()) {
            fail("ConfigRef missing string 'name'");
            return std::nullopt;
        }
        std::string name = val["name"].get<std::string>();
        if (name.empty()) { fail("ConfigRef 'name' must be non-empty"); return std::nullopt; }
        ConfigRef cr; cr.name = std::move(name);
        return ParamValue{cr};
    }

    switch (expected) {
        case ParamType::Double: {
            if (!val.is_number() || val.is_boolean()) {
                fail("expected JSON number");
                return std::nullopt;
            }
            double d = val.get<double>();
            if (!std::isfinite(d)) { fail("NaN or Inf not allowed"); return std::nullopt; }
            return ParamValue{d};
        }
        case ParamType::Int: {
            if (!val.is_number_integer()) {
                fail("expected JSON integer (no float/bool coercion)");
                return std::nullopt;
            }
            // js numbers > int64 range: nlohmann exposes as is_number_integer
            // but get<int64>() can still overflow to int. Range-check to int.
            std::int64_t i64 = val.get<std::int64_t>();
            if (i64 < static_cast<std::int64_t>(std::numeric_limits<int>::min()) ||
                i64 > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
                fail("integer out of range for kernel int");
                return std::nullopt;
            }
            return ParamValue{static_cast<int>(i64)};
        }
        case ParamType::Bool: {
            if (!val.is_boolean()) { fail("expected JSON bool"); return std::nullopt; }
            return ParamValue{val.get<bool>()};
        }
        case ParamType::String: {
            if (!val.is_string()) { fail("expected JSON string"); return std::nullopt; }
            return ParamValue{val.get<std::string>()};
        }
        case ParamType::Pnt: {
            if (!expectTypeTag(val, "point", reasonOut)) return std::nullopt;
            double x, y, z;
            if (!requireNumber(val, "x", x, reasonOut) ||
                !requireNumber(val, "y", y, reasonOut) ||
                !requireNumber(val, "z", z, reasonOut)) return std::nullopt;
            return ParamValue{gp_Pnt(x, y, z)};
        }
        case ParamType::Vec: {
            if (!expectTypeTag(val, "vec", reasonOut)) return std::nullopt;
            double x, y, z;
            if (!requireNumber(val, "x", x, reasonOut) ||
                !requireNumber(val, "y", y, reasonOut) ||
                !requireNumber(val, "z", z, reasonOut)) return std::nullopt;
            return ParamValue{gp_Vec(x, y, z)};
        }
        case ParamType::Dir: {
            if (!expectTypeTag(val, "dir", reasonOut)) return std::nullopt;
            double x, y, z;
            if (!requireNumber(val, "x", x, reasonOut) ||
                !requireNumber(val, "y", y, reasonOut) ||
                !requireNumber(val, "z", z, reasonOut)) return std::nullopt;
            // gp_Dir normalises internally; a zero vector throws. Catch it.
            if (std::abs(x) + std::abs(y) + std::abs(z) < 1e-12) {
                fail("direction has zero magnitude");
                return std::nullopt;
            }
            return ParamValue{gp_Dir(x, y, z)};
        }
        case ParamType::Ax1: {
            if (!expectTypeTag(val, "ax1", reasonOut)) return std::nullopt;
            double px, py, pz, dx, dy, dz;
            if (!requireNumber(val, "px", px, reasonOut) ||
                !requireNumber(val, "py", py, reasonOut) ||
                !requireNumber(val, "pz", pz, reasonOut) ||
                !requireNumber(val, "dx", dx, reasonOut) ||
                !requireNumber(val, "dy", dy, reasonOut) ||
                !requireNumber(val, "dz", dz, reasonOut)) return std::nullopt;
            if (std::abs(dx) + std::abs(dy) + std::abs(dz) < 1e-12) {
                fail("axis direction has zero magnitude");
                return std::nullopt;
            }
            return ParamValue{gp_Ax1(gp_Pnt(px, py, pz), gp_Dir(dx, dy, dz))};
        }
        case ParamType::Ax2: {
            if (!expectTypeTag(val, "ax2", reasonOut)) return std::nullopt;
            double px, py, pz, dx, dy, dz, xx, xy, xz;
            if (!requireNumber(val, "px", px, reasonOut) ||
                !requireNumber(val, "py", py, reasonOut) ||
                !requireNumber(val, "pz", pz, reasonOut) ||
                !requireNumber(val, "dx", dx, reasonOut) ||
                !requireNumber(val, "dy", dy, reasonOut) ||
                !requireNumber(val, "dz", dz, reasonOut) ||
                !requireNumber(val, "xx", xx, reasonOut) ||
                !requireNumber(val, "xy", xy, reasonOut) ||
                !requireNumber(val, "xz", xz, reasonOut)) return std::nullopt;
            if (std::abs(dx) + std::abs(dy) + std::abs(dz) < 1e-12 ||
                std::abs(xx) + std::abs(xy) + std::abs(xz) < 1e-12) {
                fail("Ax2 axis or xDir has zero magnitude");
                return std::nullopt;
            }
            return ParamValue{
                gp_Ax2(gp_Pnt(px, py, pz), gp_Dir(dx, dy, dz), gp_Dir(xx, xy, xz))
            };
        }
        case ParamType::Pln: {
            if (!expectTypeTag(val, "pln", reasonOut)) return std::nullopt;
            double px, py, pz, dx, dy, dz;
            if (!requireNumber(val, "px", px, reasonOut) ||
                !requireNumber(val, "py", py, reasonOut) ||
                !requireNumber(val, "pz", pz, reasonOut) ||
                !requireNumber(val, "dx", dx, reasonOut) ||
                !requireNumber(val, "dy", dy, reasonOut) ||
                !requireNumber(val, "dz", dz, reasonOut)) return std::nullopt;
            if (std::abs(dx) + std::abs(dy) + std::abs(dz) < 1e-12) {
                fail("plane normal has zero magnitude");
                return std::nullopt;
            }
            return ParamValue{gp_Pln(gp_Pnt(px, py, pz), gp_Dir(dx, dy, dz))};
        }
        case ParamType::ElemRef: {
            ElementRef ref;
            if (!decodeElementRef(val, ref, reasonOut)) return std::nullopt;
            return ParamValue{ref};
        }
        case ParamType::ElemRefList: {
            if (!val.is_array()) { fail("expected JSON array for ElemRefList"); return std::nullopt; }
            std::vector<ElementRef> refs;
            refs.reserve(val.size());
            for (std::size_t i = 0; i < val.size(); ++i) {
                ElementRef ref;
                std::string itemReason;
                if (!decodeElementRef(val[i], ref, &itemReason)) {
                    fail("ElementRef[" + std::to_string(i) + "]: " + itemReason);
                    return std::nullopt;
                }
                refs.push_back(std::move(ref));
            }
            return ParamValue{refs};
        }
        case ParamType::ConfigRef: {
            if (!expectTypeTag(val, "configRef", reasonOut)) return std::nullopt;
            if (!val.contains("name") || !val["name"].is_string()) {
                fail("ConfigRef missing string 'name'");
                return std::nullopt;
            }
            std::string name = val["name"].get<std::string>();
            if (name.empty()) { fail("ConfigRef 'name' must be non-empty"); return std::nullopt; }
            ConfigRef cr; cr.name = std::move(name);
            return ParamValue{cr};
        }
    }
    fail("unknown ParamType enum value");
    return std::nullopt;
}

const char* paramTypeToString(ParamType t) {
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

ParamType paramTypeFromString(const std::string& s, bool* ok) {
    if (ok) *ok = true;
    if (s == "Double")         return ParamType::Double;
    if (s == "Int")            return ParamType::Int;
    if (s == "Bool")           return ParamType::Bool;
    if (s == "String")         return ParamType::String;
    if (s == "Vec")            return ParamType::Vec;
    if (s == "Pnt")            return ParamType::Pnt;
    if (s == "Dir")            return ParamType::Dir;
    if (s == "Ax1")            return ParamType::Ax1;
    if (s == "Ax2")            return ParamType::Ax2;
    if (s == "Pln")            return ParamType::Pln;
    if (s == "ElementRef")     return ParamType::ElemRef;
    if (s == "ElementRefList") return ParamType::ElemRefList;
    if (s == "ConfigRef")      return ParamType::ConfigRef;
    if (ok) *ok = false;
    return ParamType::Double;
}

} // namespace oreo
