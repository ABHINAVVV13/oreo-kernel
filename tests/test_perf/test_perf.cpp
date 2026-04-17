// SPDX-License-Identifier: LGPL-2.1-or-later

// test_perf.cpp — Performance regression suite for oreo-kernel.
//
// Purpose: catch catastrophic regressions (10x+ slowdown), NOT microbenchmark
// noise. Budgets are intentionally generous so CI flakiness is minimized on
// slow machines. Each test prints its actual time so a human reviewer can spot
// trends across runs.
//
// Use EXPECT_LT on elapsed ms so that a single slow machine does not abort the
// entire suite — only the slow test fails.

#include <gtest/gtest.h>

#include "oreo_kernel.h"

#include "core/kernel_context.h"
#include "core/diagnostic.h"
#include "core/tag_allocator.h"
#include "core/oreo_error.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace {

// ═══════════════════════════════════════════════════════════════════════════
// OREO_BENCH — run `body`, print `[PERF] name: X ms`, and
// EXPECT_LT on the elapsed wall-clock milliseconds vs `budget_ms`.
// Uses steady_clock so wall-clock jumps do not perturb the measurement.
// ═══════════════════════════════════════════════════════════════════════════

#define OREO_BENCH(name, body, budget_ms)                                     \
    do {                                                                      \
        auto _oreo_bench_t0 = std::chrono::steady_clock::now();               \
        body                                                                  \
        auto _oreo_bench_t1 = std::chrono::steady_clock::now();               \
        long long _oreo_bench_ms =                                            \
            std::chrono::duration_cast<std::chrono::milliseconds>(            \
                _oreo_bench_t1 - _oreo_bench_t0).count();                     \
        std::fprintf(stdout, "[PERF] %s: %lld ms\n", (name), _oreo_bench_ms); \
        std::fflush(stdout);                                                  \
        EXPECT_LT(_oreo_bench_ms, (long long)(budget_ms))                     \
            << "Performance regression in " << (name)                         \
            << ": elapsed " << _oreo_bench_ms << " ms, budget "               \
            << (long long)(budget_ms) << " ms";                               \
    } while (0)

// One-time kernel init for the whole process. gtest runs tests in an
// undefined order within a binary, and the legacy C API requires oreo_init().
struct PerfSuiteInit {
    PerfSuiteInit() {
        oreo::KernelContext::initOCCT();
#ifdef OREO_ENABLE_LEGACY_API
        oreo_init();
#endif
    }
};
static PerfSuiteInit _perf_suite_init;

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════
// P1 — Boolean chain: 50 subtractions
// Catches booleans going quadratic/exponential or OCCT churning on retry.
// Budget 10 s — each subtract on a ~100 mm box with simple tool runs in
// well under 200 ms on Release MSVC; 10 s gives us a ~3x safety margin even
// on slow CI hardware.
// ═══════════════════════════════════════════════════════════════════════════

TEST(Perf, BooleanChain50Subtractions) {
    OREO_BENCH("boolean_chain_50",
    {
        OreoSolid base = oreo_make_box(100.0, 100.0, 100.0);
        ASSERT_NE(base, nullptr);

        // Deterministic RNG — seed=42 so the test is reproducible.
        std::mt19937 rng(42);
        std::uniform_real_distribution<double> posDist(5.0, 90.0);
        std::uniform_real_distribution<double> sizeDist(1.0, 5.0);

        for (int i = 0; i < 50; ++i) {
            double sx = sizeDist(rng);
            double sy = sizeDist(rng);
            double sz = sizeDist(rng);
            OreoSolid tool = oreo_make_box(sx, sy, sz);
            ASSERT_NE(tool, nullptr) << "tool " << i;

            // Random-but-deterministic position — move the tool around the
            // base by translating it via a fresh boolean against a placed box.
            // Since the C API doesn't expose a transform, we approximate
            // position variety by creating boxes at random sizes. The actual
            // boolean shape here is not important — we're measuring the
            // boolean op dispatch + OCCT compute cost, not geometric variety.
            OreoSolid next = oreo_boolean_subtract(base, tool);
            oreo_free_solid(tool);

            if (next) {
                oreo_free_solid(base);
                base = next;
            }
            // If boolean failed (can happen with degenerate overlap), keep
            // base as-is; we still burned the dispatch cost which is what
            // we're measuring.

            // Consume the unused random to keep RNG sequence deterministic.
            (void)posDist(rng);
            (void)posDist(rng);
            (void)posDist(rng);
        }

        oreo_free_solid(base);
    }, 10000);
}

