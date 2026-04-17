// SPDX-License-Identifier: LGPL-2.1-or-later

// test_sketch_persistence.cpp — Phase D6 acceptance suite.
//
// Verifies the cloud-collab contract for sketch entity IDs:
//   * IDs returned by oreo_*_sketch_add_* are slot indices that
//     never shift after later add / remove / re-add operations.
//   * Removing an entity cascades to constraints that reference it.
//   * The sketch can be re-solved across edits without losing IDs.
//   * slot_count grows monotonically; live_count tracks the live set.
//
// Most tests use the ctx-aware C API (which exposes the new
// remove/alive/count surface). One test asserts the legacy
// oreo_sketch_* path still works unchanged for the no-delete case.

#include <gtest/gtest.h>

#include "oreo_kernel.h"

namespace {

struct CtxGuard {
    OreoContext h = nullptr;
    explicit CtxGuard(OreoContext c) : h(c) {}
    ~CtxGuard() { if (h) oreo_context_free(h); }
    CtxGuard(const CtxGuard&) = delete;
    CtxGuard& operator=(const CtxGuard&) = delete;
};

struct SketchGuard {
    OreoSketch h = nullptr;
    explicit SketchGuard(OreoSketch s) : h(s) {}
    ~SketchGuard() { if (h) oreo_ctx_sketch_free(h); }
    SketchGuard(const SketchGuard&) = delete;
    SketchGuard& operator=(const SketchGuard&) = delete;
};

// ConstraintType::Horizontal == 13 per src/sketch/oreo_sketch.h order.
constexpr int kHorizontal = 13;
// ConstraintType::Coincident == 0
constexpr int kCoincident = 0;
// ConstraintType::Distance  == 6
constexpr int kDistance = 6;

} // anonymous namespace

// ════════════════════════════════════════════════════════════════
// Stable IDs across add/remove/add
// ════════════════════════════════════════════════════════════════

TEST(SketchPersistence, AddingPointsReturnsMonotonicallyIncreasingIds) {
    CtxGuard ctx(oreo_context_create());
    SketchGuard s(oreo_ctx_sketch_create(ctx.h));
    int p0 = oreo_ctx_sketch_add_point(s.h, 0, 0);
    int p1 = oreo_ctx_sketch_add_point(s.h, 1, 0);
    int p2 = oreo_ctx_sketch_add_point(s.h, 2, 0);
    EXPECT_EQ(p0, 0);
    EXPECT_EQ(p1, 1);
    EXPECT_EQ(p2, 2);
}

TEST(SketchPersistence, RemovingMiddleEntityKeepsOuterIdsStable) {
    CtxGuard ctx(oreo_context_create());
    SketchGuard s(oreo_ctx_sketch_create(ctx.h));
    int p0 = oreo_ctx_sketch_add_point(s.h, 1, 1);
    int p1 = oreo_ctx_sketch_add_point(s.h, 2, 2);
    int p2 = oreo_ctx_sketch_add_point(s.h, 3, 3);
    ASSERT_EQ(oreo_ctx_sketch_remove_point(s.h, p1), OREO_OK);

    // p0 and p2 retain their values.
    double x = 0, y = 0;
    EXPECT_EQ(oreo_ctx_sketch_get_point(s.h, p0, &x, &y), OREO_OK);
    EXPECT_DOUBLE_EQ(x, 1.0);
    EXPECT_DOUBLE_EQ(y, 1.0);
    EXPECT_EQ(oreo_ctx_sketch_get_point(s.h, p2, &x, &y), OREO_OK);
    EXPECT_DOUBLE_EQ(x, 3.0);
    EXPECT_DOUBLE_EQ(y, 3.0);

    // p1 is dead.
    EXPECT_EQ(oreo_ctx_sketch_get_point(s.h, p1, &x, &y), OREO_INVALID_INPUT);
    EXPECT_EQ(oreo_ctx_sketch_point_alive(s.h, p1), 0);
    EXPECT_EQ(oreo_ctx_sketch_point_alive(s.h, p0), 1);
    EXPECT_EQ(oreo_ctx_sketch_point_alive(s.h, p2), 1);
}

