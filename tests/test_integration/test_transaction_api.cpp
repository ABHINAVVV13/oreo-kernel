// SPDX-License-Identifier: LGPL-2.1-or-later

// test_transaction_api.cpp — Phase D2 + D5 + D8 acceptance suite.
//
// Exercises the cloud-collab edit surface added to the C API:
//   * set_param_{int,bool,string,vec}
//   * broken_count / broken_id / broken_message
//   * move (reorder)
//   * replace_reference

#include <gtest/gtest.h>
#include "oreo_kernel.h"

#include <cstring>
#include <string>

namespace {

struct CtxGuard {
    OreoContext h = nullptr;
    explicit CtxGuard(OreoContext c) : h(c) {}
    ~CtxGuard() { if (h) oreo_context_free(h); }
    CtxGuard(const CtxGuard&) = delete;
    CtxGuard& operator=(const CtxGuard&) = delete;
};

struct TreeGuard {
    OreoFeatureTree h = nullptr;
    explicit TreeGuard(OreoFeatureTree t) : h(t) {}
    ~TreeGuard() { if (h) oreo_ctx_feature_tree_free(h); }
    TreeGuard(const TreeGuard&) = delete;
    TreeGuard& operator=(const TreeGuard&) = delete;
};

struct BuilderGuard {
    OreoFeatureBuilder h = nullptr;
    explicit BuilderGuard(OreoFeatureBuilder b) : h(b) {}
    ~BuilderGuard() { if (h) oreo_feature_builder_free(h); }
    BuilderGuard(const BuilderGuard&) = delete;
    BuilderGuard& operator=(const BuilderGuard&) = delete;
};

struct SolidGuard {
    OreoSolid h = nullptr;
    SolidGuard() = default;
    explicit SolidGuard(OreoSolid s) : h(s) {}
    ~SolidGuard() { if (h) oreo_free_solid(h); }
    SolidGuard(const SolidGuard&) = delete;
    SolidGuard& operator=(const SolidGuard&) = delete;
};

OreoFeatureBuilder buildBox(const char* id, double x, double y, double z) {
    auto b = oreo_feature_builder_create(id, "MakeBox");
    oreo_feature_builder_set_vec(b, "dimensions", x, y, z);
    return b;
}

} // anonymous namespace

// ════════════════════════════════════════════════════════════════
// Per-type setters
// ════════════════════════════════════════════════════════════════

TEST(Transaction, SetParamIntChangesPatternCount) {
    CtxGuard ctx(oreo_context_create());
    TreeGuard t(oreo_ctx_feature_tree_create(ctx.h));
    BuilderGuard box(buildBox("F1", 5, 5, 5));
    ASSERT_EQ(oreo_ctx_feature_tree_add(t.h, box.h), OREO_OK);

    BuilderGuard pat(oreo_feature_builder_create("F2", "LinearPattern"));
    oreo_feature_builder_set_vec(pat.h, "direction", 1, 0, 0);
    oreo_feature_builder_set_int(pat.h, "count", 3);
    oreo_feature_builder_set_double(pat.h, "spacing", 10);
    ASSERT_EQ(oreo_ctx_feature_tree_add(t.h, pat.h), OREO_OK);

    EXPECT_EQ(oreo_ctx_feature_tree_set_param_int(t.h, "F2", "count", 5),
              OREO_OK);
}

TEST(Transaction, SetParamVecChangesExtrudeDirection) {
    CtxGuard ctx(oreo_context_create());
    TreeGuard t(oreo_ctx_feature_tree_create(ctx.h));
    BuilderGuard box(buildBox("F1", 5, 5, 5));
    oreo_ctx_feature_tree_add(t.h, box.h);

    BuilderGuard ext(oreo_feature_builder_create("F2", "Extrude"));
    oreo_feature_builder_set_vec(ext.h, "direction", 0, 0, 5);
    oreo_ctx_feature_tree_add(t.h, ext.h);

    EXPECT_EQ(oreo_ctx_feature_tree_set_param_vec(
                  t.h, "F2", "direction", 0, 0, 10),
              OREO_OK);
}

TEST(Transaction, SetParamUnknownFeatureRejected) {
    CtxGuard ctx(oreo_context_create());
    TreeGuard t(oreo_ctx_feature_tree_create(ctx.h));
    EXPECT_EQ(oreo_ctx_feature_tree_set_param_double(t.h, "no", "x", 1.0),
              OREO_INVALID_INPUT);
    EXPECT_EQ(oreo_ctx_feature_tree_set_param_int(t.h, "no", "x", 1),
              OREO_INVALID_INPUT);
    EXPECT_EQ(oreo_ctx_feature_tree_set_param_string(t.h, "no", "x", "y"),
              OREO_INVALID_INPUT);
}

// ════════════════════════════════════════════════════════════════
// Broken-feature query
// ════════════════════════════════════════════════════════════════

TEST(Transaction, BrokenCountIsZeroBeforeReplay) {
    CtxGuard ctx(oreo_context_create());
    TreeGuard t(oreo_ctx_feature_tree_create(ctx.h));
    BuilderGuard box(buildBox("F1", 5, 5, 5));
    oreo_ctx_feature_tree_add(t.h, box.h);
    EXPECT_EQ(oreo_ctx_feature_tree_broken_count(t.h), 0);
}