// ═══════════════════════════════════════════════════════════════════════════
// P2 — Primitive creation: 10000 boxes
// Catches regressions in tag allocation, diagnostic scope setup, element-map
// construction, or OCCT prim allocation. A 1x1x1 box is the cheapest possible
// primitive — the cost should be dominated by per-op overhead.
// Budget 5 s — ~500 µs per box headroom, ample for a 10x slowdown warning.
// ═══════════════════════════════════════════════════════════════════════════

TEST(Perf, PrimitiveCreation10000Boxes) {
    OREO_BENCH("primitive_creation_10000",
    {
        for (int i = 0; i < 10000; ++i) {
            OreoSolid s = oreo_make_box(1.0, 1.0, 1.0);
            ASSERT_NE(s, nullptr) << "iteration " << i;
            oreo_free_solid(s);
        }
    }, 5000);
}

// ═══════════════════════════════════════════════════════════════════════════
// P3 — Mesh tessellation: complex solid
// Box + 10 cylinders subtracted + a filleted edge. Exercises the full
// BRepMesh_IncrementalMesh pipeline plus per-face extraction and normal
// computation.
// Budget 5 s — generous; a 100 mm solid at 0.01 mm deflection should mesh
// in well under 1 s on Release MSVC.
// ═══════════════════════════════════════════════════════════════════════════

TEST(Perf, MeshTessellationComplex) {
    // Build the solid OUTSIDE the benchmarked region — we only want to time
    // tessellation, not geometry construction. (Geometry construction has
    // its own coverage above.)
    OreoSolid base = oreo_make_box(100.0, 100.0, 100.0);
    ASSERT_NE(base, nullptr);

    // Subtract 10 cylinders at varying positions. Without a transform API
    // we use varying cylinder dimensions to generate geometric variety.
    for (int i = 0; i < 10; ++i) {
        double r = 2.0 + (i % 5) * 0.5;
        double h = 120.0;
        OreoSolid cyl = oreo_make_cylinder(r, h);
        if (!cyl) continue;
        OreoSolid next = oreo_boolean_subtract(base, cyl);
        oreo_free_solid(cyl);
        if (next) {
            oreo_free_solid(base);
            base = next;
        }
    }

    // Fillet the first few edges if any exist.
    int nedges = oreo_edge_count(base);
    if (nedges > 0) {
        int filletCount = std::min(nedges, 4);
        std::vector<OreoEdge> edges;
        edges.reserve(filletCount);
        for (int i = 1; i <= filletCount; ++i) {
            OreoEdge e = oreo_get_edge(base, i);
            if (e) edges.push_back(e);
        }
        if (!edges.empty()) {
            OreoSolid filleted = oreo_fillet(base, edges.data(),
                                             (int)edges.size(), 0.5);
            for (auto e : edges) oreo_free_edge(e);
            if (filleted) {
                oreo_free_solid(base);
                base = filleted;
            }
        }
    }

    OREO_BENCH("mesh_tessellation_complex",
    {
        OreoMesh mesh = oreo_tessellate(base, 0.01, 20.0);
        ASSERT_NE(mesh, nullptr);
        // Touch the mesh briefly to force any lazy data generation.
        (void)oreo_mesh_vertex_count(mesh);
        (void)oreo_mesh_triangle_count(mesh);
        oreo_mesh_free(mesh);
    }, 5000);

    oreo_free_solid(base);
}

