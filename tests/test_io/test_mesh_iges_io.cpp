// SPDX-License-Identifier: LGPL-2.1-or-later

// test_mesh_iges_io.cpp — Round-trip coverage for the new interchange
// formats (STL + IGES + 3MF).
//
// STL and IGES are tested via round-trip: write, read, compare basic
// invariants (face count, AABB). 3MF is export-only so we verify the
// emitted file matches 3MF Core structure (ZIP local-file-header magic
// + the three required parts) without attempting a round trip.

#include <gtest/gtest.h>

#include "core/kernel_context.h"
#include "geometry/oreo_geometry.h"
#include "io/oreo_iges.h"
#include "io/oreo_mesh_io.h"
#include "query/oreo_query.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct ScopedTempFile {
    fs::path path;
    explicit ScopedTempFile(const std::string& ext) {
        // Collision-resistant naming so concurrent tests (e.g. the
        // ConcurrentThreeMfExports_NoRace stress test below) don't
        // stomp on each other's paths. Same idiom as src/io/temp_file.h.
        static std::atomic<std::uint64_t> counter{0};
        std::uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
        std::size_t tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
        auto name = "oreo_test_"
                  + std::to_string(n) + "_"
                  + std::to_string(static_cast<std::uint64_t>(tid))
                  + ext;
        path = fs::temp_directory_path() / name;
    }
    ~ScopedTempFile() {
        std::error_code ec;
        fs::remove(path, ec);
    }
    ScopedTempFile(const ScopedTempFile&) = delete;
    ScopedTempFile& operator=(const ScopedTempFile&) = delete;
};

std::vector<std::uint8_t> slurpFile(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary | std::ios::ate);
    if (!ifs) return {};
    auto sz = ifs.tellg();
    if (sz <= 0) return {};
    ifs.seekg(0);
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(sz));
    ifs.read(reinterpret_cast<char*>(buf.data()), sz);
    return buf;
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════
// STL
// ═══════════════════════════════════════════════════════════════

TEST(MeshIO, StlBinaryRoundTrip) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10.0, 20.0, 30.0);
    ASSERT_TRUE(box.ok());

    ScopedTempFile tmp(".stl");
    oreo::StlExportOptions opts;
    opts.format = oreo::StlFormat::Binary;
    auto exp = oreo::exportStlFile(*ctx, box.value(), tmp.path.string(), opts);
    ASSERT_TRUE(exp.ok());
    ASSERT_TRUE(exp.value());
    ASSERT_TRUE(fs::exists(tmp.path));
    auto bytes = slurpFile(tmp.path);
    ASSERT_FALSE(bytes.empty());

    // Binary STL: bytes 80..84 hold a little-endian triangle count; must be > 0.
    ASSERT_GE(bytes.size(), 84u);
    std::uint32_t triCount = static_cast<std::uint32_t>(bytes[80])
                            | (static_cast<std::uint32_t>(bytes[81]) << 8)
                            | (static_cast<std::uint32_t>(bytes[82]) << 16)
                            | (static_cast<std::uint32_t>(bytes[83]) << 24);
    EXPECT_GT(triCount, 0u);
    // A box tessellated at default deflection produces 12 triangles
    // (2 per face, 6 faces). Allow a generous range: [12, 100].
    EXPECT_GE(triCount, 12u);
    EXPECT_LE(triCount, 100u);

    // Import back. StlAPI_Reader builds a compound of faces; we get a
    // shape with the triangle count reflected in the face explorer.
    auto imp = oreo::importStlFile(*ctx, tmp.path.string());
    ASSERT_TRUE(imp.ok());
    EXPECT_GT(imp.value().countSubShapes(TopAbs_FACE), 0);
}

TEST(MeshIO, StlAsciiWritesHeader) {
    auto ctx = oreo::KernelContext::create();
    auto sphere = oreo::makeSphere(*ctx, 5.0);
    ASSERT_TRUE(sphere.ok());

    ScopedTempFile tmp(".stl");
    oreo::StlExportOptions opts;
    opts.format = oreo::StlFormat::Ascii;
    opts.linearDeflection = 1.0; // coarser for speed
    auto exp = oreo::exportStlFile(*ctx, sphere.value(), tmp.path.string(), opts);
    ASSERT_TRUE(exp.ok());
    ASSERT_TRUE(exp.value());

    auto bytes = slurpFile(tmp.path);
    ASSERT_GE(bytes.size(), 5u);
    // ASCII STL starts with "solid"
    EXPECT_EQ(0, std::memcmp(bytes.data(), "solid", 5));
}