TEST(SketchPersistence, AddAfterRemoveDoesNotReusePublicId) {
    CtxGuard ctx(oreo_context_create());
    SketchGuard s(oreo_ctx_sketch_create(ctx.h));
    int p0 = oreo_ctx_sketch_add_point(s.h, 0, 0);
    int p1 = oreo_ctx_sketch_add_point(s.h, 1, 0);
    int p2 = oreo_ctx_sketch_add_point(s.h, 2, 0);
    ASSERT_EQ(oreo_ctx_sketch_remove_point(s.h, p1), OREO_OK);
    int p3 = oreo_ctx_sketch_add_point(s.h, 3, 0);
    EXPECT_EQ(p3, 3);                 // never reused
    EXPECT_NE(p3, p1);
    EXPECT_EQ(oreo_ctx_sketch_point_slot_count(s.h), 4);
    EXPECT_EQ(oreo_ctx_sketch_point_live_count(s.h), 3);
}

TEST(SketchPersistence, RemoveOfRemovedReturnsInvalidInput) {
    CtxGuard ctx(oreo_context_create());
    SketchGuard s(oreo_ctx_sketch_create(ctx.h));
    int p = oreo_ctx_sketch_add_point(s.h, 0, 0);
    ASSERT_EQ(oreo_ctx_sketch_remove_point(s.h, p), OREO_OK);
    EXPECT_EQ(oreo_ctx_sketch_remove_point(s.h, p), OREO_INVALID_INPUT);
}

// ════════════════════════════════════════════════════════════════
// Cascade: removing an entity removes constraints that reference it
// ════════════════════════════════════════════════════════════════

TEST(SketchPersistence, RemovingEntityCascadesToReferencingConstraints) {
    CtxGuard ctx(oreo_context_create());
    SketchGuard s(oreo_ctx_sketch_create(ctx.h));
    int p0 = oreo_ctx_sketch_add_point(s.h, 0, 0);
    int p1 = oreo_ctx_sketch_add_point(s.h, 1, 0);
    int p2 = oreo_ctx_sketch_add_point(s.h, 2, 0);
    int c01 = oreo_ctx_sketch_add_constraint(s.h, kCoincident, p0, p1, -1, 0.0);
    int c02 = oreo_ctx_sketch_add_constraint(s.h, kCoincident, p0, p2, -1, 0.0);
    int c12 = oreo_ctx_sketch_add_constraint(s.h, kCoincident, p1, p2, -1, 0.0);

    // Sanity: all three constraints are alive.
    EXPECT_EQ(oreo_ctx_sketch_constraint_live_count(s.h), 3);

    ASSERT_EQ(oreo_ctx_sketch_remove_point(s.h, p1), OREO_OK);

    // Constraints touching p1 (c01 and c12) should be tombstoned;
    // c02 (which references p0 and p2) survives.
    EXPECT_EQ(oreo_ctx_sketch_constraint_alive(s.h, c01), 0);
    EXPECT_EQ(oreo_ctx_sketch_constraint_alive(s.h, c02), 1);
    EXPECT_EQ(oreo_ctx_sketch_constraint_alive(s.h, c12), 0);
    EXPECT_EQ(oreo_ctx_sketch_constraint_live_count(s.h), 1);
    EXPECT_EQ(oreo_ctx_sketch_constraint_slot_count(s.h), 3);
}

// ════════════════════════════════════════════════════════════════
// Solve survives across edits without renumbering
// ════════════════════════════════════════════════════════════════