// ═══════════════════════════════════════════════════════════════════════════
// P4 — Sketch solve: 100 constraints
// 50 points, 50 lines, 100 constraints (coincident / distance / angle mix).
// Exercises PlaneGCS solver setup and Dogleg iterations.
// Budget 2 s — a 100-constraint sketch at this size should solve in
// well under 200 ms. 10x budget catches regressions to O(n^3) solver paths.
// ═══════════════════════════════════════════════════════════════════════════

TEST(Perf, SketchSolve100Constraints) {
    OreoSketch sk = oreo_sketch_create();
    ASSERT_NE(sk, nullptr);

    // 50 points scattered on a grid (deterministic).
    std::vector<int> pointIdx;
    pointIdx.reserve(50);
    for (int i = 0; i < 50; ++i) {
        double x = (i % 10) * 10.0 + 0.1 * i;
        double y = (i / 10) * 10.0 + 0.2 * i;
        pointIdx.push_back(oreo_sketch_add_point(sk, x, y));
    }

    // 50 lines between successive grid points (wrap at end).
    std::vector<int> lineIdx;
    lineIdx.reserve(50);
    for (int i = 0; i < 50; ++i) {
        double x1 = (i % 10) * 10.0;
        double y1 = (i / 10) * 10.0;
        double x2 = x1 + 5.0;
        double y2 = y1 + 5.0;
        lineIdx.push_back(oreo_sketch_add_line(sk, x1, y1, x2, y2));
    }

    // 100 constraints — mix of Coincident (cheap), Distance (medium),
    // and Angle (medium). Keep each constraint independent so we don't
    // hit conflicting/redundant paths.
    //
    // ConstraintType enum values (see sketch/oreo_sketch.h):
    //   Coincident = 0, ..., Distance = 6, ..., Angle = 9
    constexpr int kCoincident = 0;
    constexpr int kDistance   = 6;
    constexpr int kAngle      = 9;

    int added = 0;

    // 40 Coincident constraints between adjacent points — these are
    // redundant in bulk but the solver handles them as a simple kernel
    // of equalities; ideal for stress-testing scalar merging without
    // producing an unsolvable over-constrained system.
    for (int i = 0; i < 40 && added < 100; ++i) {
        int p1 = pointIdx[i];
        int p2 = pointIdx[(i + 1) % pointIdx.size()];
        oreo_sketch_add_constraint(sk, kCoincident, p1, p2, -1, 0.0);
        ++added;
    }

    // 40 Distance constraints on lines (using two distinct points).
    for (int i = 0; i < 40 && added < 100; ++i) {
        int p1 = pointIdx[i % pointIdx.size()];
        int p2 = pointIdx[(i + 7) % pointIdx.size()];
        double d = 3.0 + 0.1 * i;
        oreo_sketch_add_constraint(sk, kDistance, p1, p2, -1, d);
        ++added;
    }

    // 20 Angle constraints between pairs of lines.
    for (int i = 0; i < 20 && added < 100; ++i) {
        int l1 = lineIdx[i % lineIdx.size()];
        int l2 = lineIdx[(i + 3) % lineIdx.size()];
        double ang = 0.5 + 0.05 * i;  // radians; each pair independent
        oreo_sketch_add_constraint(sk, kAngle, l1, l2, -1, ang);
        ++added;
    }

    OREO_BENCH("sketch_solve_100",
    {
        // The solver may return redundant/conflicting for our random mix —
        // that's fine, we care about wall time, not solve quality.
        int status = oreo_sketch_solve(sk);
        (void)status;
    }, 2000);

    oreo_sketch_free(sk);
}

// ═══════════════════════════════════════════════════════════════════════════
// P5 — Serialize round-trip: complex solid, 100 iterations
// Catches regressions in the BinTools BREP write/read pipeline, the
// NamedShape wrapping, and element-map serialization.
// Budget 10 s — ~100 ms per round-trip headroom.
// ═══════════════════════════════════════════════════════════════════════════

