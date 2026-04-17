// SPDX-License-Identifier: LGPL-2.1-or-later

// test_replay_golden.cpp — Phase D4 acceptance suite.
//
// Cloud collaboration requires deterministic replay so that:
//   * The server and the client compute identical topology IDs from the
//     same feature tree, regardless of order in which features were
//     authored or which thread loaded the document.
//   * Incremental replay from a dirty feature produces the SAME
//     identities for all unchanged downstream features as a full
//     from-scratch replay would.
//
// These tests build small canonical feature trees, capture every
// face/edge/vertex identity, then re-build / re-replay and assert
// the identity bag matches exactly.

#include <gtest/gtest.h>

#include "core/kernel_context.h"
#include "feature/feature.h"
#include "feature/feature_tree.h"
#include "feature/part_studio.h"
#include "naming/element_map.h"
#include "naming/named_shape.h"

#include <set>
#include <string>
#include <vector>

namespace {

// Snapshot every face / edge / vertex identity from a NamedShape into
// a sorted set of {kind, id}. Order-independent — the test wants to
// know "did the same identity bag come out", not "in what order".
struct IdEntry {
    std::string kind;
    oreo::ShapeIdentity id;
    bool operator<(const IdEntry& o) const {
        if (kind != o.kind) return kind < o.kind;
        if (id.documentId != o.id.documentId) return id.documentId < o.id.documentId;
        return id.counter < o.id.counter;
    }
    bool operator==(const IdEntry& o) const {
        return kind == o.kind
            && id.documentId == o.id.documentId
            && id.counter    == o.id.counter;
    }
};

std::set<IdEntry> snapshot(const oreo::NamedShape& ns) {
    std::set<IdEntry> bag;
    if (ns.isNull() || !ns.elementMap()) return bag;
    auto pull = [&](const char* kind, TopAbs_ShapeEnum t) {
        const int n = ns.countSubShapes(t);
        for (int i = 1; i <= n; ++i) {
            auto name = ns.getElementName(oreo::IndexedName(kind, i));
            auto id = oreo::ElementMap::extractShapeIdentity(name);
            bag.insert({kind, id});
        }
    };
    pull("Face",   TopAbs_FACE);
    pull("Edge",   TopAbs_EDGE);
    pull("Vertex", TopAbs_VERTEX);
    return bag;
}

oreo::FeatureTree buildBoxThenOffset(std::shared_ptr<oreo::KernelContext> ctx) {
    oreo::FeatureTree tree(ctx);
    oreo::Feature box;
    box.id = "F1";
    box.type = "MakeBox";
    box.params["dimensions"] = gp_Vec(10, 20, 30);
    tree.addFeature(box);

    oreo::Feature off;
    off.id = "F2";
    off.type = "Offset";
    off.params["distance"] = 1.0;
    tree.addFeature(off);
    return tree;
}

} // anonymous namespace

// ════════════════════════════════════════════════════════════════
// Two replays of the same tree in the same context produce identical
// identity bags (caching invariant).
// ════════════════════════════════════════════════════════════════

TEST(ReplayGolden, RepeatedReplaySameContextMatches) {
    oreo::KernelConfig cfg; cfg.documentId = 0x42;
    auto ctx = oreo::KernelContext::create(cfg);
    auto tree = buildBoxThenOffset(ctx);

    auto s1 = tree.replay();
    auto bag1 = snapshot(s1);

    auto s2 = tree.replay();
    auto bag2 = snapshot(s2);

    EXPECT_EQ(bag1, bag2);
}

// ════════════════════════════════════════════════════════════════
// Two FRESH contexts with the same documentId produce IDENTICAL bags
// — replay is reproducible across server/client.
// ════════════════════════════════════════════════════════════════

TEST(ReplayGolden, FreshContextSameDocIdMatches) {
    oreo::KernelConfig cfg; cfg.documentId = 0x99;
    auto ctxA = oreo::KernelContext::create(cfg);
    auto treeA = buildBoxThenOffset(ctxA);
    auto bagA = snapshot(treeA.replay());

    auto ctxB = oreo::KernelContext::create(cfg);
    auto treeB = buildBoxThenOffset(ctxB);
    auto bagB = snapshot(treeB.replay());

    EXPECT_EQ(bagA, bagB);
}

// ════════════════════════════════════════════════════════════════
// Different documentIds → bags differ (in docId), but counts align.
// ════════════════════════════════════════════════════════════════

TEST(ReplayGolden, DifferentDocIdsHaveSameStructureButDifferentIdentity) {
    oreo::KernelConfig cfgA; cfgA.documentId = 0x111;
    oreo::KernelConfig cfgB; cfgB.documentId = 0x222;
    auto ctxA = oreo::KernelContext::create(cfgA);
    auto ctxB = oreo::KernelContext::create(cfgB);
    auto bagA = snapshot(buildBoxThenOffset(ctxA).replay());
    auto bagB = snapshot(buildBoxThenOffset(ctxB).replay());

    EXPECT_EQ(bagA.size(), bagB.size()); // same topology
    // Every entry's docId in bagA must be 0x111; bagB's is 0x222.
    for (const auto& e : bagA) EXPECT_EQ(e.id.documentId, 0x111u);
    for (const auto& e : bagB) EXPECT_EQ(e.id.documentId, 0x222u);
}

// ════════════════════════════════════════════════════════════════
// Incremental replay produces same bag as full replay
// ════════════════════════════════════════════════════════════════

TEST(ReplayGolden, IncrementalReplayMatchesFullReplay) {
    oreo::KernelConfig cfg; cfg.documentId = 0x55;
    auto ctxFull = oreo::KernelContext::create(cfg);
    auto treeFull = buildBoxThenOffset(ctxFull);
    auto bagFull = snapshot(treeFull.replay());

    auto ctxInc = oreo::KernelContext::create(cfg);
    auto treeInc = buildBoxThenOffset(ctxInc);
    treeInc.replay();
    // Edit a downstream parameter then replay incrementally from F2.
    // Restore the original value first so incremental and full must agree.
    treeInc.updateParameter("F2", "distance", 2.5);
    treeInc.replay();
    treeInc.updateParameter("F2", "distance", 1.0);
    auto bagInc = snapshot(treeInc.replayFrom("F2"));

    EXPECT_EQ(bagFull, bagInc);
}

// ════════════════════════════════════════════════════════════════
// PartStudio JSON round-trip preserves replay identities
// ════════════════════════════════════════════════════════════════

TEST(ReplayGolden, PartStudioJsonRoundTripPreservesIdentity) {
    oreo::KernelConfig cfg; cfg.documentId = 0xABC;
    auto ctx = oreo::KernelContext::create(cfg);
    oreo::PartStudio studio(ctx, "PrototypePlate");

    oreo::Feature box;
    box.id = "F1"; box.type = "MakeBox";
    box.params["dimensions"] = gp_Vec(15, 25, 5);
    studio.tree().addFeature(box);

    auto bag1 = snapshot(studio.replay());
    auto json = studio.toJSON();

    // Reload into a fresh context with the SAME docId.
    auto ctx2 = oreo::KernelContext::create(cfg);
    auto restored = oreo::PartStudio::fromJSON(ctx2, json);
    ASSERT_TRUE(restored.ok) << restored.error;
    auto bag2 = snapshot(restored.studio->replay());
    EXPECT_EQ(bag1, bag2);
    EXPECT_EQ(restored.studio->name(), "PrototypePlate");
}
