// SPDX-License-Identifier: LGPL-2.1-or-later

// config.cpp — ConfigSchema, ConfigValue, resolveConfigRefs.
// See docs/part-studio-as-function.md for the design.

#include "config.h"
#include "core/diagnostic.h"

#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <type_traits>

namespace oreo {

namespace {

constexpr double kConfigZeroEpsilon = 1e-12;

bool isFinite(double v) { return std::isfinite(v); }

// Variant-index → human-readable name (duplicates paramTypeName but
// avoids pulling feature_schema.cpp internals into our scope).
const char* indexName(std::size_t idx) {
    static const char* names[] = {
        "Double", "Int", "Bool", "String",
        "Vec", "Pnt", "Dir", "Ax1", "Ax2", "Pln",
        "ElementRef", "ElementRefList", "ConfigRef",
    };
    if (idx < sizeof(names) / sizeof(names[0])) return names[idx];
    return "Unknown";
}

// True iff the type is legal as a config input (ElementRef and
// ConfigRef types are not — configs are inputs, not references into
// the studio's own output).
bool isLegalConfigInputType(ParamType t) {
    switch (t) {
        case ParamType::ElemRef:
        case ParamType::ElemRefList:
        case ParamType::ConfigRef:
            return false;
        default:
            return true;
    }
}

// Validate (type, value) + numeric bounds + NaN/Inf. Returns empty
// string on success; a failure reason string otherwise.
std::string checkValueAgainstSpec(const ConfigInputSpec& spec,
                                  const ParamValue& v) {
    if (v.index() != static_cast<std::size_t>(spec.type)) {
        std::ostringstream oss;
        oss << "type mismatch: spec expects " << indexName(static_cast<std::size_t>(spec.type))
            << ", value has " << indexName(v.index());
        return oss.str();
    }
    switch (spec.type) {
        case ParamType::Double: {
            double d = std::get<double>(v);
            if (!isFinite(d)) return "value is NaN/Inf";
            if (spec.minInclusive && d < *spec.minInclusive) {
                std::ostringstream oss;
                oss << d << " < min " << *spec.minInclusive;
                return oss.str();
            }
            if (spec.maxInclusive && d > *spec.maxInclusive) {
                std::ostringstream oss;
                oss << d << " > max " << *spec.maxInclusive;
                return oss.str();
            }
            return {};
        }
        case ParamType::Int: {
            double d = static_cast<double>(std::get<int>(v));
            if (spec.minInclusive && d < *spec.minInclusive) {
                std::ostringstream oss;
                oss << d << " < min " << *spec.minInclusive;
                return oss.str();
            }
            if (spec.maxInclusive && d > *spec.maxInclusive) {
                std::ostringstream oss;
                oss << d << " > max " << *spec.maxInclusive;
                return oss.str();
            }
            return {};
        }
        case ParamType::Vec: {
            const auto& vec = std::get<gp_Vec>(v);
            if (!isFinite(vec.X()) || !isFinite(vec.Y()) || !isFinite(vec.Z()))
                return "vector has NaN/Inf component";
            return {};
        }
        case ParamType::Pnt: {
            const auto& p = std::get<gp_Pnt>(v);
            if (!isFinite(p.X()) || !isFinite(p.Y()) || !isFinite(p.Z()))
                return "point has NaN/Inf coordinate";
            return {};
        }
        case ParamType::Dir: {
            const auto& d = std::get<gp_Dir>(v);
            if (!isFinite(d.X()) || !isFinite(d.Y()) || !isFinite(d.Z()))
                return "direction has NaN/Inf component";
            return {};
        }
        default:
            return {};  // other types have no extra invariants for config inputs
    }
}

} // anonymous namespace

// ─── ConfigSchema ────────────────────────────────────────────────

int ConfigSchema::addInput(KernelContext& ctx, ConfigInputSpec spec) {
    if (spec.name.empty()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                         "ConfigSchema::addInput: name must not be empty");
        return static_cast<int>(ErrorCode::INVALID_INPUT);
    }
    if (find(spec.name) != nullptr) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                         "ConfigSchema::addInput: duplicate input name '"
                         + spec.name + "'");
        return static_cast<int>(ErrorCode::INVALID_INPUT);
    }
    if (!isLegalConfigInputType(spec.type)) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                         "ConfigSchema::addInput: ElementRef / ConfigRef "
                         "types are not valid as config inputs (input '"
                         + spec.name + "')");
        return static_cast<int>(ErrorCode::INVALID_INPUT);
    }
    // Validate the default.
    std::string reason = checkValueAgainstSpec(spec, spec.defaultValue);
    if (!reason.empty()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                         "ConfigSchema::addInput: default for '" + spec.name
                         + "' is invalid: " + reason);
        return static_cast<int>(ErrorCode::INVALID_INPUT);
    }
    inputs_.push_back(std::move(spec));
    return static_cast<int>(ErrorCode::OK);
}

