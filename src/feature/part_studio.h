// SPDX-License-Identifier: LGPL-2.1-or-later

// part_studio.h — Onshape-style Part Studio, shaped as a pure typed function.
//
// A PartStudio is the cloud-document unit: a single ordered FeatureTree
// plus a declared ConfigSchema of typed inputs, bound to a KernelContext
// (which carries documentId + units). The typed function contract is:
//
//     PartStudio(inputs: ConfigSchema) -> PartStudioOutputs
//
// Call `execute(ConfigValue)` to run it; call `executeWithDefaults()` for
// the common case of running with every input at its schema default.
// The stored feature tree is never mutated — a config value is an
// argument, not state, so multiple configs can be evaluated from the
// same studio cheaply.
//
// See docs/part-studio-as-function.md for the full design.
//
// The legacy `replay()` / `replayToRollback()` / `replayFrom()` methods
// are preserved as thin aliases so every existing call site continues
// to work against the v1 "linear timeline" framing.

#ifndef OREO_PART_STUDIO_H
#define OREO_PART_STUDIO_H

#include "config.h"
#include "feature_tree.h"
#include "core/kernel_context.h"
#include "core/units.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace oreo {

// ═══════════════════════════════════════════════════════════════
// PartStudioOutputs — the function's return value
// ═══════════════════════════════════════════════════════════════

struct PartStudioOutputs {
    enum class Status { Ok, Partial, Failed };

    // The shape produced by the last non-suppressed, non-null feature.
    // Null when the tree is empty or every feature failed.
    NamedShape finalShape;

    // Named outputs — keyed on feature id. Populated for every
    // non-suppressed feature whose type carries the `isExport` flag
    // (today: no built-in feature type carries that flag; this keeps
    // the ABI additive for a future `Export { name, shape }` feature
    // without another wire-format bump).
    std::map<std::string, NamedShape> namedParts;

    // Non-owning pointers into the tree's features vector. Valid only
    // until the next tree mutation. Callers who want a stable list
    // should copy the relevant fields.
    std::vector<const Feature*> brokenFeatures;

    Status status = Status::Ok;
};

// ═══════════════════════════════════════════════════════════════
// PartStudio — the aggregate
// ═══════════════════════════════════════════════════════════════

class PartStudio {
public:
    PartStudio(std::shared_ptr<KernelContext> ctx, std::string name = "")
        : ctx_(std::move(ctx)),
          tree_(ctx_),
          name_(std::move(name)) {}

    // ── Identity / metadata ──────────────────────────────────

    const std::string& name() const noexcept { return name_; }
    void setName(std::string n) noexcept { name_ = std::move(n); }

    std::uint64_t documentId() const noexcept {
        return ctx_ ? ctx_->tags().documentId() : 0;
    }
    const UnitSystem& units() const noexcept { return ctx_->units(); }

    KernelContext&       context()       noexcept { return *ctx_; }
    const KernelContext& context() const noexcept { return *ctx_; }
    std::shared_ptr<KernelContext> contextHandle() const noexcept { return ctx_; }

    // ── Feature tree access ──────────────────────────────────

    FeatureTree&       tree()       noexcept { return tree_; }
    const FeatureTree& tree() const noexcept { return tree_; }
    int featureCount() const { return tree_.featureCount(); }

    // ── Config schema access ─────────────────────────────────

    ConfigSchema&       configSchema()       noexcept { return schema_; }
    const ConfigSchema& configSchema() const noexcept { return schema_; }

    // ── The function ─────────────────────────────────────────
    //
    // execute() is deterministic in (tree, schema, config, ctx.documentId).
    // Repeated calls with the same ConfigValue yield identical shape
    // identities and geometry — see docs/part-studio-as-function.md §3.2.
    //
    // Pre-conditions enforced internally:
    //   * validateConfig(schema, config)     → INVALID_INPUT on failure
    //   * every ConfigRef in a feature param resolves against the schema
    //   * feature-level schema validation (delegated to executeFeature)
    // Failures short-circuit to status=Failed; partial success (some
    // features broken but a final shape was produced) returns status=Partial.
    PartStudioOutputs execute(const ConfigValue& config);

    // Convenience: execute with every config input at its schema default.
    PartStudioOutputs executeWithDefaults() {
        return execute(ConfigValue::fromSchemaDefaults(schema_));
    }

    // ── Legacy linear-timeline aliases (kept for back-compat) ─

    NamedShape replay()             { return executeWithDefaults().finalShape; }
    NamedShape replayToRollback()   { return tree_.replayToRollback(); }
    NamedShape replayFrom(const std::string& dirtyId) { return tree_.replayFrom(dirtyId); }

    void setRollbackIndex(int idx)  { tree_.setRollbackIndex(idx); }
    int  rollbackIndex() const      { return tree_.rollbackIndex(); }
    void rollToEnd()                { tree_.rollToEnd(); }

    // ── Serialization ────────────────────────────────────────
    //
    // v2 PartStudio envelope wraps the FeatureTree JSON with:
    //   _schema       — "oreo.part_studio"
    //   _version      — schema::PART_STUDIO (≥ 2.0.0)
    //   name          — display name
    //   documentId    — stringified uint64 (informational; load uses ctx docId)
    //   configSchema  — array of ConfigInputSpec objects
    //   tree          — nested FeatureTree JSON
    //
    // fromJSON accepts:
    //   a) v2 PartStudio envelope (preferred)
    //   b) v1 PartStudio envelope (no configSchema) — auto-upgraded to
    //      empty schema
    //   c) bare FeatureTree JSON — auto-wrapped into a studio with
    //      empty schema + empty name (back-compat for early callers)
    std::string toJSON() const;

    struct FromJsonResult {
        std::unique_ptr<PartStudio> studio;
        bool        ok = false;
        std::string error;
    };
    static FromJsonResult fromJSON(std::shared_ptr<KernelContext> ctx,
                                   const std::string& json);

private:
    std::shared_ptr<KernelContext> ctx_;
    FeatureTree tree_;
    ConfigSchema schema_;
    std::string name_;
};

} // namespace oreo

#endif // OREO_PART_STUDIO_H