TEST(SketchPersistence, SolveAfterRemoveProducesValidGeometry) {
    CtxGuard ctx(oreo_context_create());
    SketchGuard s(oreo_ctx_sketch_create(ctx.h));
    int l0 = oreo_ctx_sketch_add_line(s.h, 0, 0, 10, 0.5);  // small slope
    int l1 = oreo_ctx_sketch_add_line(s.h, 0, 5, 10, 5);
    // Constrain l0 horizontal.
    int c0 = oreo_ctx_sketch_add_constraint(s.h, kHorizontal, l0, -1, -1, 0.0);
    EXPECT_GE(c0, 0);

    EXPECT_EQ(oreo_ctx_sketch_solve(s.h), OREO_OK);

    // After solve, l0 should be horizontal (y1 == y2).
    double x1, y1, x2, y2;
    EXPECT_EQ(oreo_ctx_sketch_get_line(s.h, l0, &x1, &y1, &x2, &y2), OREO_OK);
    EXPECT_NEAR(y1, y2, 1e-6);

    // Now delete l1, re-solve, l0 must still be horizontal AND its ID
    // unchanged.
    ASSERT_EQ(oreo_ctx_sketch_remove_line(s.h, l1), OREO_OK);
    EXPECT_EQ(oreo_ctx_sketch_line_alive(s.h, l1), 0);
    EXPECT_EQ(oreo_ctx_sketch_line_alive(s.h, l0), 1);

    EXPECT_EQ(oreo_ctx_sketch_solve(s.h), OREO_OK);
    EXPECT_EQ(oreo_ctx_sketch_get_line(s.h, l0, &x1, &y1, &x2, &y2), OREO_OK);
    EXPECT_NEAR(y1, y2, 1e-6);
}

TEST(SketchPersistence, SolveSkipsConstraintsWithDeadEntities) {
    // Sanity: even if we somehow have a constraint referencing a
    // removed entity (e.g. user removed without going through the
    // public remove API), the solve must not crash.
    CtxGuard ctx(oreo_context_create());
    SketchGuard s(oreo_ctx_sketch_create(ctx.h));
    int p0 = oreo_ctx_sketch_add_point(s.h, 0, 0);
    int p1 = oreo_ctx_sketch_add_point(s.h, 5, 5);
    int c  = oreo_ctx_sketch_add_constraint(s.h, kDistance, p0, p1, -1, 7.0);
    EXPECT_GE(c, 0);
    // remove p1 — cascades and kills the distance constraint.
    ASSERT_EQ(oreo_ctx_sketch_remove_point(s.h, p1), OREO_OK);
    // Re-add a new point — gets ID 2.
    int p2 = oreo_ctx_sketch_add_point(s.h, 5, 5);
    EXPECT_EQ(p2, 2);
    // Solve still works.
    EXPECT_EQ(oreo_ctx_sketch_solve(s.h), OREO_OK);
}

// ════════════════════════════════════════════════════════════════
// to_wire ignores tombstones
// ════════════════════════════════════════════════════════════════

TEST(SketchPersistence, ToWireSkipsRemovedLines) {
    CtxGuard ctx(oreo_context_create());
    SketchGuard s(oreo_ctx_sketch_create(ctx.h));
    int l0 = oreo_ctx_sketch_add_line(s.h, 0, 0, 10, 0);
    int l1 = oreo_ctx_sketch_add_line(s.h, 10, 0, 10, 5);
    int l2 = oreo_ctx_sketch_add_line(s.h, 10, 5, 0, 5);
    int l3 = oreo_ctx_sketch_add_line(s.h, 0, 5, 0, 0);
    EXPECT_EQ(oreo_ctx_sketch_solve(s.h), OREO_OK);
    OreoWire w1 = oreo_ctx_sketch_to_wire(s.h);
    EXPECT_NE(w1, nullptr);
    if (w1) oreo_free_wire(w1);

    // Remove one edge — wire generator must not crash and must produce
    // something (or null on disconnected wire).
    ASSERT_EQ(oreo_ctx_sketch_remove_line(s.h, l2), OREO_OK);
    EXPECT_EQ(oreo_ctx_sketch_line_alive(s.h, l2), 0);
    EXPECT_EQ(oreo_ctx_sketch_line_alive(s.h, l0), 1);
    EXPECT_EQ(oreo_ctx_sketch_line_alive(s.h, l1), 1);
    EXPECT_EQ(oreo_ctx_sketch_line_alive(s.h, l3), 1);
    OreoWire w2 = oreo_ctx_sketch_to_wire(s.h);
    // Wire may be null because the boundary is now open — but the
    // call must NOT crash.
    if (w2) oreo_free_wire(w2);
}

