// SPDX-License-Identifier: LGPL-2.1-or-later

// feature_tree.h — Parametric feature tree with replay engine.
//
// The FeatureTree is an ordered list of Features. When replayed,
// features execute top-to-bottom, each building on the result of
// the previous one. Element references are resolved at replay time
// using MappedNames from cached results.
//
// This is the core of parametric CAD:
//   1. User adds features (extrude, fillet, hole...)
//   2. Each feature references elements by stable name
//   3. When a parameter changes, replay from that feature onward
//   4. Stable names survive the replay → downstream features still work

#ifndef OREO_FEATURE_TREE_H
#define OREO_FEATURE_TREE_H

#include "feature.h"
#include "core/kernel_context.h"
#include "naming/named_shape.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace oreo {

// Pre-execute hook. Called by replayFrom on each feature just before
// it dispatches to executeFeature. Receives the stored feature, returns
// a transformed copy that actually runs. Used by PartStudio::execute to
// substitute ConfigRef placeholders before validation + dispatch — the
// stored tree is never mutated, so replaying the same tree with two
// different configs yields two deterministic outputs.
//
// Status + errorMessage on the returned feature are mirrored back to
// the stored feature so getBrokenFeatures() reflects the transformed
// execution (e.g. a ConfigRef pointing at an unknown input becomes a
// BrokenReference on the stored feature).
using FeatureTransformer = std::function<Feature(const Feature&)>;

class FeatureTree {
public:
    // Create with an explicit context (preferred)
    explicit FeatureTree(std::shared_ptr<KernelContext> ctx)
        : ctx_(std::move(ctx)) {}

    // Default constructor uses default context (deprecated)
    FeatureTree() : ctx_(KernelContext::create()) {}

    // Access the context
    KernelContext& context() { return *ctx_; }
    const KernelContext& context() const { return *ctx_; }

    // ── Feature management ────────────────────────────────

    // Add a feature to the end of the tree. Returns false (and emits an
    // INVALID_STATE diagnostic on the bound context) if a feature with the
    // same id already exists. Uniqueness of featureId is a core invariant:
    // replay caches, dirty tracking and downstream element-ref resolution
    // all index by id, and a silent duplicate would corrupt all three.
    // [[nodiscard]] — callers must observe the failure; at least one
    // direct-C++ caller prior to 2026-04-18 was ignoring it and silently
    // admitting duplicates (see docs/PROTOTYPE-V0.md audit, finding #8).
    [[nodiscard]] bool addFeature(const Feature& feature);

    // Insert a feature at a specific position (0-based index). Same
    // duplicate-id contract as addFeature; returns false on collision.
    [[nodiscard]] bool insertFeature(int index, const Feature& feature);

    // Remove a feature by ID. Returns false if not found.
    bool removeFeature(const std::string& featureId);

    // Update a parameter on a feature. Marks it and all downstream features dirty.
    void updateParameter(const std::string& featureId,
                         const std::string& paramName,
                         const ParamValue& value);

    // Suppress/unsuppress a feature
    void suppressFeature(const std::string& featureId, bool suppress);

    // Move a feature to a different position in the ordered list.
    // Marks every feature from min(oldIdx, newIdx) onward dirty so the
    // next replay rebuilds the affected window. Returns false if
    // featureId is unknown. newIndex is clamped to [0, featureCount).
    bool moveFeature(const std::string& featureId, int newIndex);

    // Walk every feature's params and rewrite ElementRefs whose
    // featureId matches `oldFeatureId` to point at `newFeatureId`
    // instead. Returns the number of refs rewritten. Marks every
    // feature whose params changed (and everything downstream) as
    // dirty so the next replay re-resolves the references.
    int replaceReference(const std::string& oldFeatureId,
                         const std::string& newFeatureId);

    // Get a feature by ID (const)
    const Feature* getFeature(const std::string& id) const;

    // Get all features (ordered)
    const std::vector<Feature>& features() const { return features_; }

    // Number of features
    int featureCount() const { return static_cast<int>(features_.size()); }

    // ── Replay engine ─────────────────────────────────────

