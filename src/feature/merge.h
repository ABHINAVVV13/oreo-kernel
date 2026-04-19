// SPDX-License-Identifier: LGPL-2.1-or-later

// merge.h — three-way merge over FeatureTree feature vectors.
//
// The merge operates on plain `std::vector<Feature>` rather than on
// FeatureTree handles so that callers can merge any three snapshots —
// workspace current, workspace base, arbitrary persisted tree — without
// needing to construct live trees. The returned MergeResult wraps the
// merged features in a live FeatureTree bound to the caller's context.
//
// Algorithm + conflict taxonomy: see docs/branching-merging.md §3.

#ifndef OREO_MERGE_H
#define OREO_MERGE_H

#include "feature.h"
#include "feature_tree.h"
#include "core/kernel_context.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace oreo {

// ═══════════════════════════════════════════════════════════════
// MergeConflict
// ═══════════════════════════════════════════════════════════════

enum class MergeConflictKind {
    ParamConflict,         // same param modified to different values
    SuppressionConflict,   // suppressed flag toggled differently
    AddRemoveConflict,     // added in one side, removed in the other (or diverging adds)
    PositionConflict,      // moved to different indices in each side
    TypeConflict,          // same id but different `type` strings
};

struct MergeConflict {
    MergeConflictKind kind;
    std::string featureId;
    std::string paramName;   // empty unless kind == ParamConflict

    // Best-effort snapshots of the three values for display/resolution.
    // Encoded uniformly so a UI can stringify them without case-splitting.
    // For ParamConflict these hold the param values (can be inspected
    // directly with paramValueEquals / paramValueToJson).
    // For TypeConflict they hold the three type strings.
    // For SuppressionConflict / PositionConflict they hold Bool / Int
    // ParamValues with the three states.
    // For AddRemoveConflict they are advisory sentinel strings
    // ("<removed>" / "<modified>"); the authoritative payload is in
    // the feature-snapshot optionals below.
    ParamValue baseValue;
    ParamValue oursValue;
    ParamValue theirsValue;

    // Feature payloads. Populated for AddRemoveConflict (and also
    // snapshotted for every conflict kind that touches a whole
    // feature) so applyResolutions has enough information to
    // reinsert a deleted feature when the user chooses the
    // non-removing side.
    //
    // Nullopt means "absent on that side." For AddRemoveConflict
    // exactly one of oursSnapshot / theirsSnapshot is nullopt (the
    // removing side), and the other holds the modified feature.
    // baseSnapshot holds the original version from the common
    // ancestor whenever present on `base`.
    std::optional<Feature> baseSnapshot;
    std::optional<Feature> oursSnapshot;
    std::optional<Feature> theirsSnapshot;

    // Suggested merged-tree insert index for features that need to
    // be reinserted by applyResolutions. Taken from base's ordering
    // when the feature was in base, otherwise from ours/theirs' own
    // position. -1 means "append to end."
    int insertIndexHint = -1;

    std::string message;
};

// ═══════════════════════════════════════════════════════════════
// MergeResult
// ═══════════════════════════════════════════════════════════════

struct MergeResult {
    // Live, ctx-bound tree containing the merged features. Conflicts
    // (if any) are recorded separately; the corresponding feature
    // content in `merged` reflects the base value, not a guess.
    FeatureTree merged;

    std::vector<MergeConflict> conflicts;

    bool clean() const { return conflicts.empty(); }

    explicit MergeResult(std::shared_ptr<KernelContext> ctx)
        : merged(std::move(ctx)) {}
};

// ═══════════════════════════════════════════════════════════════
// threeWayMerge — the core algorithm
// ═══════════════════════════════════════════════════════════════
//
// Diffs (base → ours) and (base → theirs), combines non-conflicting
// changes, records conflicts for human resolution. The merged tree is
// constructed on `ctx` so its features allocate identities in the
// shared documentId namespace.
//
// See docs/branching-merging.md §3 for the rule table. Important
// invariants (verified by test_branch_merge.cpp):
//
//   1. Disjoint changes merge cleanly with zero conflicts.
//   2. Identical concurrent changes merge silently (no spurious conflict).
//   3. No path produces a partially-applied conflicted feature —
//      a conflict surfaces OR the change applies, never both.
MergeResult threeWayMerge(std::shared_ptr<KernelContext> ctx,
                          const std::vector<Feature>& base,
                          const std::vector<Feature>& ours,
                          const std::vector<Feature>& theirs);

// ═══════════════════════════════════════════════════════════════
// Conflict resolution
// ═══════════════════════════════════════════════════════════════

enum class ResolveChoice {
    Ours,
    Theirs,
    Base,
    Custom,  // use resolution.customValue
};

struct Resolution {
    std::string featureId;
    std::string paramName;   // "" for non-ParamConflict kinds
    ResolveChoice choice = ResolveChoice::Ours;
    ParamValue customValue;  // used iff choice == Custom
};

// Apply user-chosen resolutions to `mergedFeatures` (the features
// vector extracted from a MergeResult). Returns the final feature list
// — safe to feed into a FeatureTree or to another Workspace.
//
// Every conflict must match exactly one Resolution (by featureId +
// paramName). Unmatched conflicts route a diagnostic through `ctx.diag`
// and leave the feature at its base-state value.
std::vector<Feature> applyResolutions(KernelContext& ctx,
                                      const std::vector<Feature>& mergedFeatures,
                                      const std::vector<MergeConflict>& conflicts,
                                      const std::vector<Resolution>& resolutions);

// Human-readable name for a conflict kind. Owned by the library — do not free.
const char* mergeConflictKindName(MergeConflictKind k);

} // namespace oreo

#endif // OREO_MERGE_H