const ConfigInputSpec* ConfigSchema::find(const std::string& name) const {
    for (const auto& s : inputs_) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

// ─── Schema fingerprint (ConfigSchema::fingerprint) ─────────────────
//
// Rolling FNV-1a 64 over each input's identity + bounds + default.
// Stable across process runs and build configs — never persisted, so
// we do not need cryptographic strength, just collision resistance
// "good enough" to distinguish two different ConfigSchemas. We fold
// doubles by their raw IEEE-754 byte representation so NaN/−0/+0
// differences propagate into the hash.
namespace {

constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
constexpr std::uint64_t kFnvPrime  = 0x100000001b3ULL;

inline void fnvUpdate(std::uint64_t& h, const void* data, std::size_t n) {
    const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= static_cast<std::uint64_t>(p[i]);
        h *= kFnvPrime;
    }
}
inline void fnvUpdate(std::uint64_t& h, std::uint64_t v) {
    fnvUpdate(h, &v, sizeof(v));
}
inline void fnvUpdate(std::uint64_t& h, double v) {
    std::uint64_t bits = 0;
    static_assert(sizeof(v) == sizeof(bits), "double is not 64 bits");
    std::memcpy(&bits, &v, sizeof(bits));
    fnvUpdate(h, bits);
}
inline void fnvUpdate(std::uint64_t& h, const std::string& s) {
    std::uint64_t n = s.size();
    fnvUpdate(h, n);
    if (!s.empty()) fnvUpdate(h, s.data(), s.size());
}
inline void fnvUpdateOpt(std::uint64_t& h, const std::optional<double>& o) {
    fnvUpdate(h, static_cast<std::uint64_t>(o ? 1 : 0));
    if (o) fnvUpdate(h, *o);
}

// Fold one ParamValue default into the hash. Unknown/future alternatives
// get a sentinel tag so two different future shapes cannot collide with
// the current ones.
void fnvUpdateParamValue(std::uint64_t& h, const ParamValue& v) {
    std::uint64_t tag = static_cast<std::uint64_t>(v.index());
    fnvUpdate(h, tag);
    std::visit([&h](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, double>) {
            fnvUpdate(h, arg);
        } else if constexpr (std::is_same_v<T, int>) {
            fnvUpdate(h, static_cast<std::uint64_t>(static_cast<std::int64_t>(arg)));
        } else if constexpr (std::is_same_v<T, bool>) {
            fnvUpdate(h, static_cast<std::uint64_t>(arg ? 1 : 0));
        } else if constexpr (std::is_same_v<T, std::string>) {
            fnvUpdate(h, arg);
        } else if constexpr (std::is_same_v<T, gp_Vec>) {
            fnvUpdate(h, arg.X()); fnvUpdate(h, arg.Y()); fnvUpdate(h, arg.Z());
        } else if constexpr (std::is_same_v<T, gp_Pnt>) {
            fnvUpdate(h, arg.X()); fnvUpdate(h, arg.Y()); fnvUpdate(h, arg.Z());
        } else if constexpr (std::is_same_v<T, gp_Dir>) {
            fnvUpdate(h, arg.X()); fnvUpdate(h, arg.Y()); fnvUpdate(h, arg.Z());
        } else if constexpr (std::is_same_v<T, gp_Ax1>) {
            auto p = arg.Location(); auto d = arg.Direction();
            fnvUpdate(h, p.X()); fnvUpdate(h, p.Y()); fnvUpdate(h, p.Z());
            fnvUpdate(h, d.X()); fnvUpdate(h, d.Y()); fnvUpdate(h, d.Z());
        } else if constexpr (std::is_same_v<T, gp_Ax2>) {
            auto p = arg.Location(); auto d = arg.Direction(); auto x = arg.XDirection();
            fnvUpdate(h, p.X()); fnvUpdate(h, p.Y()); fnvUpdate(h, p.Z());
            fnvUpdate(h, d.X()); fnvUpdate(h, d.Y()); fnvUpdate(h, d.Z());
            fnvUpdate(h, x.X()); fnvUpdate(h, x.Y()); fnvUpdate(h, x.Z());
        } else if constexpr (std::is_same_v<T, gp_Pln>) {
            auto p = arg.Location(); auto d = arg.Axis().Direction();
            fnvUpdate(h, p.X()); fnvUpdate(h, p.Y()); fnvUpdate(h, p.Z());
            fnvUpdate(h, d.X()); fnvUpdate(h, d.Y()); fnvUpdate(h, d.Z());
        } else if constexpr (std::is_same_v<T, ElementRef>) {
            fnvUpdate(h, arg.featureId);
            fnvUpdate(h, arg.elementName);
            fnvUpdate(h, arg.elementType);
        } else if constexpr (std::is_same_v<T, std::vector<ElementRef>>) {
            fnvUpdate(h, static_cast<std::uint64_t>(arg.size()));
            for (const auto& r : arg) {
                fnvUpdate(h, r.featureId);
                fnvUpdate(h, r.elementName);
                fnvUpdate(h, r.elementType);
            }
        } else if constexpr (std::is_same_v<T, ConfigRef>) {
            fnvUpdate(h, arg.name);
        }
    }, v);
}

} // anonymous namespace