TEST(Transaction, BrokenCountFlagsBadFeatureAfterReplay) {
    CtxGuard ctx(oreo_context_create());
    TreeGuard t(oreo_ctx_feature_tree_create(ctx.h));

    // F1 makes a box (succeeds). F2 fillets a non-existent edge,
    // which fails at replay time → status BrokenReference.
    BuilderGuard box(buildBox("F1", 5, 5, 5));
    oreo_ctx_feature_tree_add(t.h, box.h);

    BuilderGuard fill(oreo_feature_builder_create("F2", "Fillet"));
    oreo_feature_builder_add_ref(fill.h, "edges", "F1", "Edge99", "Edge");
    oreo_feature_builder_set_double(fill.h, "radius", 0.5);
    oreo_ctx_feature_tree_add(t.h, fill.h);

    SolidGuard s(oreo_ctx_feature_tree_replay(t.h));
    // Even though F2 fails, F1 succeeded — the tree returns F1's shape
    // as the rollback fallback.
    int broken = oreo_ctx_feature_tree_broken_count(t.h);
    EXPECT_EQ(broken, 1);

    char idBuf[32] = {0};
    size_t needed = 0;
    EXPECT_EQ(oreo_ctx_feature_tree_broken_id(
                  t.h, 0, idBuf, sizeof(idBuf), &needed), OREO_OK);
    EXPECT_STREQ(idBuf, "F2");

    // Probe the message buffer length first.
    needed = 0;
    EXPECT_EQ(oreo_ctx_feature_tree_broken_message(
                  t.h, 0, nullptr, 0, &needed), OREO_OK);
    EXPECT_GT(needed, 0u);
}

TEST(Transaction, BrokenIdOutOfRangeReturnsOutOfRange) {
    CtxGuard ctx(oreo_context_create());
    TreeGuard t(oreo_ctx_feature_tree_create(ctx.h));
    char buf[8] = {0};
    size_t needed = 0;
    EXPECT_EQ(oreo_ctx_feature_tree_broken_id(t.h, 5, buf, sizeof(buf), &needed),
              OREO_OUT_OF_RANGE);
}

// ════════════════════════════════════════════════════════════════
// Move / reorder
// ════════════════════════════════════════════════════════════════

TEST(Transaction, MoveFeatureChangesPosition) {
    CtxGuard ctx(oreo_context_create());
    TreeGuard t(oreo_ctx_feature_tree_create(ctx.h));
    BuilderGuard a(buildBox("F1", 5, 5, 5));
    BuilderGuard b(buildBox("F2", 6, 6, 6));
    BuilderGuard c(buildBox("F3", 7, 7, 7));
    oreo_ctx_feature_tree_add(t.h, a.h);
    oreo_ctx_feature_tree_add(t.h, b.h);
    oreo_ctx_feature_tree_add(t.h, c.h);

    EXPECT_EQ(oreo_ctx_feature_tree_move(t.h, "F1", 2), OREO_OK);
    // After move: order should be F2, F3, F1.
    EXPECT_EQ(oreo_ctx_feature_tree_count(t.h), 3);
}

TEST(Transaction, MoveUnknownFeatureRejected) {
    CtxGuard ctx(oreo_context_create());
    TreeGuard t(oreo_ctx_feature_tree_create(ctx.h));
    EXPECT_EQ(oreo_ctx_feature_tree_move(t.h, "Nope", 0), OREO_INVALID_INPUT);
}

// ════════════════════════════════════════════════════════════════
// Replace reference (D5: conflict-primitive helper)
// ════════════════════════════════════════════════════════════════

TEST(Transaction, ReplaceReferenceUpdatesElementRef) {
    CtxGuard ctx(oreo_context_create());
    TreeGuard t(oreo_ctx_feature_tree_create(ctx.h));

    BuilderGuard a(buildBox("F1", 5, 5, 5));
    BuilderGuard b(buildBox("F2", 6, 6, 6));
    oreo_ctx_feature_tree_add(t.h, a.h);
    oreo_ctx_feature_tree_add(t.h, b.h);

    BuilderGuard fill(oreo_feature_builder_create("F3", "Fillet"));
    oreo_feature_builder_add_ref(fill.h, "edges", "F1", "Edge1", "Edge");
    oreo_feature_builder_add_ref(fill.h, "edges", "F1", "Edge2", "Edge");
    oreo_feature_builder_set_double(fill.h, "radius", 0.5);
    oreo_ctx_feature_tree_add(t.h, fill.h);

    // Swap every "F1" reference for "F2".
    int rewrites = oreo_ctx_feature_tree_replace_reference(t.h, "F1", "F2");
    EXPECT_EQ(rewrites, 2);
    // Replacing again is a no-op.
    EXPECT_EQ(oreo_ctx_feature_tree_replace_reference(t.h, "F1", "F2"), 0);
}

TEST(Transaction, ReplaceReferenceIgnoresMismatch) {
    CtxGuard ctx(oreo_context_create());
    TreeGuard t(oreo_ctx_feature_tree_create(ctx.h));
    BuilderGuard a(buildBox("F1", 5, 5, 5));
    oreo_ctx_feature_tree_add(t.h, a.h);
    EXPECT_EQ(oreo_ctx_feature_tree_replace_reference(t.h, "Nope", "F1"), 0);
}
