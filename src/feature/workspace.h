// SPDX-License-Identifier: LGPL-2.1-or-later

// workspace.h — branch-identifying wrapper around a FeatureTree.
//
// A Workspace is a named snapshot of a PartStudio timeline. Two
// workspaces sharing a KernelContext share documentId; their shape
// identities live in the same namespace and can be merged cleanly.
//
// See docs/branching-merging.md for the full design. Short version:
//
//   Workspace main(ctx, "main");
//   main.tree().addFeature(boxF);
//   auto feature = main.fork("feature/handle");
//   feature->tree().addFeature(filletF);
//   // ... main.tree() also receives another feature ...
//   auto merged = threeWayMerge(main.tree().features(),
//                               main.tree().features(),          // "ours"
//                               feature->tree().features(),      // "theirs"
//                               ctx);

#ifndef OREO_WORKSPACE_H
#define OREO_WORKSPACE_H

#include "feature.h"
#include "feature_tree.h"
#include "core/kernel_context.h"

#include <memory>
#include <string>
#include <vector>

namespace oreo {

class Workspace {
public:
    Workspace(std::shared_ptr<KernelContext> ctx, std::string name);

    // Deep-copy fork. The new workspace:
    //   - shares the SAME KernelContext (documentId survives)
    //   - has its own FeatureTree (features copied, cache/dirty/snapshots NOT
    //     copied — they'll regenerate on first replay)
    //   - records this workspace's features as its baseSnapshot, for future
    //     threeWayMerge
    //   - stores this->name() as parentName
    std::unique_ptr<Workspace> fork(std::string newName) const;

    // Metadata
    const std::string& name()       const noexcept { return name_; }
    void  setName(std::string n)          noexcept { name_ = std::move(n); }
    const std::string& parentName() const noexcept { return parentName_; }

    // Tree access (live — mutations target this workspace's branch).
    FeatureTree&       tree()       noexcept { return tree_; }
    const FeatureTree& tree() const noexcept { return tree_; }

    // Base snapshot — the parent's tree at fork time. Empty vector on
    // root workspaces. The corresponding rollback index is preserved
    // because baseSnapshotRollbackIndex() can differ from the current
    // tree's rollback index.
    const std::vector<Feature>& baseSnapshot() const noexcept { return baseSnapshot_; }
    int baseSnapshotRollbackIndex() const noexcept { return baseSnapshotRollbackIndex_; }

    // KernelContext handle — shared between sibling workspaces.
    std::shared_ptr<KernelContext> contextHandle() const noexcept { return ctx_; }
    KernelContext&       context()       noexcept { return *ctx_; }
    const KernelContext& context() const noexcept { return *ctx_; }
    std::uint64_t documentId() const noexcept {
        return ctx_ ? ctx_->tags().documentId() : 0;
    }

    // Serialisation — see docs/branching-merging.md §4.1 for the envelope.
    std::string toJSON() const;

    struct FromJsonResult {
        std::unique_ptr<Workspace> workspace;
        bool        ok = false;
        std::string error;
    };
    static FromJsonResult fromJSON(std::shared_ptr<KernelContext> ctx,
                                   const std::string& json);

    // Internal: used by fromJSON to populate the base snapshot directly.
    void setBaseSnapshot(std::vector<Feature> feats, int rollbackIndex) noexcept {
        baseSnapshot_ = std::move(feats);
        baseSnapshotRollbackIndex_ = rollbackIndex;
    }
    void setParentName(std::string n) noexcept { parentName_ = std::move(n); }

private:
    std::shared_ptr<KernelContext> ctx_;
    FeatureTree tree_;
    std::string name_;
    std::string parentName_;
    std::vector<Feature> baseSnapshot_;
    int baseSnapshotRollbackIndex_ = -1;
};

} // namespace oreo

#endif // OREO_WORKSPACE_H