TEST(MeshIO, StlNullShapeReports) {
    auto ctx = oreo::KernelContext::create();
    oreo::NamedShape empty;
    oreo::StlExportOptions opts;
    auto r = oreo::exportStlFile(*ctx, empty, "/tmp/should-not-be-written.stl", opts);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(ctx->diag().hasErrors());
}

// ═══════════════════════════════════════════════════════════════
// IGES
// ═══════════════════════════════════════════════════════════════

TEST(MeshIO, IgesBoxRoundTrip) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 15.0, 25.0, 5.0);
    ASSERT_TRUE(box.ok());
    const auto origAabb = oreo::aabb(*ctx, box.value());
    ASSERT_TRUE(origAabb.ok());

    ScopedTempFile tmp(".iges");
    auto exp = oreo::exportIgesFile(*ctx, {box.value()}, tmp.path.string());
    ASSERT_TRUE(exp.ok()) << "IGES export should succeed";
    ASSERT_TRUE(fs::exists(tmp.path));

    auto imp = oreo::importIgesFile(*ctx, tmp.path.string());
    ASSERT_TRUE(imp.ok()) << "IGES import should succeed";
    ASSERT_FALSE(imp.value().isNull());

    const auto roundAabb = oreo::aabb(*ctx, imp.value());
    ASSERT_TRUE(roundAabb.ok());

    auto o = origAabb.value();
    auto r = roundAabb.value();
    // IGES surface-based round-trip loses the BRep solid structure but
    // preserves the geometric envelope. Tolerate 0.05mm slop from
    // B-spline representation conversion.
    EXPECT_NEAR(o.xmax - o.xmin, r.xmax - r.xmin, 0.05);
    EXPECT_NEAR(o.ymax - o.ymin, r.ymax - r.ymin, 0.05);
    EXPECT_NEAR(o.zmax - o.zmin, r.zmax - r.zmin, 0.05);
}

// ═══════════════════════════════════════════════════════════════
// 3MF export
// ═══════════════════════════════════════════════════════════════

TEST(MeshIO, ThreeMfExportProducesValidZip) {
    auto ctx = oreo::KernelContext::create();
    auto cyl = oreo::makeCylinder(*ctx, 3.0, 10.0);
    ASSERT_TRUE(cyl.ok());

    auto r = oreo::exportThreeMf(*ctx, cyl.value());
    ASSERT_TRUE(r.ok());
    const auto& buf = r.value();

    // Minimum valid 3MF: at least three local-file-headers + central
    // directory + EOCD record. Realistic files are several KB.
    ASSERT_GE(buf.size(), 200u);

    // Local file header magic is "PK\x03\x04" (ZIP signature).
    EXPECT_EQ(buf[0], 0x50); // 'P'
    EXPECT_EQ(buf[1], 0x4B); // 'K'
    EXPECT_EQ(buf[2], 0x03);
    EXPECT_EQ(buf[3], 0x04);

    // The central directory must contain all three required 3MF parts.
    // Easy way to check: a byte search for the OPC part names.
    auto contains = [&](const char* needle) {
        const std::size_t nlen = std::strlen(needle);
        if (buf.size() < nlen) return false;
        for (std::size_t i = 0; i + nlen <= buf.size(); ++i) {
            if (std::memcmp(buf.data() + i, needle, nlen) == 0) return true;
        }
        return false;
    };
    EXPECT_TRUE(contains("[Content_Types].xml"));
    EXPECT_TRUE(contains("_rels/.rels"));
    EXPECT_TRUE(contains("3D/3dmodel.model"));

    // The model XML must include the 3MF Core namespace.
    EXPECT_TRUE(contains("http://schemas.microsoft.com/3dmanufacturing/core/2015/02"));

    // And at least one triangle element — otherwise slicers silently
    // accept but print nothing.
    EXPECT_TRUE(contains("<triangle "));
}

TEST(MeshIO, ThreeMfExportToFileRoundsThroughDisk) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 8.0, 8.0, 8.0);
    ASSERT_TRUE(box.ok());

    ScopedTempFile tmp(".3mf");
    oreo::ThreeMfExportOptions opts;
    opts.objectName = "TestCube";
    auto r = oreo::exportThreeMfFile(*ctx, box.value(), tmp.path.string(), opts);
    ASSERT_TRUE(r.ok());
    ASSERT_TRUE(r.value());
    ASSERT_TRUE(fs::exists(tmp.path));

    auto bytes = slurpFile(tmp.path);
    ASSERT_GE(bytes.size(), 200u);
    // ZIP magic again at byte 0.
    EXPECT_EQ(bytes[0], 0x50);
    EXPECT_EQ(bytes[1], 0x4B);
}