    // Install / clear the pre-execute feature transformer. Passing an
    // empty std::function (nullptr) disables the hook. The transformer
    // is installed on the tree, not on a per-replay basis, so callers
    // that want single-shot behaviour should install, replay, then
    // clear. PartStudio::execute follows that pattern.
    void setFeatureTransformer(FeatureTransformer t) noexcept {
        featureTransformer_ = std::move(t);
    }
    bool hasFeatureTransformer() const noexcept {
        return static_cast<bool>(featureTransformer_);
    }

    // Replay the entire tree from scratch.
    // Returns the final shape (or null if tree is empty or all features failed).
    NamedShape replay();

    // Replay from a specific feature onward (incremental replay).
    // Features before dirtyFrom use cached results.
    NamedShape replayFrom(const std::string& dirtyFromId);

    // Onshape-style rollback marker. -1 means "roll to end"; otherwise
    // the index is the last feature included in the visible rollback state.
    void setRollbackIndex(int index);
    int rollbackIndex() const { return rollbackIndex_; }
    void rollToEnd() { rollbackIndex_ = -1; }
    NamedShape replayToRollback();

    // ── Shape access ──────────────────────────────────────

    // Get the cached result of a specific feature (must be replayed first)
    NamedShape getShapeAt(const std::string& featureId) const;

    // Get the final result (after last feature)
    NamedShape getFinalShape() const;

    // Rollback: get the shape just before a specific feature
    NamedShape rollbackTo(const std::string& featureId) const;

    // ── Diagnostics ───────────────────────────────────────

    // Get all broken features (status == BrokenReference or ExecutionFailed)
    std::vector<const Feature*> getBrokenFeatures() const;

    // Check if the tree has any broken features
    bool hasBrokenFeatures() const;

    // ── Serialization ─────────────────────────────────────

    // Serialize the feature tree to a JSON string
    std::string toJSON() const;

    // Deserialize from JSON. Returns a FeatureTreeFromJsonResult so
    // callers can detect malformed input — the previous silent-fallback
    // API (returning an empty tree on parse failure) made it impossible
    // to distinguish "user really wanted an empty tree" from "the
    // payload was corrupt". Returns ok=true only when:
    //   - the JSON parses
    //   - the top-level shape matches the FeatureTree schema
    //     (object containing a "features" array)
    //   - every feature has a non-empty id and type
    //   - feature count fits within ctx.quotas().maxFeatures (when
    //     a context is supplied via the second overload)
    // On failure, .tree is empty, .ok is false, and .error names the
    // first violation (suitable for diagnostics).
    static struct FeatureTreeFromJsonResult fromJSON(const std::string& json);

    // Ctx-aware overload: same parse, plus quota enforcement against
    // ctx.quotas().maxFeatures and any failure routed through
    // ctx.diag() so it surfaces in the caller's diagnostic stream.
    static struct FeatureTreeFromJsonResult fromJSON(KernelContext& ctx,
                                                     const std::string& json);

private:
    std::shared_ptr<KernelContext> ctx_;
    std::vector<Feature> features_;

    // Cache: featureId → result NamedShape after execution
    std::map<std::string, NamedShape> cache_;

    // Dirty set: features that need re-execution
    std::map<std::string, bool> dirty_;

    // Allocator state captured immediately before each feature executes.
    // Incremental replay restores this snapshot before replaying a dirty
    // feature so operation identities match full replay.
    std::map<std::string, TagAllocator::SnapshotV2> allocatorSnapshotsBefore_;

    int rollbackIndex_ = -1;

    // Optional pre-execute transformer (see FeatureTransformer above).
    // Default-constructed to "no transformer"; PartStudio::execute sets
    // it for the duration of one call.
    FeatureTransformer featureTransformer_;

    // Find feature index by ID. Returns -1 if not found.
    int findIndex(const std::string& id) const;

    // Mark a feature and all subsequent features as dirty
    void markDirtyFrom(int index);

    // Reference resolver: looks up element references in cached shapes
    ResolvedRef resolveReference(const ElementRef& ref) const;
};

// Result from FeatureTree::fromJSON. Lives at namespace scope (not
// nested in FeatureTree) because a nested struct member of the
// enclosing-class type would reference an incomplete type.
struct FeatureTreeFromJsonResult {
    FeatureTree tree;
    bool        ok = false;
    std::string error;
};

} // namespace oreo

#endif // OREO_FEATURE_TREE_H