std::uint64_t ConfigSchema::fingerprint() const noexcept {
    std::uint64_t h = kFnvOffset;
    // Version tag — bump if the hashing algorithm changes so stale
    // fingerprints from old builds cannot collide with new ones.
    fnvUpdate(h, std::uint64_t{1});
    fnvUpdate(h, static_cast<std::uint64_t>(inputs_.size()));
    for (const auto& spec : inputs_) {
        fnvUpdate(h, spec.name);
        fnvUpdate(h, static_cast<std::uint64_t>(spec.type));
        fnvUpdateOpt(h, spec.minInclusive);
        fnvUpdateOpt(h, spec.maxInclusive);
        fnvUpdateParamValue(h, spec.defaultValue);
    }
    return h;
}

// ─── ConfigValue ─────────────────────────────────────────────────

ConfigValue ConfigValue::fromSchemaDefaults(const ConfigSchema& schema) {
    ConfigValue cv;
    for (const auto& spec : schema.inputs()) {
        cv.values_[spec.name] = spec.defaultValue;
    }
    return cv;
}

const ParamValue* ConfigValue::find(const std::string& name) const {
    auto it = values_.find(name);
    if (it == values_.end()) return nullptr;
    return &it->second;
}

int ConfigValue::set(KernelContext& ctx, const ConfigSchema& schema,
                     const std::string& name, ParamValue v) {
    const auto* spec = schema.find(name);
    if (!spec) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                         "ConfigValue::set: unknown input '" + name + "'");
        return static_cast<int>(ErrorCode::INVALID_INPUT);
    }
    std::string reason = checkValueAgainstSpec(*spec, v);
    if (!reason.empty()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                         "ConfigValue::set: input '" + name + "' — " + reason);
        return static_cast<int>(ErrorCode::INVALID_INPUT);
    }
    values_[name] = std::move(v);
    return static_cast<int>(ErrorCode::OK);
}

