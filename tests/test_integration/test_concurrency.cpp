// SPDX-License-Identifier: LGPL-2.1-or-later

// test_concurrency.cpp — Phase D3 acceptance suite.
//
// The kernel's thread-safety contract (src/core/thread_safety.h):
//   * One KernelContext per thread, full stop.
//   * Distinct contexts running in parallel must NOT interact, even
//     when both touch global OCCT initialisation, run STEP I/O
//     concurrently, or stress the tag allocator.
//
// These tests spawn N worker threads, each driving its own ctx end-to-
// end. They assert: no exceptions escape, every worker produces the
// expected geometry, no diagnostics cross over to a sibling ctx.

#include <gtest/gtest.h>

#include "oreo_kernel.h"
#include "core/kernel_context.h"
#include "geometry/oreo_geometry.h"
#include "io/oreo_step.h"
#include "io/oreo_serialize.h"
#include "naming/named_shape.h"
#include "feature/feature.h"
#include "feature/feature_tree.h"

#include <BRepPrimAPI_MakeBox.hxx>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

namespace {
constexpr int kThreads = 8;
constexpr int kIterPerThread = 12;
} // anonymous namespace

// ════════════════════════════════════════════════════════════════
// Per-context isolation: tag streams do not collide
// ════════════════════════════════════════════════════════════════

TEST(Concurrency, DistinctContextsHaveDistinctTagSequences) {
    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([t, &failed]() {
            // Each thread gets a unique documentId so its tag stream
            // is namespaced — even if scheduling interleaves the
            // workers, no collision should occur.
            oreo::KernelConfig cfg;
            cfg.documentId = 0x10000ULL + static_cast<std::uint64_t>(t);
            auto ctx = oreo::KernelContext::create(cfg);
            std::vector<oreo::ShapeIdentity> ids;
            ids.reserve(kIterPerThread);
            for (int i = 0; i < kIterPerThread; ++i) {
                ids.push_back(ctx->tags().nextShapeIdentity());
            }
            for (auto& id : ids) {
                if (id.documentId != 0x10000ULL + static_cast<std::uint64_t>(t)) {
                    failed = true;
                    return;
                }
                if (!id.isValid()) {
                    failed = true;
                    return;
                }
            }
        });
    }
    for (auto& w : workers) w.join();
    EXPECT_FALSE(failed.load());
}

// ════════════════════════════════════════════════════════════════
// Geometry parallelism: N workers, each builds + serialises a box
// ════════════════════════════════════════════════════════════════

TEST(Concurrency, ParallelGeometryAndSerializeAcrossContexts) {
    std::vector<std::future<bool>> futures;
    futures.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        futures.push_back(std::async(std::launch::async, [t]() {
            try {
                oreo::KernelConfig cfg;
                cfg.documentId = 0x20000ULL + static_cast<std::uint64_t>(t);
                auto ctx = oreo::KernelContext::create(cfg);
                for (int i = 0; i < kIterPerThread; ++i) {
                    auto r = oreo::makeBox(*ctx, 1.0 + i, 2.0 + i, 3.0 + i);
                    if (!r.ok()) return false;
                    auto wr = oreo::serialize(*ctx, r.value());
                    if (!wr.ok()) return false;
                    auto rr = oreo::deserialize(*ctx, wr.value().data(), wr.value().size());
                    if (!rr.ok()) return false;
                    if (rr.value().shapeId().documentId != cfg.documentId) {
                        return false;
                    }
                }
                return true;
            } catch (...) {
                return false;
            }
        }));
    }
    for (auto& f : futures) {
        EXPECT_TRUE(f.get());
    }
}

// ════════════════════════════════════════════════════════════════
// Diagnostics do not bleed across contexts
// ════════════════════════════════════════════════════════════════

TEST(Concurrency, DiagnosticsAreContextLocal) {
    // ctxA does something that fails (zero-size box). ctxB does only
    // valid work. ctxB's diagnostic stream must remain empty.
    auto ctxA = oreo::KernelContext::create();
    auto ctxB = oreo::KernelContext::create();

    std::thread tA([&] {
        for (int i = 0; i < 50; ++i) {
            auto r = oreo::makeBox(*ctxA, -1, 5, 5);  // negative dx -> error
            (void)r;
        }
    });
    std::thread tB([&] {
        for (int i = 0; i < 50; ++i) {
            auto r = oreo::makeBox(*ctxB, 5, 5, 5);
            (void)r;
        }
    });
    tA.join();
    tB.join();

    EXPECT_TRUE(ctxA->diag().hasErrors());
    EXPECT_FALSE(ctxB->diag().hasErrors());
}

// ════════════════════════════════════════════════════════════════
// FeatureTree parallel replay across ctx
// ════════════════════════════════════════════════════════════════

TEST(Concurrency, ParallelFeatureTreeReplay) {
    std::vector<std::future<bool>> futures;
    for (int t = 0; t < kThreads; ++t) {
        futures.push_back(std::async(std::launch::async, [t]() {
            try {
                oreo::KernelConfig cfg;
                cfg.documentId = 0x30000ULL + static_cast<std::uint64_t>(t);
                auto ctx = oreo::KernelContext::create(cfg);
                oreo::FeatureTree tree(ctx);

                oreo::Feature box;
                box.id = "F1";
                box.type = "MakeBox";
                box.params["dimensions"] = gp_Vec(10.0 + t, 10.0, 10.0);
                (void)tree.addFeature(box);

                oreo::Feature off;
                off.id = "F2";
                off.type = "Offset";
                off.params["distance"] = 0.5;
                (void)tree.addFeature(off);

                auto result = tree.replay();
                return !result.isNull();
            } catch (...) {
                return false;
            }
        }));
    }
    for (auto& f : futures) EXPECT_TRUE(f.get());
}
