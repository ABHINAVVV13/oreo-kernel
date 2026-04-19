// SPDX-License-Identifier: LGPL-2.1-or-later

// test_fuzz.cpp — Property-based "smoke fuzz" harness for oreo-kernel.
//
// True coverage-guided fuzzing (Clang + libFuzzer) is not available under
// this MSVC build. Instead, we exercise the kernel's entry points with
// random / malformed / truncated / bit-flipped inputs and assert that the
// kernel fails GRACEFULLY — every invocation must return a failure result
// (or a valid one) and leave a structured diagnostic behind. Nothing is
// allowed to abort the process, escape an unknown exception, or produce
// undefined behavior.
//
// All RNGs use fixed seeds so any failure is reproducible.
//
// NOTE on APIs used:
//   - Deserializer tests use the C++ API oreo::deserialize(ctx, data, len).
//   - Schema tests use oreo::SchemaRegistry::migrate and SchemaVersion::parse
//     directly — these are where malformed JSON / strings enter the system.
//   - STEP import tests use oreo::importStep. The public header does NOT
//     expose an oreo_import_step_from_text function; oreo_import_step /
//     oreo::importStep is the correct entry point for in-memory text data.
//   - Sketch fuzz (F6) uses the C API oreo_sketch_* as requested.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/kernel_context.h"
#include "core/diagnostic.h"
#include "core/diagnostic.h"
#include "core/schema.h"
#include "io/oreo_serialize.h"
#include "io/oreo_step.h"
#include "naming/named_shape.h"
#include "sketch/oreo_sketch.h"

#include "oreo_kernel.h"

#include <BRepPrimAPI_MakeBox.hxx>