int ConfigValue::setDouble(KernelContext& ctx, const ConfigSchema& schema,
                           const std::string& name, double v) {
    return set(ctx, schema, name, ParamValue(v));
}
int ConfigValue::setInt(KernelContext& ctx, const ConfigSchema& schema,
                        const std::string& name, int v) {
    return set(ctx, schema, name, ParamValue(v));
}
int ConfigValue::setBool(KernelContext& ctx, const ConfigSchema& schema,
                         const std::string& name, bool v) {
    return set(ctx, schema, name, ParamValue(v));
}
int ConfigValue::setString(KernelContext& ctx, const ConfigSchema& schema,
                           const std::string& name, std::string v) {
    return set(ctx, schema, name, ParamValue(std::move(v)));
}
int ConfigValue::setVec(KernelContext& ctx, const ConfigSchema& schema,
                        const std::string& name, const gp_Vec& v) {
    return set(ctx, schema, name, ParamValue(v));
}
int ConfigValue::setPnt(KernelContext& ctx, const ConfigSchema& schema,
                        const std::string& name, const gp_Pnt& v) {
    return set(ctx, schema, name, ParamValue(v));
}
int ConfigValue::setDir(KernelContext& ctx, const ConfigSchema& schema,
                        const std::string& name, const gp_Dir& v) {
    return set(ctx, schema, name, ParamValue(v));
}

// ─── validateConfig ──────────────────────────────────────────────

int validateConfig(KernelContext& ctx, const ConfigSchema& schema,
                   const ConfigValue& config) {
    for (const auto& spec : schema.inputs()) {
        const ParamValue* bound = config.find(spec.name);
        if (!bound) {
            // Fallback to schema default — which was validated at
            // addInput time, so this path is always OK.
            continue;
        }
        std::string reason = checkValueAgainstSpec(spec, *bound);
        if (!reason.empty()) {
            ctx.diag().error(ErrorCode::INVALID_INPUT,
                             "config input '" + spec.name + "': " + reason);
            return static_cast<int>(ErrorCode::INVALID_INPUT);
        }
    }
    // Warn (not fail) on config values that don't correspond to any
    // schema input — forward compat for future schema additions.
    for (const auto& kv : config.values()) {
        if (!schema.find(kv.first)) {
            ctx.diag().warning(ErrorCode::INVALID_INPUT,
                               "config carries value for unknown input '"
                               + kv.first + "' (ignored)");
        }
    }
    (void)kConfigZeroEpsilon;  // reserved for future non-zero checks on Vec inputs
    return static_cast<int>(ErrorCode::OK);
}

// ─── resolveConfigRefs ───────────────────────────────────────────

Feature resolveConfigRefs(const Feature& feature,
                          const ConfigSchema& schema,
                          const ConfigValue& config) {
    Feature out = feature;  // copy: id, type, suppressed, status, errorMessage, params

    for (auto& kv : out.params) {
        auto* cr = std::get_if<ConfigRef>(&kv.second);
        if (!cr) continue;

        const std::string& refName = cr->name;
        if (refName.empty()) {
            out.status = FeatureStatus::BrokenReference;
            out.errorMessage = "ConfigRef has empty name on param '"
                             + kv.first + "'";
            return out;
        }
        const ConfigInputSpec* spec = schema.find(refName);
        if (!spec) {
            out.status = FeatureStatus::BrokenReference;
            out.errorMessage = "ConfigRef '" + refName + "' on param '"
                             + kv.first + "' does not match any config "
                             + "input in the studio schema";
            return out;
        }
        const ParamValue* bound = config.find(refName);
        kv.second = bound ? *bound : spec->defaultValue;
    }
    return out;
}

} // namespace oreo