// ════════════════════════════════════════════════════════════════
// Concurrent-IO regression tests (landed 2026-04-18 audit).
// These exercise the two fixes simultaneously:
//   * CRC-32 table init race in 3MF export — now a function-local
//     magic-static. TSan flagged the old plain `static bool ready`.
//   * STL temp-file name collision — `::rand()` was predictable
//     enough that concurrent calls could alias into the same temp
//     path, causing silent data corruption. Shared TempFile helper
//     in src/io/temp_file.h now provides atomic-counter + thread-id
//     naming.
//
// The tests use N threads × M ops each; if the fix is undone, ASan
// / TSan flags the race or a random failure rate appears.
// ════════════════════════════════════════════════════════════════

// TSan-skip guard: OCCT's BRepMesh_IncrementalMesh runs with
// isInParallel=true during 3MF and STL export. Concurrent callers
// therefore hit OCCT's TBB-based parallel mesher, which emits TSan
// reports unrelated to the CRC / temp-file paths these tests are
// actually trying to cover. The project's ci/tsan.supp suppresses
// libtbb but not every OCCT internal. Skip under TSan — gcc-release
// and ASan+UBSan runs still exercise the real fixes and keep the
// regression gates active.
#if defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define OREO_SKIP_UNDER_TSAN 1
#  endif
#endif
#ifndef OREO_SKIP_UNDER_TSAN
#define OREO_SKIP_UNDER_TSAN 0
#endif

TEST(MeshIO, ConcurrentThreeMfExports_NoRace) {
    if (OREO_SKIP_UNDER_TSAN) {
        GTEST_SKIP() << "TSan noise from OCCT's parallel mesher masks "
                        "the CRC init race this test gates; other CI "
                        "cells (gcc-release + ASan+UBSan) cover it.";
    }
    // Each worker gets its OWN KernelContext (one-ctx-per-thread) but
    // they all share the process-global CRC32 table. This is exactly
    // the shape of a server handling N simultaneous 3MF export
    // requests — which pre-fix would race on the table init.
    constexpr int kThreads = 8;
    constexpr int kPerThread = 4;
    std::vector<std::thread> workers;
    std::atomic<int> successes{0};
    std::atomic<int> failures{0};

    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&]() {
            for (int i = 0; i < kPerThread; ++i) {
                auto ctx = oreo::KernelContext::create();
                auto box = oreo::makeBox(*ctx, 5.0, 5.0, 5.0);
                if (!box.ok()) { failures++; continue; }
                ScopedTempFile tmp(".3mf");
                oreo::ThreeMfExportOptions opts;
                auto r = oreo::exportThreeMfFile(*ctx, box.value(),
                                                  tmp.path.string(), opts);
                if (r.ok() && r.value() && fs::exists(tmp.path)) {
                    successes++;
                } else {
                    failures++;
                }
            }
        });
    }
    for (auto& w : workers) w.join();
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(successes.load(), kThreads * kPerThread);
}

TEST(MeshIO, ConcurrentStlBufferImportExport_NoTempFileCollision) {
    if (OREO_SKIP_UNDER_TSAN) {
        GTEST_SKIP() << "See ConcurrentThreeMfExports_NoRace — same OCCT "
                        "parallel-mesher noise obscures the temp-file race "
                        "fix this test gates. Other CI cells cover it.";
    }
    // Before the shared TempFile helper, the STL buffer paths named
    // temp files via `::rand()`. That's seeded once per process, so
    // two concurrent threads advancing through the same sequence
    // could emit the same path and one would silently clobber the
    // other's staged file. With the fix, the atomic counter +
    // thread-id hash guarantees uniqueness.
    //
    // We exercise exportStl (buffer form) and importStl (buffer form)
    // round-trip concurrently and assert every call succeeds.
    constexpr int kThreads = 8;
    constexpr int kPerThread = 4;
    std::vector<std::thread> workers;
    std::atomic<int> ok{0};
    std::atomic<int> bad{0};

    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t]() {
            for (int i = 0; i < kPerThread; ++i) {
                auto ctx = oreo::KernelContext::create();
                // Vary geometry so failures are not benign.
                auto box = oreo::makeBox(*ctx,
                                          2.0 + t * 0.1,
                                          2.0 + i * 0.1,
                                          3.0);
                if (!box.ok()) { bad++; continue; }

                oreo::StlExportOptions opts;
                auto buf = oreo::exportStl(*ctx, box.value(), opts);
                if (!buf.ok() || buf.value().empty()) { bad++; continue; }

                auto roundTrip = oreo::importStl(*ctx, buf.value().data(),
                                                  buf.value().size());
                if (roundTrip.ok() && !roundTrip.value().isNull()) {
                    ok++;
                } else {
                    bad++;
                }
            }
        });
    }
    for (auto& w : workers) w.join();
    EXPECT_EQ(bad.load(), 0);
    EXPECT_EQ(ok.load(), kThreads * kPerThread);
}