namespace {

// Number of iterations for each fuzz test. Kept modest so the suite
// finishes in seconds on CI while still producing useful coverage.
constexpr int kF1_Iterations = 500;    // Deserializer random bytes
constexpr int kF2_Step       = 16;     // Truncation step (bytes)
constexpr int kF3_Iterations = 200;    // Bit-flip iterations
constexpr int kF4_Iterations = 200;    // Schema malformed JSON
constexpr int kF5_Iterations = 500;    // SchemaVersion::parse
constexpr int kF6_Iterations = 50;     // Sketch random constraint graph
constexpr int kF7_Iterations = 100;    // STEP malformed text
constexpr int kF8_BucketPct  = 10;     // STEP truncation step (10% of len)

// ── Helpers ──────────────────────────────────────────────────────

// Build a deterministic, valid serialized solid. Used as seed input for
// F2 (truncation) and F3 (bit-flip).
std::vector<uint8_t> makeValidSerializedBox(oreo::KernelContext& ctx,
                                             double dx, double dy, double dz) {
    TopoDS_Shape shape = BRepPrimAPI_MakeBox(dx, dy, dz).Shape();
    oreo::NamedShape ns(shape, ctx.tags().nextShapeIdentity());
    auto r = oreo::serialize(ctx, ns);
    if (!r.ok()) return {};
    return r.value();
}

// Build a random JSON value up to the given remaining depth. Mixes
// valid and wildly-nested structures, plus junk strings, numbers,
// arrays and objects. Never recurses past `depth` to bound runtime.
nlohmann::json randomJson(std::mt19937& rng, int depth) {
    std::uniform_int_distribution<int> kindDist(0, 7);
    int kind = kindDist(rng);
    if (depth <= 0) kind = kind % 4;  // Force a leaf at depth 0

    switch (kind) {
    case 0: return nullptr;
    case 1: return static_cast<bool>(rng() & 1);
    case 2: {
        std::uniform_int_distribution<int> d(-1000000, 1000000);
        return d(rng);
    }
    case 3: {
        // Random short ASCII string (may contain junk bytes)
        std::uniform_int_distribution<int> lenDist(0, 24);
        std::uniform_int_distribution<int> chDist(0x20, 0x7E);
        int n = lenDist(rng);
        std::string s;
        s.reserve(n);
        for (int i = 0; i < n; ++i) s.push_back(static_cast<char>(chDist(rng)));
        return s;
    }
    case 4: {
        // Array
        std::uniform_int_distribution<int> lenDist(0, 4);
        int n = lenDist(rng);
        nlohmann::json arr = nlohmann::json::array();
        for (int i = 0; i < n; ++i) arr.push_back(randomJson(rng, depth - 1));
        return arr;
    }
    case 5: {
        // Object
        std::uniform_int_distribution<int> lenDist(0, 4);
        int n = lenDist(rng);
        nlohmann::json obj = nlohmann::json::object();
        for (int i = 0; i < n; ++i) {
            std::string key = "k" + std::to_string(rng() % 1000);
            obj[key] = randomJson(rng, depth - 1);
        }
        return obj;
    }
    case 6: {
        // A "looks-like-a-header" object (more likely to reach migration
        // code paths that validate version fields).
        nlohmann::json obj = nlohmann::json::object();
        obj["_schema"] = randomJson(rng, 0);
        obj["_version"] = randomJson(rng, 1);
        obj["_kernelVersion"] = randomJson(rng, 0);
        std::uniform_int_distribution<int> lenDist(0, 3);
        int extra = lenDist(rng);
        for (int i = 0; i < extra; ++i) {
            std::string key = "extra" + std::to_string(i);
            obj[key] = randomJson(rng, depth - 1);
        }
        return obj;
    }
    default: {
        // Numeric with possible edge values
        std::uniform_int_distribution<int> which(0, 4);
        switch (which(rng)) {
            case 0: return 0;
            case 1: return -1;
            case 2: return 2147483647;
            case 3: return 0.0;
            default: return 1.5;
        }
    }
    }
}

// A short pool of candidate schema types — a mix of real and bogus
// strings, so migrate() will hit both "unknown type" and "header
// invalid" branches.
const char* const kSchemaTypes[] = {
    "widget",
    "oreo.feature_tree",
    "oreo.feature",
    "oreo.element_map",
    "oreo.named_shape",
    "oreo.tolerance",
    "oreo.kernel_context",
    "",
    "\x01\x02\x03",
    "...",
};

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════
// F1: Deserializer — random bytes
// ═══════════════════════════════════════════════════════════════

TEST(Fuzz, F1_DeserializeRandomBytes) {
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> lenDist(0, 4096);
    std::uniform_int_distribution<int> byteDist(0, 255);

    int crashCount = 0;

    for (int i = 0; i < kF1_Iterations; ++i) {
        SCOPED_TRACE("iter=" + std::to_string(i));

        int len = lenDist(rng);
        std::vector<uint8_t> buf(len);
        for (int j = 0; j < len; ++j) buf[j] = static_cast<uint8_t>(byteDist(rng));

        auto ctx = oreo::KernelContext::create();
        try {
            auto result = oreo::deserialize(*ctx, buf.data(), buf.size());
            // Contract: either returns ok with a valid shape, or fails and
            // the context has an error diagnostic.
            if (!result.ok()) {
                EXPECT_TRUE(ctx->diag().hasErrors())
                    << "Failed deserialize did not record an error diagnostic "
                       "(iter=" << i << ", len=" << len << ")";
            }
        } catch (...) {
            ++crashCount;
        }
    }

    EXPECT_EQ(crashCount, 0)
        << "F1: " << crashCount << " invocation(s) let an exception escape";
}

// ═══════════════════════════════════════════════════════════════
// F2: Deserializer — truncated valid data
// ═══════════════════════════════════════════════════════════════

TEST(Fuzz, F2_DeserializeTruncated) {
    auto seedCtx = oreo::KernelContext::create();
    auto valid = makeValidSerializedBox(*seedCtx, 10, 20, 30);
    ASSERT_FALSE(valid.empty()) << "Failed to build seed serialized buffer";

    const size_t half = valid.size() / 2;
    int crashCount = 0;
    int iteration = 0;

    for (size_t offset = 1; offset <= half; offset += kF2_Step) {
        SCOPED_TRACE("iter=" + std::to_string(iteration)
                     + " offset=" + std::to_string(offset)
                     + " total=" + std::to_string(valid.size()));
        ++iteration;

        auto ctx = oreo::KernelContext::create();
        try {
            auto result = oreo::deserialize(*ctx, valid.data(), offset);
            // Truncated data — the deserializer should fail and record an
            // error diagnostic. We don't enforce "must fail" because an
            // oddly-sized truncation could in theory happen to produce a
            // parseable stub; but if it fails, a diagnostic is mandatory.
            if (!result.ok()) {
                EXPECT_TRUE(ctx->diag().hasErrors())
                    << "Truncated-deserialize failure had no diagnostic "
                       "(offset=" << offset << ")";
            }
        } catch (...) {
            ++crashCount;
        }
    }

    EXPECT_EQ(crashCount, 0)
        << "F2: " << crashCount << " truncation offsets escaped an exception";
}

// ═══════════════════════════════════════════════════════════════
// F3: Deserializer — bit-flip
// ═══════════════════════════════════════════════════════════════
//
// Previously disabled: the first enabled run of this test (2026-04-17, MSVC Release)
// produced a genuine access violation (SEH 0xc0000005) at iteration 0 on
// a single-bit flip of a valid serialized box.
//
// This is a REAL deserializer robustness bug caught by the fuzzer — not a
// false positive. The deserializer currently assumes its input is well-
// formed and does not bounds-check all header/field reads, so a mutated
// length or type field dereferences into memory it shouldn't.
//
// TODO(serialize-robustness): audit src/io/oreo_serialize.cpp for
// unchecked length/offset reads; add explicit bounds checks at each
// fread-equivalent and return a diagnostic-bearing failure on overrun.
// The serializer now rejects mutated payloads before OCCT sees them.
//
// Run this test anyway (to reproduce the crash):
//   test_fuzz.exe --gtest_filter=Fuzz.F3_DeserializeBitFlip

TEST(Fuzz, F3_DeserializeBitFlip) {
    auto seedCtx = oreo::KernelContext::create();
    auto validSeed = makeValidSerializedBox(*seedCtx, 10, 20, 30);
    ASSERT_FALSE(validSeed.empty()) << "Failed to build seed serialized buffer";

    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> idxDist(0, validSeed.size() - 1);
    std::uniform_int_distribution<int> nFlipsDist(1, 3);
    std::uniform_int_distribution<int> bitDist(0, 7);

    int crashCount = 0;

    for (int i = 0; i < kF3_Iterations; ++i) {
        SCOPED_TRACE("iter=" + std::to_string(i));

        std::vector<uint8_t> buf = validSeed;  // fresh copy
        int flips = nFlipsDist(rng);
        for (int f = 0; f < flips; ++f) {
            size_t idx = idxDist(rng);
            int bit = bitDist(rng);
            buf[idx] ^= static_cast<uint8_t>(1u << bit);
        }

        auto ctx = oreo::KernelContext::create();
        try {
            auto result = oreo::deserialize(*ctx, buf.data(), buf.size());
            if (!result.ok()) {
                EXPECT_TRUE(ctx->diag().hasErrors())
                    << "Bit-flip deserialize failure had no diagnostic";
            }
            // A successful decode is acceptable too — some bit-flips land
            // in the tag/payload-length fields and happen to round-trip.
        } catch (...) {
            ++crashCount;
        }
    }

    EXPECT_EQ(crashCount, 0)
        << "F3: " << crashCount << " bit-flip iterations escaped an exception";
}

// ═══════════════════════════════════════════════════════════════
// F4: Schema migration — malformed JSON
// ═══════════════════════════════════════════════════════════════

TEST(Fuzz, F4_SchemaMigrateMalformedJson) {
    std::mt19937 rng(43);
    std::uniform_int_distribution<int> depthDist(0, 10);
    std::uniform_int_distribution<int> typeIdxDist(
        0, static_cast<int>(sizeof(kSchemaTypes) / sizeof(kSchemaTypes[0])) - 1);

    int crashCount = 0;
    int expectedExceptionCount = 0;

    oreo::SchemaRegistry registry;

    for (int i = 0; i < kF4_Iterations; ++i) {
        SCOPED_TRACE("iter=" + std::to_string(i));

        int depth = depthDist(rng);
        nlohmann::json blob = randomJson(rng, depth);
        const char* type = kSchemaTypes[typeIdxDist(rng)];

        try {
            // migrate() is expected to throw runtime_error/invalid_argument
            // on malformed input, or return a value on happy paths.
            (void)registry.migrate(type, blob);
        } catch (const std::runtime_error&) {
            // Expected: missing header, unknown type, version mismatch.
            ++expectedExceptionCount;
        } catch (const std::invalid_argument&) {
            // Expected: SchemaVersion::fromJSON rejects the _version field.
            ++expectedExceptionCount;
        } catch (const std::logic_error&) {
            // Expected: registry invariant violations.
            ++expectedExceptionCount;
        } catch (...) {
            // Anything else is a crash candidate.
            ++crashCount;
        }
    }

    EXPECT_EQ(crashCount, 0)
        << "F4: " << crashCount << " migrate() calls threw unknown exception types";
    // Sanity: at least some of the random inputs should have been rejected.
    EXPECT_GT(expectedExceptionCount, 0);
}

// ═══════════════════════════════════════════════════════════════
// F5: SchemaVersion::parse — random strings
// ═══════════════════════════════════════════════════════════════

TEST(Fuzz, F5_SchemaVersionParse) {
    std::mt19937 rng(44);
    std::uniform_int_distribution<int> lenDist(0, 32);
    // Printable ASCII + a few control chars so we hit the digit-validation
    // branches as well as the length limits.
    std::uniform_int_distribution<int> chDist(0x01, 0x7F);

    int crashCount = 0;
    int rejectedCount = 0;
    int acceptedCount = 0;

    for (int i = 0; i < kF5_Iterations; ++i) {
        SCOPED_TRACE("iter=" + std::to_string(i));

        int n = lenDist(rng);
        std::string s;
        s.reserve(n);
        for (int j = 0; j < n; ++j) s.push_back(static_cast<char>(chDist(rng)));

        try {
            (void)oreo::SchemaVersion::parse(s);
            ++acceptedCount;
        } catch (const std::invalid_argument&) {
            ++rejectedCount;
        } catch (...) {
            ++crashCount;
        }
    }

    EXPECT_EQ(crashCount, 0)
        << "F5: " << crashCount << " parse() calls threw unknown exception types";
    // At least one rejection must happen — random junk strings should not
    // all be valid "major.minor.patch".
    EXPECT_GT(rejectedCount, 0);
}

// ═══════════════════════════════════════════════════════════════
// F6: Sketch — random constraint graph (via oreo_sketch_* C API)
// ═══════════════════════════════════════════════════════════════

TEST(Fuzz, F6_SketchRandomConstraintGraph) {
#ifdef OREO_ENABLE_LEGACY_API
    // oreo_sketch_add_constraint accepts type in [0, Concentric]; mirror
    // that limit here so we exercise every valid enum value uniformly.
    // Concentric is the last enum member — index 33 under the current
    // ConstraintType layout. Using static_cast keeps us resilient to
    // future reordering.
    constexpr int kMaxConstraintType =
        static_cast<int>(oreo::ConstraintType::Concentric);

    std::mt19937 rng(45);
    std::uniform_int_distribution<int> nPointsDist(5, 15);
    std::uniform_int_distribution<int> nLinesDist(5, 20);
    std::uniform_int_distribution<int> nConstraintsDist(0, 10);
    std::uniform_real_distribution<double> coordDist(-100.0, 100.0);
    std::uniform_int_distribution<int> ctypeDist(0, kMaxConstraintType);
    std::uniform_real_distribution<double> valueDist(-50.0, 50.0);

    int crashCount = 0;

    for (int i = 0; i < kF6_Iterations; ++i) {
        SCOPED_TRACE("iter=" + std::to_string(i));

        OreoSketch sketch = oreo_sketch_create();
        ASSERT_NE(sketch, nullptr);

        try {
            int nPoints = nPointsDist(rng);
            int nLines = nLinesDist(rng);
            int nConstraints = nConstraintsDist(rng);

            // Points
            std::vector<int> pointIdx;
            pointIdx.reserve(nPoints);
            for (int p = 0; p < nPoints; ++p) {
                int idx = oreo_sketch_add_point(sketch, coordDist(rng), coordDist(rng));
                pointIdx.push_back(idx);
            }

            // Lines — endpoints use random coords (decoupled from points,
            // since the C API doesn't expose endpoint-by-index wiring).
            std::vector<int> lineIdx;
            lineIdx.reserve(nLines);
            for (int l = 0; l < nLines; ++l) {
                int idx = oreo_sketch_add_line(
                    sketch,
                    coordDist(rng), coordDist(rng),
                    coordDist(rng), coordDist(rng));
                lineIdx.push_back(idx);
            }

            // Constraints — choose type + up to three entity indices at
            // random from the available pool. This deliberately produces
            // mis-typed pairings (e.g. PointOnCircle with a line index).
            // The solver's per-constraint `if (entity < size)` guards
            // should skip the constraint or report conflicting, never
            // crash.
            std::uniform_int_distribution<int> pickDist(
                0, std::max(1, nPoints + nLines) - 1);
            for (int c = 0; c < nConstraints; ++c) {
                int type = ctypeDist(rng);
                int e1 = pickDist(rng);
                int e2 = (rng() & 1) ? pickDist(rng) : -1;
                int e3 = (rng() & 1) ? pickDist(rng) : -1;
                double val = valueDist(rng);
                (void)oreo_sketch_add_constraint(sketch, type, e1, e2, e3, val);
            }

            // Solve — expected OK/Redundant/Conflicting/SolveFailed. All
            // are acceptable; the only failure mode we reject is a crash.
            int status = oreo_sketch_solve(sketch);
            EXPECT_TRUE(status == OREO_OK
                     || status == OREO_SKETCH_REDUNDANT
                     || status == OREO_SKETCH_CONFLICTING
                     || status == OREO_SKETCH_SOLVE_FAILED
                     || status == OREO_INVALID_INPUT
                     || status == OREO_INTERNAL_ERROR)
                << "Unexpected sketch solve status " << status
                << " on iter=" << i;
        } catch (...) {
            ++crashCount;
        }

        oreo_sketch_free(sketch);
    }

    EXPECT_EQ(crashCount, 0)
        << "F6: " << crashCount << " sketch iterations escaped an exception";
#else
    GTEST_SKIP() << "F6 requires the legacy C API (OREO_ENABLE_LEGACY_API)";
#endif
}

// ═══════════════════════════════════════════════════════════════
// F7: STEP import — malformed text
// ═══════════════════════════════════════════════════════════════
//
// Note: the public C header exposes `oreo_import_step(const uint8_t*, size_t)`
// for memory-buffer import; there is no `oreo_import_step_from_text`. We
// use the C++ entry point `oreo::importStep` here, which is the one the
// C API wraps.

TEST(Fuzz, F7_StepImportMalformedText) {
    std::mt19937 rng(46);
    std::uniform_int_distribution<int> lenDist(100, 10000);
    // Full printable ASCII, including a dash of control bytes so we hit
    // the parser's character-class branches.
    std::uniform_int_distribution<int> chDist(0x09, 0x7E);

    int crashCount = 0;

    for (int i = 0; i < kF7_Iterations; ++i) {
        SCOPED_TRACE("iter=" + std::to_string(i));

        int n = lenDist(rng);
        std::vector<uint8_t> buf(n);
        for (int j = 0; j < n; ++j) buf[j] = static_cast<uint8_t>(chDist(rng));

        auto ctx = oreo::KernelContext::create();
        try {
            auto result = oreo::importStep(*ctx, buf.data(), buf.size());
            if (!result.ok()) {
                EXPECT_TRUE(ctx->diag().hasErrors())
                    << "STEP-import failure had no diagnostic";
            }
            // A "successful" import of random ASCII is vanishingly unlikely
            // but we do not assert against it — OCCT's STEP reader is
            // permissive and may accept degenerate stubs.
        } catch (...) {
            ++crashCount;
        }
    }

    EXPECT_EQ(crashCount, 0)
        << "F7: " << crashCount << " STEP imports escaped an exception";
}

// ═══════════════════════════════════════════════════════════════
// F8: STEP import — truncated valid payload
// ═══════════════════════════════════════════════════════════════

TEST(Fuzz, F8_StepImportTruncated) {
    // Build a real STEP payload to truncate.
    auto buildCtx = oreo::KernelContext::create();
    TopoDS_Shape shape = BRepPrimAPI_MakeBox(10, 20, 30).Shape();
    oreo::NamedShape box(shape, buildCtx->tags().nextShapeIdentity());

    auto stepR = oreo::exportStep(*buildCtx, {box});
    ASSERT_TRUE(stepR.ok()) << "Could not produce baseline STEP buffer";
    auto stepBytes = stepR.value();
    ASSERT_FALSE(stepBytes.empty());

    int crashCount = 0;
    int iteration = 0;

    // Truncate at every kF8_BucketPct (10%) boundary from 10% up to 100%.
    const size_t total = stepBytes.size();
    for (int pct = kF8_BucketPct; pct <= 100; pct += kF8_BucketPct) {
        size_t len = (total * static_cast<size_t>(pct)) / 100u;
        if (len == 0) len = 1;  // Always pass at least one byte
        SCOPED_TRACE("iter=" + std::to_string(iteration)
                     + " pct=" + std::to_string(pct)
                     + " len=" + std::to_string(len)
                     + "/" + std::to_string(total));
        ++iteration;

        auto ctx = oreo::KernelContext::create();
        try {
            auto result = oreo::importStep(*ctx, stepBytes.data(), len);
            if (!result.ok()) {
                EXPECT_TRUE(ctx->diag().hasErrors())
                    << "Truncated STEP import failure had no diagnostic "
                       "(pct=" << pct << ")";
            }
        } catch (...) {
            ++crashCount;
        }
    }

    EXPECT_EQ(crashCount, 0)
        << "F8: " << crashCount << " truncated STEP imports escaped an exception";
}