// ════════════════════════════════════════════════════════════════
// Slot bookkeeping
// ════════════════════════════════════════════════════════════════

TEST(SketchPersistence, SlotCountNeverDecreases) {
    CtxGuard ctx(oreo_context_create());
    SketchGuard s(oreo_ctx_sketch_create(ctx.h));
    EXPECT_EQ(oreo_ctx_sketch_point_slot_count(s.h), 0);
    int p0 = oreo_ctx_sketch_add_point(s.h, 0, 0);
    int p1 = oreo_ctx_sketch_add_point(s.h, 1, 0);
    EXPECT_EQ(oreo_ctx_sketch_point_slot_count(s.h), 2);
    EXPECT_EQ(oreo_ctx_sketch_point_live_count(s.h), 2);

    ASSERT_EQ(oreo_ctx_sketch_remove_point(s.h, p0), OREO_OK);
    EXPECT_EQ(oreo_ctx_sketch_point_slot_count(s.h), 2);  // unchanged
    EXPECT_EQ(oreo_ctx_sketch_point_live_count(s.h), 1);

    int p2 = oreo_ctx_sketch_add_point(s.h, 2, 0);
    EXPECT_EQ(p2, 2);
    EXPECT_EQ(oreo_ctx_sketch_point_slot_count(s.h), 3);
    EXPECT_EQ(oreo_ctx_sketch_point_live_count(s.h), 2);
    (void)p1;
}

TEST(SketchPersistence, NullHandleSurvivesEveryAccessor) {
    EXPECT_EQ(oreo_ctx_sketch_remove_point(nullptr, 0), OREO_INVALID_INPUT);
    EXPECT_EQ(oreo_ctx_sketch_remove_line(nullptr, 0), OREO_INVALID_INPUT);
    EXPECT_EQ(oreo_ctx_sketch_remove_circle(nullptr, 0), OREO_INVALID_INPUT);
    EXPECT_EQ(oreo_ctx_sketch_remove_arc(nullptr, 0), OREO_INVALID_INPUT);
    EXPECT_EQ(oreo_ctx_sketch_remove_constraint(nullptr, 0), OREO_INVALID_INPUT);
    EXPECT_EQ(oreo_ctx_sketch_point_alive(nullptr, 0), 0);
    EXPECT_EQ(oreo_ctx_sketch_point_slot_count(nullptr), 0);
    EXPECT_EQ(oreo_ctx_sketch_point_live_count(nullptr), 0);
}

// ════════════════════════════════════════════════════════════════
// Constraint ID stability (constraint IDs are also slot indices)
// ════════════════════════════════════════════════════════════════

TEST(SketchPersistence, ConstraintIdsAreStableAcrossEdits) {
    CtxGuard ctx(oreo_context_create());
    SketchGuard s(oreo_ctx_sketch_create(ctx.h));
    int p0 = oreo_ctx_sketch_add_point(s.h, 0, 0);
    int p1 = oreo_ctx_sketch_add_point(s.h, 1, 1);
    int c0 = oreo_ctx_sketch_add_constraint(s.h, kDistance, p0, p1, -1, 5.0);
    EXPECT_EQ(c0, 0);

    ASSERT_EQ(oreo_ctx_sketch_remove_constraint(s.h, c0), OREO_OK);
    EXPECT_EQ(oreo_ctx_sketch_constraint_alive(s.h, c0), 0);

    int c1 = oreo_ctx_sketch_add_constraint(s.h, kDistance, p0, p1, -1, 7.0);
    EXPECT_EQ(c1, 1);  // not reused
    EXPECT_EQ(oreo_ctx_sketch_constraint_slot_count(s.h), 2);
}
