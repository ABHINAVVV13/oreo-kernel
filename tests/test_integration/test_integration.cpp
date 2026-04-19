// SPDX-License-Identifier: LGPL-2.1-or-later

// test_integration.cpp — End-to-end integration tests for oreo-kernel.
//
// Each TEST exercises a workflow that CROSSES SUBSYSTEM BOUNDARIES
// (sketch -> geometry -> query, feature tree -> STEP, etc.).
// Subsystem-focused tests live in tests/test_*/ for their own subsystem;
// this file is for integration coverage only.

#include <gtest/gtest.h>

#include "oreo_kernel.h"
#include "core/kernel_context.h"
#include "core/cancellation.h"
#include "core/diagnostic.h"
#include "core/shape_identity.h"
#include "feature/feature.h"
#include "feature/feature_tree.h"
#include "geometry/oreo_geometry.h"
#include "io/oreo_step.h"
#include "io/oreo_serialize.h"
#include "query/oreo_query.h"
#include "naming/named_shape.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

// ── RAII guards for the C-API handles — tests must not leak ─────────
struct SolidGuard {
    OreoSolid h = nullptr;
    SolidGuard() = default;
    explicit SolidGuard(OreoSolid s) : h(s) {}
    SolidGuard(const SolidGuard&) = delete;
    SolidGuard& operator=(const SolidGuard&) = delete;
    SolidGuard(SolidGuard&& o) noexcept : h(o.h) { o.h = nullptr; }
    SolidGuard& operator=(SolidGuard&& o) noexcept {
        if (this != &o) { reset(o.h); o.h = nullptr; }
        return *this;
    }
    ~SolidGuard() { if (h) oreo_free_solid(h); }
    OreoSolid release() { auto t = h; h = nullptr; return t; }
    void reset(OreoSolid s = nullptr) {
        if (h && h != s) oreo_free_solid(h);
        h = s;
    }
    OreoSolid get() const { return h; }
    explicit operator bool() const { return h != nullptr; }
};

struct EdgeGuard {
    OreoEdge h = nullptr;
    explicit EdgeGuard(OreoEdge e = nullptr) : h(e) {}
    EdgeGuard(const EdgeGuard&) = delete;
    EdgeGuard& operator=(const EdgeGuard&) = delete;
    ~EdgeGuard() { if (h) oreo_free_edge(h); }
    OreoEdge get() const { return h; }
};

struct WireGuard {
    OreoWire h = nullptr;
    explicit WireGuard(OreoWire w = nullptr) : h(w) {}
    WireGuard(const WireGuard&) = delete;
    WireGuard& operator=(const WireGuard&) = delete;
    ~WireGuard() { if (h) oreo_free_wire(h); }
    OreoWire get() const { return h; }
};

struct SketchGuard {
    OreoSketch h = nullptr;
    SketchGuard() : h(oreo_sketch_create()) {}
    SketchGuard(const SketchGuard&) = delete;
    SketchGuard& operator=(const SketchGuard&) = delete;
    ~SketchGuard() { if (h) oreo_sketch_free(h); }
    OreoSketch get() const { return h; }
};

struct CtxGuard {
    OreoContext h = nullptr;
    CtxGuard() : h(oreo_context_create()) {}
    CtxGuard(const CtxGuard&) = delete;
    CtxGuard& operator=(const CtxGuard&) = delete;
    ~CtxGuard() { if (h) oreo_context_free(h); }
    OreoContext get() const { return h; }
};

