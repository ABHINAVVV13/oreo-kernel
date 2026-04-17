// SPDX-License-Identifier: LGPL-2.1-or-later

// part_studio.h — Onshape-style Part Studio aggregate.
//
// A PartStudio is the cloud-document unit: a single ordered FeatureTree
// bound to a KernelContext (which carries documentId + units), plus a
// human-visible name. This is the v1 collaboration unit — a request
// from the server creates exactly one PartStudio per document, mutates
// its FeatureTree, replays it, and serialises it back to storage.
//
// PartStudio intentionally owns nothing the FeatureTree doesn't already
// own; its purpose is to be the single typed handle the C API (and any
// future server runtime) hands around. It exists as a named aggregate
// so callers don't have to juggle (FeatureTree, name, ctx) tuples
// everywhere.

#ifndef OREO_PART_STUDIO_H
#define OREO_PART_STUDIO_H

#include "feature_tree.h"
#include "core/kernel_context.h"
#include "core/units.h"

#include <memory>
#include <string>

namespace oreo {

class PartStudio {
public:
    // Create a new Part Studio bound to the given context. The
    // PartStudio takes a shared reference to the context so the
    // FeatureTree's replay logic always operates against the
    // document's tag allocator and diagnostic collector.
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

    // ── Replay convenience ───────────────────────────────────

    NamedShape replay()             { return tree_.replay(); }
    NamedShape replayToRollback()   { return tree_.replayToRollback(); }
    NamedShape replayFrom(const std::string& dirtyId) { return tree_.replayFrom(dirtyId); }

    void setRollbackIndex(int idx)  { tree_.setRollbackIndex(idx); }
    int  rollbackIndex() const      { return tree_.rollbackIndex(); }
    void rollToEnd()                { tree_.rollToEnd(); }

    // ── Serialization ────────────────────────────────────────
    //
    // The PartStudio document format wraps the FeatureTree JSON in an
    // outer object that captures name + documentId + units, so that a
    // round-trip through the cloud preserves all four. Older v1
    // FeatureTree-only JSON is auto-detected and lifted into a
    // PartStudio on load (with default name + ctx units).
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
    std::string name_;
};

} // namespace oreo

#endif // OREO_PART_STUDIO_H