TEST(Perf, SerializeRoundTrip100) {
    // Build a similar (but simpler) solid to P3 — we want a real shape
    // with non-trivial element map, not a single box.
    OreoSolid base = oreo_make_box(50.0, 50.0, 50.0);
    ASSERT_NE(base, nullptr);

    for (int i = 0; i < 3; ++i) {
        OreoSolid cyl = oreo_make_cylinder(2.0 + i * 0.5, 80.0);
        if (!cyl) continue;
        OreoSolid next = oreo_boolean_subtract(base, cyl);
        oreo_free_solid(cyl);
        if (next) {
            oreo_free_solid(base);
            base = next;
        }
    }

    OREO_BENCH("serialize_roundtrip_100",
    {
        for (int i = 0; i < 100; ++i) {
            size_t buflen = 0;
            uint8_t* buf = oreo_serialize(base, &buflen);
            ASSERT_NE(buf, nullptr) << "serialize iteration " << i;
            ASSERT_GT(buflen, 0u);

            OreoSolid round = oreo_deserialize(buf, buflen);
            ASSERT_NE(round, nullptr) << "deserialize iteration " << i;

            oreo_free_buffer(buf);
            oreo_free_solid(round);
        }
    }, 10000);

    oreo_free_solid(base);
}

// ═══════════════════════════════════════════════════════════════════════════
// P6 — Context creation: 1000 contexts
// Stresses shared_ptr<KernelContext>::create(), config validation,
// OCCT-safe init, schema registry default population, and destructor paths.
// Budget 2 s — ~2 ms per context headroom.
// ═══════════════════════════════════════════════════════════════════════════

TEST(Perf, ContextCreation1000) {
    OREO_BENCH("context_creation_1000",
    {
        for (int i = 0; i < 1000; ++i) {
            auto ctx = oreo::KernelContext::create();
            ASSERT_NE(ctx, nullptr) << "iteration " << i;
            // Touch the tag allocator so the context is not optimized away.
            (void)ctx->tags().peek();
        }
    }, 2000);
}

// ═══════════════════════════════════════════════════════════════════════════
// P7 — Tag allocation: 1M tags
// Stresses the atomic counter fast path. Should be ~1 ns per tag on modern
// hardware; 1M tags in 500 ms gives a 500 ns per-tag budget — 500x margin.
// Catches regressions to any tag path that adds locks or heap allocations.
// ═══════════════════════════════════════════════════════════════════════════

TEST(Perf, TagAllocation1M) {
    oreo::TagAllocator alloc;
    volatile int64_t sink = 0;  // prevents optimizer from discarding nextTag()

    OREO_BENCH("tag_allocation_1M",
    {
        for (int i = 0; i < 1000000; ++i) {
            sink = alloc.nextTag();
        }
    }, 500);

    (void)sink;
    EXPECT_EQ(alloc.currentValue(), 1000000);
}

// ═══════════════════════════════════════════════════════════════════════════
// P8 — Diagnostic report: 10k warnings
// Context with default quotas (maxDiagnostics = 10000). Reports exactly 10k
// warnings — right up to the cap. Stresses stamp metadata + counter updates.
// Budget 1 s — ~100 µs per diagnostic headroom; in practice each report is
// a few microseconds.
// ═══════════════════════════════════════════════════════════════════════════

TEST(Perf, DiagnosticReport10kWarnings) {
    auto ctx = oreo::KernelContext::create();
    ASSERT_NE(ctx, nullptr);

    OREO_BENCH("diagnostic_report_10k",
    {
        for (int i = 0; i < 10000; ++i) {
            ctx->diag().warning(
                oreo::ErrorCode::SHAPE_INVALID,
                "Perf test warning " + std::to_string(i));
        }
    }, 1000);

    // Sanity: the default cap is 10000, so we should have exactly 10000
    // warnings recorded (no truncation marker) or, if the cap clipped us
    // at 9999 + 1 marker, at least 10000 entries total.
    EXPECT_GE(ctx->diag().count(), 10000);
}
