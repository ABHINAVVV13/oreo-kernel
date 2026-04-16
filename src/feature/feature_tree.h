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

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace oreo {

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

    // Add a feature to the end of the tree
    void addFeature(const Feature& feature);

    // Insert a feature at a specific position (0-based index)
    void insertFeature(int index, const Feature& feature);

    // Remove a feature by ID. Returns false if not found.
    bool removeFeature(const std::string& featureId);

    // Update a parameter on a feature. Marks it and all downstream features dirty.
    void updateParameter(const std::string& featureId,
                         const std::string& paramName,
                         const ParamValue& value);

    // Suppress/unsuppress a feature
    void suppressFeature(const std::string& featureId, bool suppress);

    // Get a feature by ID (const)
    const Feature* getFeature(const std::string& id) const;

    // Get all features (ordered)
    const std::vector<Feature>& features() const { return features_; }

    // Number of features
    int featureCount() const { return static_cast<int>(features_.size()); }

    // ── Replay engine ─────────────────────────────────────

    // Replay the entire tree from scratch.
    // Returns the final shape (or null if tree is empty or all features failed).
    NamedShape replay();

    // Replay from a specific feature onward (incremental replay).
    // Features before dirtyFrom use cached results.
    NamedShape replayFrom(const std::string& dirtyFromId);

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

    // Deserialize from JSON. Returns empty tree on failure.
    static FeatureTree fromJSON(const std::string& json);

private:
    std::shared_ptr<KernelContext> ctx_;
    std::vector<Feature> features_;

    // Cache: featureId → result NamedShape after execution
    std::map<std::string, NamedShape> cache_;

    // Dirty set: features that need re-execution
    std::map<std::string, bool> dirty_;

    // Find feature index by ID. Returns -1 if not found.
    int findIndex(const std::string& id) const;

    // Mark a feature and all subsequent features as dirty
    void markDirtyFrom(int index);

    // Reference resolver: looks up element references in cached shapes
    ResolvedRef resolveReference(const ElementRef& ref) const;
};

} // namespace oreo

#endif // OREO_FEATURE_TREE_H
