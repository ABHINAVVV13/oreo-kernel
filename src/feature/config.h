// SPDX-License-Identifier: LGPL-2.1-or-later

// config.h — Part Studio configuration: typed inputs for the pure-function ABI.
//
// See docs/part-studio-as-function.md for the full design. Short version:
//
//   A PartStudio is a function: execute(ConfigValue) -> PartStudioOutputs.
//   ConfigSchema declares the typed inputs; ConfigValue binds values at
//   call time. Feature parameters use ConfigRef placeholders to reference
//   config values; resolveConfigRefs substitutes before validation +
//   execution, producing a pure dependency of (tree, config) -> outputs.

#ifndef OREO_CONFIG_H
#define OREO_CONFIG_H

#include "feature.h"
#include "feature_schema.h"
#include "core/kernel_context.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace oreo {

// ═══════════════════════════════════════════════════════════════
// ConfigInputSpec — one declared input of a Part Studio function
// ═══════════════════════════════════════════════════════════════

struct ConfigInputSpec {
    std::string name;              // unique within a ConfigSchema
    ParamType   type;              // reuses ParamType enum; ElementRef kinds illegal
    ParamValue  defaultValue;      // must match `type`
    std::string description;

    // Numeric bounds for Double / Int inputs. Ignored for other types.
    std::optional<double> minInclusive;
    std::optional<double> maxInclusive;
};

// ═══════════════════════════════════════════════════════════════
// ConfigSchema — the declared input list for a Part Studio
// ═══════════════════════════════════════════════════════════════

class ConfigSchema {
public:
    ConfigSchema() = default;

    // Add one input. Returns OREO_OK on success and emits a diagnostic +
    // returns an error code on:
    //   - duplicate name        → INVALID_INPUT
    //   - empty name            → INVALID_INPUT
    //   - ElementRef[List] type → INVALID_INPUT (refs are studio outputs,
    //                              not inputs — see §2.1 of the design doc)
    //   - default's variant index ≠ type               → INVALID_INPUT
    //   - default numeric out of [min, max] bounds     → OUT_OF_RANGE
    //   - default numeric NaN/Inf                      → OUT_OF_RANGE
    //
    // On failure, the spec is NOT added.
    int addInput(KernelContext& ctx, ConfigInputSpec spec);

    // Lookup by name. Returns nullptr if absent.
    const ConfigInputSpec* find(const std::string& name) const;

    // Iteration + size.
    const std::vector<ConfigInputSpec>& inputs() const noexcept { return inputs_; }
    std::size_t inputCount() const noexcept { return inputs_.size(); }
    bool empty() const noexcept { return inputs_.empty(); }

    // Clear the schema — used during fromJSON rebuild and by tests.
    void clear() noexcept { inputs_.clear(); }

    // Stable 64-bit content hash over every input's (name, type, default,
    // min, max) tuple, in declaration order. Two schemas produce the
    // same fingerprint iff their input lists are semantically equal.
    //
    // Used by the OreoConfig C ABI guard to detect "config built for
    // studio A, passed to studio B" misuse: oreo_config_create snapshots
    // the studio's fingerprint; oreo_ctx_part_studio_execute rejects a
    // config whose fingerprint no longer matches the target studio's
    // current schema. Audit 2026-04-19 finding P1/P2: previously the
    // ABI accepted any config against any studio and relied on the
    // "unknown input" warning to notice mismatches, which lets a schema
    // overlap silently run with misleading values.
    std::uint64_t fingerprint() const noexcept;

private:
    std::vector<ConfigInputSpec> inputs_;
};

// ═══════════════════════════════════════════════════════════════
// ConfigValue — runtime binding of values to a schema's inputs
// ═══════════════════════════════════════════════════════════════

class ConfigValue {
public:
    ConfigValue() = default;

    // Populate with every input's default value. Safe against a schema
    // containing a default that is itself a ConfigRef — which would be
    // malformed; addInput rejects it. Still, the resolver guards against
    // that case at execute time.
    static ConfigValue fromSchemaDefaults(const ConfigSchema& schema);

    // Per-type setters. On name / type / bound mismatch, emit a
    // diagnostic and return an error code; the value is not stored.
    int setDouble(KernelContext& ctx, const ConfigSchema& schema,
                  const std::string& name, double v);
    int setInt   (KernelContext& ctx, const ConfigSchema& schema,
                  const std::string& name, int v);
    int setBool  (KernelContext& ctx, const ConfigSchema& schema,
                  const std::string& name, bool v);
    int setString(KernelContext& ctx, const ConfigSchema& schema,
                  const std::string& name, std::string v);
    int setVec   (KernelContext& ctx, const ConfigSchema& schema,
                  const std::string& name, const gp_Vec& v);
    int setPnt   (KernelContext& ctx, const ConfigSchema& schema,
                  const std::string& name, const gp_Pnt& v);
    int setDir   (KernelContext& ctx, const ConfigSchema& schema,
                  const std::string& name, const gp_Dir& v);

    // Lower-level: set a ParamValue directly. Validates variant index
    // matches spec.type and numeric bounds for numerics. For Double / Int
    // values, NaN / Inf are rejected.
    int set(KernelContext& ctx, const ConfigSchema& schema,
            const std::string& name, ParamValue v);

    // Lookup. Returns nullptr when absent; callers should fall back to
    // the matching ConfigInputSpec::defaultValue from the schema (which
    // is what resolveConfigRefs does automatically).
    const ParamValue* find(const std::string& name) const;

    // Iteration surface (for serialisation).
    const std::map<std::string, ParamValue>& values() const noexcept { return values_; }
    std::size_t size() const noexcept { return values_.size(); }

    void clear() noexcept { values_.clear(); }

private:
    std::map<std::string, ParamValue> values_;
};

// ═══════════════════════════════════════════════════════════════
// validateConfig — structural check before execute dispatches
// ═══════════════════════════════════════════════════════════════
//
// Walks every input in `schema` and confirms that either:
//   a. `config` supplies a value of the correct variant index + numeric
//      range, OR
//   b. `config` does not supply a value and the schema's default is
//      usable (defaults are pre-validated at addInput time so this is
//      always true when reached).
//
// On any violation, emits a diagnostic and returns a non-OK error code.
// Returns OREO_OK when all inputs resolve to valid values.
int validateConfig(KernelContext& ctx, const ConfigSchema& schema,
                   const ConfigValue& config);

// ═══════════════════════════════════════════════════════════════
// resolveConfigRefs — substitute ConfigRef params in a feature
// ═══════════════════════════════════════════════════════════════
//
// Returns a copy of `feature` with every ParamValue of variant
// ConfigRef replaced by the resolved value (config[name] if set, else
// schema.find(name)->defaultValue). If resolution fails (unknown name),
// the returned feature carries:
//   - status = FeatureStatus::BrokenReference
//   - errorMessage describing the offending param name
//
// Does NOT mutate `feature`. Callers should feed the returned feature
// into validateFeature + executeFeature; a BrokenReference status is
// honoured by both and short-circuits cleanly.
Feature resolveConfigRefs(const Feature& feature,
                          const ConfigSchema& schema,
                          const ConfigValue& config);

} // namespace oreo

#endif // OREO_CONFIG_H