// Build the rectangle sketch -> wire path used by W1 / W7.
// Returns an OreoWire (caller owns via WireGuard) or nullptr on failure.
OreoWire buildRectWire(double w, double h) {
    SketchGuard sk;
    if (!sk.get()) return nullptr;

    // Four lines forming a closed rectangle. Initial positions are
    // already consistent; we don't add constraints — the solver treats
    // this as under-constrained (status = OK).
    oreo_sketch_add_line(sk.get(), 0.0, 0.0, w, 0.0);   // bottom
    oreo_sketch_add_line(sk.get(), w, 0.0, w,  h);       // right
    oreo_sketch_add_line(sk.get(), w,  h,  0.0, h);      // top
    oreo_sketch_add_line(sk.get(), 0.0, h,  0.0, 0.0);   // left

    int status = oreo_sketch_solve(sk.get());
    if (status != OREO_OK) return nullptr;

    return oreo_sketch_to_wire(sk.get());
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════
// W1: Sketch -> extrude -> fillet -> boolean -> mass properties
// ═══════════════════════════════════════════════════════════════════
//
// Touches: sketch, geometry (extrude/fillet/boolean/primitives), query.

TEST(Integration, W1_SketchExtrudeFilletBooleanQuery) {
    oreo_init();

    // 1. Sketch a rectangle wire (50 x 30).
    WireGuard rect(buildRectWire(50.0, 30.0));
    ASSERT_NE(rect.get(), nullptr) << "sketch->wire failed";

    // 2. Build a face from the wire, then extrude 20 up.
    SolidGuard face(oreo_make_face_from_wire(rect.get()));
    ASSERT_TRUE(face) << "make_face_from_wire failed";

    SolidGuard extruded(oreo_extrude(face.get(), 0.0, 0.0, 20.0));
    ASSERT_TRUE(extruded) << "extrude failed";

    // Extruded rectangle should be a prism: 6 faces, 12 edges.
    EXPECT_EQ(oreo_face_count(extruded.get()), 6);
    EXPECT_EQ(oreo_edge_count(extruded.get()), 12);

    // 3. Fillet the first edge with a small radius.
    EdgeGuard edge0(oreo_get_edge(extruded.get(), 1));
    ASSERT_NE(edge0.get(), nullptr);

    OreoEdge edgeArr[1] = {edge0.get()};
    SolidGuard filleted(oreo_fillet(extruded.get(), edgeArr, 1, 2.0));
    ASSERT_TRUE(filleted) << "fillet failed";
    EXPECT_GT(oreo_face_count(filleted.get()), 6);

    // 4. Subtract a through-hole cylinder.
    SolidGuard cyl(oreo_make_cylinder(5.0, 40.0));
    ASSERT_TRUE(cyl);

    SolidGuard result(oreo_boolean_subtract(filleted.get(), cyl.get()));
    ASSERT_TRUE(result) << "boolean_subtract failed";

    // 5. AABB sanity — should straddle the original prism bounds.
    OreoBBox bb = oreo_aabb(result.get());
    EXPECT_LT(bb.xmin, bb.xmax);
    EXPECT_LT(bb.ymin, bb.ymax);
    EXPECT_LT(bb.zmin, bb.zmax);
    EXPECT_GE(bb.xmax - bb.xmin, 49.0);   // rectangle width ≈ 50
    EXPECT_GE(bb.ymax - bb.ymin, 29.0);   // rectangle depth ≈ 30

    // 6. Mass properties: volume must be strictly less than the prism
    //    volume (fillet shaves material, hole removes more) and strictly
    //    greater than zero. The 50x30x20 prism = 30_000; the hole
    //    removes up to pi*5^2*20 ≈ 1570; fillet removes < 10.
    OreoMassProps mp = oreo_mass_properties(result.get());
    EXPECT_GT(mp.volume, 25000.0);
    EXPECT_LT(mp.volume, 30000.0);
    EXPECT_GT(mp.surface_area, 0.0);
}

// ═══════════════════════════════════════════════════════════════════
// W2: STEP round-trip preserves geometry
// ═══════════════════════════════════════════════════════════════════
//
// Touches: geometry, STEP I/O (export + re-import), query.

TEST(Integration, W2_StepRoundTripPreservesGeometry) {
    oreo_init();

    // 1. Build: box -> fillet -> subtract cylinder.
    SolidGuard box(oreo_make_box(40.0, 40.0, 20.0));
    ASSERT_TRUE(box);

    EdgeGuard edge0(oreo_get_edge(box.get(), 1));
    ASSERT_NE(edge0.get(), nullptr);
    OreoEdge edgeArr[1] = {edge0.get()};
    SolidGuard filleted(oreo_fillet(box.get(), edgeArr, 1, 3.0));
    ASSERT_TRUE(filleted);

    SolidGuard cyl(oreo_make_cylinder(4.0, 30.0));
    ASSERT_TRUE(cyl);
    SolidGuard original(oreo_boolean_subtract(filleted.get(), cyl.get()));
    ASSERT_TRUE(original);

    int origFaces = oreo_face_count(original.get());
    OreoBBox origBB = oreo_aabb(original.get());
    OreoMassProps origMP = oreo_mass_properties(original.get());

    // 2. Export to temp STEP.
    auto tmpPath = std::filesystem::temp_directory_path()
                 / "oreo_integration_W2.step";
    std::string tmpPathStr = tmpPath.string();

    OreoSolid exportArr[1] = {original.get()};
    int exportOk = oreo_export_step_file(exportArr, 1, tmpPathStr.c_str());
    ASSERT_EQ(exportOk, 1) << "STEP export failed";
    ASSERT_TRUE(std::filesystem::exists(tmpPath));

    // 3. Import in a fresh process-local context. The C API uses the
    //    internal default context, which is shared — the "fresh" aspect
    //    is the file going through OCCT's STEP reader from disk.
    SolidGuard imported(oreo_import_step_file(tmpPathStr.c_str()));
    ASSERT_TRUE(imported) << "STEP import failed";

    // 4. Compare.
    int impFaces = oreo_face_count(imported.get());
    // Face count can drift by a few faces because STEP's AP214 can split
    // composite surfaces (e.g., a fillet can arrive as several periodic
    // surface patches). Require within a small absolute tolerance.
    EXPECT_LE(std::abs(impFaces - origFaces), 3);

    OreoBBox impBB = oreo_aabb(imported.get());
    EXPECT_NEAR(impBB.xmin, origBB.xmin, 1e-3);
    EXPECT_NEAR(impBB.ymin, origBB.ymin, 1e-3);
    EXPECT_NEAR(impBB.zmin, origBB.zmin, 1e-3);
    EXPECT_NEAR(impBB.xmax, origBB.xmax, 1e-3);
    EXPECT_NEAR(impBB.ymax, origBB.ymax, 1e-3);
    EXPECT_NEAR(impBB.zmax, origBB.zmax, 1e-3);

    OreoMassProps impMP = oreo_mass_properties(imported.get());
    // STEP preserves volume to much better than 1%.
    EXPECT_NEAR(impMP.volume, origMP.volume, std::abs(origMP.volume) * 0.01);

    // 5. Clean up temp file.
    std::error_code ec;
    std::filesystem::remove(tmpPath, ec);
    // Not fatal if cleanup fails (Windows sometimes holds file handles);
    // just verify we at least tried. The OS will reap on exit.
}

// ═══════════════════════════════════════════════════════════════════
// W3: Serialize round-trip preserves topology + tags
// ═══════════════════════════════════════════════════════════════════
//
// Touches: geometry, serialize, naming.

TEST(Integration, W3_SerializeRoundTripPreservesTopologyAndNames) {
    auto ctx = oreo::KernelContext::create();

    // Build a named shape via the C++ API so we retain element names.
    auto boxR = oreo::makeBox(*ctx, 15.0, 25.0, 10.0);
    ASSERT_TRUE(boxR.ok());
    auto box = boxR.value();

    // Add a fillet so we have non-trivial element history and names.
    auto edgesR = oreo::getEdges(*ctx, box);
    ASSERT_TRUE(edgesR.ok());
    ASSERT_GE(edgesR.value().size(), 1u);
    auto filletedR = oreo::fillet(*ctx, box, {edgesR.value()[0]}, 1.5);
    ASSERT_TRUE(filletedR.ok());
    auto filleted = filletedR.value();

    int origFaces = filleted.countSubShapes(TopAbs_FACE);
    int origEdges = filleted.countSubShapes(TopAbs_EDGE);
    oreo::ShapeIdentity origId = filleted.shapeId();

    // Collect the original face names so we can compare after round-trip.
    std::vector<std::string> origFaceNames;
    origFaceNames.reserve(origFaces);
    for (int i = 1; i <= origFaces; ++i) {
        auto mn = filleted.getElementName(oreo::IndexedName("Face", i));
        origFaceNames.emplace_back(mn.data());
    }

    // Serialize.
    auto serR = oreo::serialize(*ctx, filleted);
    ASSERT_TRUE(serR.ok());
    auto buffer = serR.value();
    ASSERT_FALSE(buffer.empty());

    // Deserialize into a fresh context.
    auto ctx2 = oreo::KernelContext::create();
    auto deserR = oreo::deserialize(*ctx2, buffer.data(), buffer.size());
    ASSERT_TRUE(deserR.ok());
    auto restored = deserR.value();
    ASSERT_FALSE(restored.isNull());

    // Topology must match exactly.
    EXPECT_EQ(restored.countSubShapes(TopAbs_FACE), origFaces);
    EXPECT_EQ(restored.countSubShapes(TopAbs_EDGE), origEdges);

    // Shape identity round-trips through the serializer payload.
    EXPECT_EQ(restored.shapeId(), origId);

    // Element names — check each face has a non-empty mapped name.
    // We also require that, where both sides have non-empty names, they
    // match. (Some import paths produce "Nimport"-suffixed names;
    // serialize preserves the original names, so equality should hold.)
    int matched = 0;
    for (int i = 1; i <= origFaces; ++i) {
        auto mn = restored.getElementName(oreo::IndexedName("Face", i));
        std::string restoredName(mn.data());
        EXPECT_FALSE(restoredName.empty())
            << "Face " << i << " has no mapped name after deserialize";
        if (!origFaceNames[i - 1].empty() && !restoredName.empty()) {
            if (origFaceNames[i - 1] == restoredName) ++matched;
        }
    }
    EXPECT_GT(matched, 0) << "No face names survived the round trip";
}

// ═══════════════════════════════════════════════════════════════════
// W4: Feature tree parameter edit round-trip
// ═══════════════════════════════════════════════════════════════════
//
// Touches: feature tree, geometry replay, query (mass properties).

TEST(Integration, W4_FeatureTreeParameterEditRoundTrip) {
    auto ctx = oreo::KernelContext::create();
    oreo::FeatureTree tree(ctx);

    // Feature F1: MakeBox(10, 10, 10).
    oreo::Feature fBox;
    fBox.id = "F1";
    fBox.type = "MakeBox";
    fBox.params["dimensions"] = gp_Vec(10.0, 10.0, 10.0);
    (void)tree.addFeature(fBox);

    // Feature F2: Fillet edge 0 at radius 1.0. We reference edge
    // "Edge1" — the IndexedName of the first box edge.
    oreo::Feature fFil;
    fFil.id = "F2";
    fFil.type = "Fillet";
    fFil.params["radius"] = 1.0;
    fFil.params["edges"] = std::vector<oreo::ElementRef>{
        {"F1", "Edge1", "Edge"}
    };
    (void)tree.addFeature(fFil);

    auto volumeOf = [&](const oreo::NamedShape& s) -> double {
        auto mp = oreo::massProperties(*ctx, s);
        return mp.ok() ? mp.value().volume : -1.0;
    };

    // A — replay with width = 10.
    auto shapeA = tree.replay();
    ASSERT_FALSE(shapeA.isNull()) << "replay A produced null shape";
    double volA = volumeOf(shapeA);
    EXPECT_GT(volA, 0.0);
    // 10^3 minus the tiny fillet slice — a little under 1000.
    EXPECT_NEAR(volA, 1000.0, 5.0);

    // B — change width to 20.
    tree.updateParameter("F1", "dimensions",
                         oreo::ParamValue(gp_Vec(20.0, 10.0, 10.0)));
    auto shapeB = tree.replay();
    ASSERT_FALSE(shapeB.isNull()) << "replay B produced null shape";
    double volB = volumeOf(shapeB);
    EXPECT_GT(volB, 0.0);
    // 20*10*10 = 2000 minus a tiny fillet slice.
    EXPECT_NEAR(volB, 2000.0, 10.0);

    // Volumes A and B must be clearly different.
    EXPECT_GT(std::abs(volA - volB), 500.0);

    // C — revert to original width.
    tree.updateParameter("F1", "dimensions",
                         oreo::ParamValue(gp_Vec(10.0, 10.0, 10.0)));
    auto shapeC = tree.replay();
    ASSERT_FALSE(shapeC.isNull()) << "replay C produced null shape";
    double volC = volumeOf(shapeC);

    // A and C should match (same parameters -> same geometry).
    EXPECT_NEAR(volA, volC, 0.01);
    EXPECT_GT(std::abs(volB - volA), std::abs(volA - volC));
}

// ═══════════════════════════════════════════════════════════════════
// W5: Multi-context isolation
// ═══════════════════════════════════════════════════════════════════
//
// Touches: kernel context, diagnostics, geometry.

TEST(Integration, W5_MultiContextIsolation) {
    CtxGuard ctx1;
    CtxGuard ctx2;
    ASSERT_NE(ctx1.get(), nullptr);
    ASSERT_NE(ctx2.get(), nullptr);

    // ctx1 — trigger a diagnostic via an invalid op. Negative box
    // dimensions fail validation (requirePositive) and record an
    // INVALID_INPUT error into ctx1's diagnostic collector. The
    // exact severity (error vs warning) is not what we're testing —
    // we're testing that the diagnostic lands in ctx1 and NOT ctx2.
    SolidGuard bad(oreo_ctx_make_box(ctx1.get(), -1.0, 10.0, 10.0));
    EXPECT_EQ(bad.get(), nullptr);

    EXPECT_TRUE(oreo_context_has_errors(ctx1.get()) != 0)
        << "ctx1 should have recorded the invalid-input diagnostic";
    EXPECT_GE(oreo_context_error_count(ctx1.get()), 1);

    // ctx2 — a clean operation.
    SolidGuard good(oreo_ctx_make_box(ctx2.get(), 10.0, 10.0, 10.0));
    ASSERT_TRUE(good) << "ctx2 clean op should succeed";

    // ctx2's diagnostic stream must NOT have inherited ctx1's error —
    // this is the whole point of per-context isolation.
    EXPECT_EQ(oreo_context_has_errors(ctx2.get()), 0)
        << "ctx2 must not see ctx1's errors (isolation)";
    EXPECT_EQ(oreo_context_error_count(ctx2.get()), 0);

    // ctx1's error is still there — we haven't done anything on ctx1
    // that could have cleared it.
    EXPECT_NE(oreo_context_has_errors(ctx1.get()), 0);
}

// ═══════════════════════════════════════════════════════════════════
// W6: Cancellation mid-operation
// ═══════════════════════════════════════════════════════════════════
//
// Touches: kernel context, cancellation, geometry.
//
// The kernel uses cooperative cancellation — workers poll the token
// at checkpoints. This test just verifies:
//   (a) setting a cancellation token and flipping it does not crash;
//   (b) checkCancellation() after cancel() observes it;
//   (c) a geometry op finishing after cancellation either returns
//       cleanly or records a CANCELLED diagnostic. EITHER outcome is
//       acceptable — the guarantee is NO CRASH.

TEST(Integration, W6_CancellationMidOperation) {
    auto ctx = oreo::KernelContext::create();
    auto token = std::make_shared<oreo::CancellationToken>();
    ctx->setCancellationToken(token);

    // Worker thread: run a boolean chain.
    std::atomic<bool> finished{false};
    std::atomic<bool> opOk{false};
    std::thread worker([&] {
        auto boxR = oreo::makeBox(*ctx, 20.0, 20.0, 20.0);
        auto sphR = oreo::makeSphere(*ctx, 12.0);
        if (boxR.ok() && sphR.ok()) {
            // Chain a few booleans. Any one of them may observe the
            // cancellation token and bail.
            auto r1 = oreo::booleanSubtract(*ctx, boxR.value(), sphR.value());
            if (r1.ok()) {
                auto cylR = oreo::makeCylinder(*ctx, 5.0, 30.0);
                if (cylR.ok()) {
                    auto r2 = oreo::booleanSubtract(*ctx, r1.value(), cylR.value());
                    opOk.store(r2.ok());
                }
            }
        }
        finished.store(true);
    });

    // Give the worker a moment to start, then cancel.
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    token->cancel();

    worker.join();
    EXPECT_TRUE(finished.load());

    // Either the operation completed cleanly (didn't hit a cancellation
    // checkpoint before finishing) OR the diagnostic collector caught
    // the cancellation. Use EXPECT so both outcomes are observed.
    bool cancelObserved = token->isCancelled();
    EXPECT_TRUE(cancelObserved);

    // ctx->checkCancellation() is the public entrypoint that surfaces
    // CANCELLED in the diagnostic stream.
    bool cancelled = ctx->checkCancellation();
    EXPECT_TRUE(cancelled);
    // After checkCancellation returns true, diag() must carry the error.
    EXPECT_TRUE(ctx->diag().hasErrors());

    // Whether the chained boolean succeeded or not — the point is we
    // didn't crash and we observed the cancellation.
    (void)opOk.load();
}

// ═══════════════════════════════════════════════════════════════════
// W7: Full production chain: sketch -> feature tree -> STEP
// ═══════════════════════════════════════════════════════════════════
//
// Touches: sketch, feature tree (JSON serialize), geometry, STEP I/O.
//
// The feature tree's built-in types don't include a Sketch feature
// (primitives only), so we:
//   (1) build a rectangle wire via the sketch C API,
//   (2) use the feature tree for a parametric box + fillet chain,
//   (3) serialize / deserialize the feature tree JSON and replay,
//   (4) export the result to STEP and re-import.
// This exercises every subsystem touched in a real "user saves their
// document, reopens it, exports it" round trip.

TEST(Integration, W7_SketchFeatureTreeStepFullChain) {
    oreo_init();

    // (1) Sketch smoke check: a rectangle must round-trip to a wire.
    WireGuard rect(buildRectWire(30.0, 20.0));
    ASSERT_NE(rect.get(), nullptr) << "sketch->wire failed in W7 warm-up";

    // (2) Parametric feature tree.
    auto ctx = oreo::KernelContext::create();
    oreo::FeatureTree tree(ctx);

    oreo::Feature fBox;
    fBox.id = "F1";
    fBox.type = "MakeBox";
    fBox.params["dimensions"] = gp_Vec(30.0, 20.0, 15.0);
    (void)tree.addFeature(fBox);

    oreo::Feature fFil;
    fFil.id = "F2";
    fFil.type = "Fillet";
    fFil.params["radius"] = 1.0;
    fFil.params["edges"] = std::vector<oreo::ElementRef>{
        {"F1", "Edge1", "Edge"}
    };
    (void)tree.addFeature(fFil);

    auto shapeFromTree = tree.replay();
    ASSERT_FALSE(shapeFromTree.isNull());
    int origFaces = shapeFromTree.countSubShapes(TopAbs_FACE);
    EXPECT_GE(origFaces, 6);  // box face count, maybe more after fillet

    // (3) Serialize the feature tree as JSON, re-parse, replay again.
    std::string json = tree.toJSON();
    ASSERT_FALSE(json.empty());
    EXPECT_NE(json.find("MakeBox"), std::string::npos);
    EXPECT_NE(json.find("Fillet"), std::string::npos);

    auto ctx2 = oreo::KernelContext::create();
    auto restoredR = oreo::FeatureTree::fromJSON(json);
    ASSERT_TRUE(restoredR.ok) << restoredR.error;
    auto& restoredTree = restoredR.tree;
    // Swap in the fresh context so replay runs in ctx2.
    oreo::FeatureTree replayTree(ctx2);
    for (const auto& f : restoredTree.features()) {
        (void)replayTree.addFeature(f);
    }
    auto shapeFromJson = replayTree.replay();
    ASSERT_FALSE(shapeFromJson.isNull());
    EXPECT_EQ(shapeFromJson.countSubShapes(TopAbs_FACE), origFaces);

    // (4) Export the replayed shape to STEP, re-import it.
    auto tmpPath = std::filesystem::temp_directory_path()
                 / "oreo_integration_W7.step";
    std::string tmpPathStr = tmpPath.string();

    auto expR = oreo::exportStepFile(*ctx2, {shapeFromJson}, tmpPathStr);
    ASSERT_TRUE(expR.ok());
    ASSERT_TRUE(expR.value());
    ASSERT_TRUE(std::filesystem::exists(tmpPath));

    auto ctx3 = oreo::KernelContext::create();
    auto impR = oreo::importStepFile(*ctx3, tmpPathStr);
    ASSERT_TRUE(impR.ok());
    auto impShape = impR.value().shape;
    ASSERT_FALSE(impShape.isNull());
    int impFaces = impShape.countSubShapes(TopAbs_FACE);
    EXPECT_GT(impFaces, 0);                            // non-trivial result
    EXPECT_LE(std::abs(impFaces - origFaces), 3);      // STEP tolerance

    std::error_code ec;
    std::filesystem::remove(tmpPath, ec);
}
