// SPDX-License-Identifier: LGPL-2.1-or-later

// test_foundation_obs.cpp — Comprehensive tests for observability and
// config-loading modules:
//
//   - ConfigLoader   (src/core/config_loader.{h,cpp})
//   - IDiagnosticSink / DiagnosticCollector sink plumbing
//                    (src/core/logging_sink.h, src/core/diagnostic.h)
//   - ResourceQuotas integration with KernelContext
//                    (src/core/resource_quotas.h, src/core/kernel_context.cpp)
//   - DiagnosticCollector direct APIs — lastErrorOpt, snapshot,
//     maxDiagnostics, setContextId
//
// These tests mutate process-wide environment variables (for ConfigLoader
// env overlay coverage). Each env-touching test cleans up after itself to
// keep side-effects local and avoid influencing sibling tests.

#include <gtest/gtest.h>

#include "core/config_loader.h"
#include "core/diagnostic.h"
#include "core/kernel_context.h"
#include "core/logging_sink.h"
#include "core/resource_quotas.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#if defined(_WIN32)
#  include <stdlib.h>  // _putenv_s
#endif

// ═══════════════════════════════════════════════════════════════
// Cross-platform env helpers — static-internal to this TU.
// ═══════════════════════════════════════════════════════════════

namespace {

static void setEnv(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static void unsetEnv(const char* name) {
#if defined(_WIN32)
    // Passing empty string removes the variable on MSVC CRT.
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

// RAII scope: ensure we clear an env var on scope exit even if an
// assertion throws or aborts the current test.
struct EnvGuard {
    const char* name;
    explicit EnvGuard(const char* n) : name(n) { unsetEnv(name); }
    ~EnvGuard() { unsetEnv(name); }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;
};

// RAII scope: write a JSON file into the temp directory and delete it
// on scope exit, regardless of how the test exits.
struct TempJsonFile {
    std::filesystem::path path;
    TempJsonFile(const std::string& basename, const std::string& body) {
        path = std::filesystem::temp_directory_path() / basename;
        std::ofstream out(path);
        out << body;
    }
    ~TempJsonFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    TempJsonFile(const TempJsonFile&) = delete;
    TempJsonFile& operator=(const TempJsonFile&) = delete;
};

} // namespace

// ═══════════════════════════════════════════════════════════════
// ConfigLoader — low-level env helpers (unset → nullopt)
// ═══════════════════════════════════════════════════════════════

TEST(ConfigLoaderEnv, EnvStringReturnsNulloptWhenUnset) {
    EnvGuard g("OREO_OBS_TEST_STR_UNSET");
    auto v = oreo::ConfigLoader::envString("OREO_OBS_TEST_STR_UNSET");
    EXPECT_FALSE(v.has_value());
}

TEST(ConfigLoaderEnv, EnvDoubleReturnsNulloptWhenUnset) {
    EnvGuard g("OREO_OBS_TEST_DOUBLE_UNSET");
    auto v = oreo::ConfigLoader::envDouble("OREO_OBS_TEST_DOUBLE_UNSET");
    EXPECT_FALSE(v.has_value());
}

TEST(ConfigLoaderEnv, EnvIntReturnsNulloptWhenUnset) {
    EnvGuard g("OREO_OBS_TEST_INT_UNSET");
    auto v = oreo::ConfigLoader::envInt("OREO_OBS_TEST_INT_UNSET");
    EXPECT_FALSE(v.has_value());
}

TEST(ConfigLoaderEnv, EnvBoolReturnsNulloptWhenUnset) {
    EnvGuard g("OREO_OBS_TEST_BOOL_UNSET");
    auto v = oreo::ConfigLoader::envBool("OREO_OBS_TEST_BOOL_UNSET");
    EXPECT_FALSE(v.has_value());
}

TEST(ConfigLoaderEnv, EnvStringReturnsNulloptForNullOrEmptyName) {
    EXPECT_FALSE(oreo::ConfigLoader::envString(nullptr).has_value());
    EXPECT_FALSE(oreo::ConfigLoader::envString("").has_value());
}

// ───── Readback after set ─────────────────────────────────────

TEST(ConfigLoaderEnv, EnvStringReadsBackWhatWeSet) {
    EnvGuard g("OREO_OBS_TEST_STR");
    setEnv("OREO_OBS_TEST_STR", "hello world");
    auto v = oreo::ConfigLoader::envString("OREO_OBS_TEST_STR");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "hello world");
}

TEST(ConfigLoaderEnv, EnvDoubleReadsBackNumeric) {
    EnvGuard g("OREO_OBS_TEST_DOUBLE");
    setEnv("OREO_OBS_TEST_DOUBLE", "3.14159");
    auto v = oreo::ConfigLoader::envDouble("OREO_OBS_TEST_DOUBLE");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 3.14159);
}

TEST(ConfigLoaderEnv, EnvDoubleRejectsTrailingJunk) {
    EnvGuard g("OREO_OBS_TEST_DOUBLE_JUNK");
    setEnv("OREO_OBS_TEST_DOUBLE_JUNK", "1.5xyz");
    auto v = oreo::ConfigLoader::envDouble("OREO_OBS_TEST_DOUBLE_JUNK");
    EXPECT_FALSE(v.has_value());
}

TEST(ConfigLoaderEnv, EnvDoubleToleratesTrailingWhitespace) {
    EnvGuard g("OREO_OBS_TEST_DOUBLE_WS");
    setEnv("OREO_OBS_TEST_DOUBLE_WS", "2.5   ");
    auto v = oreo::ConfigLoader::envDouble("OREO_OBS_TEST_DOUBLE_WS");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 2.5);
}

TEST(ConfigLoaderEnv, EnvIntReadsBackSigned) {
    EnvGuard g("OREO_OBS_TEST_INT");
    setEnv("OREO_OBS_TEST_INT", "-12345");
    auto v = oreo::ConfigLoader::envInt("OREO_OBS_TEST_INT");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, int64_t{-12345});
}

TEST(ConfigLoaderEnv, EnvIntRejectsGarbage) {
    EnvGuard g("OREO_OBS_TEST_INT_BAD");
    setEnv("OREO_OBS_TEST_INT_BAD", "notanumber");
    auto v = oreo::ConfigLoader::envInt("OREO_OBS_TEST_INT_BAD");
    EXPECT_FALSE(v.has_value());
}

// ───── envBool accepts all documented truthy/falsey strings ───

TEST(ConfigLoaderEnv, EnvBoolAcceptsAllTruthyStrings) {
    EnvGuard g("OREO_OBS_TEST_BOOL");
    for (const char* truthy : {"1", "true", "TRUE", "True",
                               "yes", "YES", "on", "ON"}) {
        setEnv("OREO_OBS_TEST_BOOL", truthy);
        auto v = oreo::ConfigLoader::envBool("OREO_OBS_TEST_BOOL");
        ASSERT_TRUE(v.has_value()) << "truthy=" << truthy;
        EXPECT_TRUE(*v) << "truthy=" << truthy;
    }
}

TEST(ConfigLoaderEnv, EnvBoolAcceptsAllFalseyStrings) {
    EnvGuard g("OREO_OBS_TEST_BOOL");
    for (const char* falsey : {"0", "false", "FALSE", "False",
                               "no", "NO", "off", "OFF"}) {
        setEnv("OREO_OBS_TEST_BOOL", falsey);
        auto v = oreo::ConfigLoader::envBool("OREO_OBS_TEST_BOOL");
        ASSERT_TRUE(v.has_value()) << "falsey=" << falsey;
        EXPECT_FALSE(*v) << "falsey=" << falsey;
    }
}

TEST(ConfigLoaderEnv, EnvBoolRejectsUnknownValues) {
    EnvGuard g("OREO_OBS_TEST_BOOL_BAD");
    setEnv("OREO_OBS_TEST_BOOL_BAD", "maybe");
    auto v = oreo::ConfigLoader::envBool("OREO_OBS_TEST_BOOL_BAD");
    EXPECT_FALSE(v.has_value());
}

// ═══════════════════════════════════════════════════════════════
// ConfigLoader — applyEnvOverlay
// ═══════════════════════════════════════════════════════════════

TEST(ConfigLoaderEnv, ApplyEnvOverlayOverridesLinearPrecision) {
    EnvGuard g("OREO_LINEAR_PRECISION");
    setEnv("OREO_LINEAR_PRECISION", "1e-9");

    oreo::KernelConfig cfg{};
    const double before = cfg.tolerance.linearPrecision;
    ASSERT_NE(before, 1e-9);  // sanity: default is not what we're about to set

    oreo::ConfigLoader::applyEnvOverlay(cfg);
    EXPECT_DOUBLE_EQ(cfg.tolerance.linearPrecision, 1e-9);
}

TEST(ConfigLoaderEnv, ApplyEnvOverlayOverridesTagSeed) {
    EnvGuard g("OREO_TAG_SEED");
    setEnv("OREO_TAG_SEED", "4242");

    oreo::KernelConfig cfg{};
    cfg.tagSeed = 0;
    oreo::ConfigLoader::applyEnvOverlay(cfg);
    EXPECT_EQ(cfg.tagSeed, int64_t{4242});
}

TEST(ConfigLoaderEnv, ApplyEnvOverlayLeavesUntouchedFieldsAlone) {
    // None of the recognised env vars set → overlay must be a no-op.
    EnvGuard g1("OREO_LINEAR_PRECISION");
    EnvGuard g2("OREO_ANGULAR_PRECISION");
    EnvGuard g3("OREO_BOOLEAN_FUZZY_FACTOR");
    EnvGuard g4("OREO_MIN_EDGE_LENGTH");
    EnvGuard g5("OREO_TAG_SEED");
    EnvGuard g6("OREO_DOCUMENT_ID");
    EnvGuard g7("OREO_DOCUMENT_UUID");
    EnvGuard g8("OREO_INIT_OCCT");

    oreo::KernelConfig cfg{};
    const double origLinear = cfg.tolerance.linearPrecision;
    const double origAngular = cfg.tolerance.angularPrecision;
    const int64_t origSeed = cfg.tagSeed;

    oreo::ConfigLoader::applyEnvOverlay(cfg);

    EXPECT_DOUBLE_EQ(cfg.tolerance.linearPrecision, origLinear);
    EXPECT_DOUBLE_EQ(cfg.tolerance.angularPrecision, origAngular);
    EXPECT_EQ(cfg.tagSeed, origSeed);
}

TEST(ConfigLoaderEnv, ApplyEnvOverlayIgnoresMalformedNumbers) {
    EnvGuard g("OREO_LINEAR_PRECISION");
    setEnv("OREO_LINEAR_PRECISION", "not-a-number");

    oreo::KernelConfig cfg{};
    const double before = cfg.tolerance.linearPrecision;
    oreo::ConfigLoader::applyEnvOverlay(cfg);
    // Malformed value → best-effort overlay leaves the field alone.
    EXPECT_DOUBLE_EQ(cfg.tolerance.linearPrecision, before);
}

// ═══════════════════════════════════════════════════════════════
// ConfigLoader — applyJsonFile
// ═══════════════════════════════════════════════════════════════

TEST(ConfigLoaderJson, ApplyJsonFileOverridesNestedTolerance) {
    TempJsonFile f("oreo_obs_cfg_nested.json",
                   R"({ "tolerance": { "linearPrecision": 1e-9 } })");

    oreo::KernelConfig cfg{};
    ASSERT_NE(cfg.tolerance.linearPrecision, 1e-9);

    oreo::ConfigLoader::applyJsonFile(cfg, f.path.string());
    EXPECT_DOUBLE_EQ(cfg.tolerance.linearPrecision, 1e-9);
}

TEST(ConfigLoaderJson, ApplyJsonFileOverridesFlatKeys) {
    TempJsonFile f("oreo_obs_cfg_flat.json",
                   R"({ "linearPrecision": 5e-8, "tagSeed": 500 })");

    oreo::KernelConfig cfg{};
    oreo::ConfigLoader::applyJsonFile(cfg, f.path.string());
    EXPECT_DOUBLE_EQ(cfg.tolerance.linearPrecision, 5e-8);
    EXPECT_EQ(cfg.tagSeed, int64_t{500});
}

TEST(ConfigLoaderJson, ApplyJsonFileOnMissingPathThrowsAndDoesNotMutate) {
    const auto missing = std::filesystem::temp_directory_path() /
                         "oreo_obs_cfg_does_not_exist_xyz.json";
    // Ensure truly absent.
    {
        std::error_code ec;
        std::filesystem::remove(missing, ec);
    }

    oreo::KernelConfig cfg{};
    const double origLinear = cfg.tolerance.linearPrecision;
    const int64_t origSeed = cfg.tagSeed;

    EXPECT_THROW(oreo::ConfigLoader::applyJsonFile(cfg, missing.string()),
                 std::runtime_error);

    // No mutation on failure.
    EXPECT_DOUBLE_EQ(cfg.tolerance.linearPrecision, origLinear);
    EXPECT_EQ(cfg.tagSeed, origSeed);
}

TEST(ConfigLoaderJson, ApplyJsonFileRejectsNonObjectTopLevel) {
    TempJsonFile f("oreo_obs_cfg_array.json", "[1, 2, 3]");

    oreo::KernelConfig cfg{};
    EXPECT_THROW(oreo::ConfigLoader::applyJsonFile(cfg, f.path.string()),
                 std::runtime_error);
}

TEST(ConfigLoaderJson, ApplyJsonFileIgnoresUnknownKeysAndWrongTypes) {
    TempJsonFile f("oreo_obs_cfg_mixed.json",
                   R"({
                       "linearPrecision": "not a number",
                       "someUnknownKey": 42,
                       "tagSeed": 777
                   })");

    oreo::KernelConfig cfg{};
    const double origLinear = cfg.tolerance.linearPrecision;
    oreo::ConfigLoader::applyJsonFile(cfg, f.path.string());

    // Wrong-typed field → untouched.
    EXPECT_DOUBLE_EQ(cfg.tolerance.linearPrecision, origLinear);
    // Well-typed field → applied.
    EXPECT_EQ(cfg.tagSeed, int64_t{777});
}

// ═══════════════════════════════════════════════════════════════
// IDiagnosticSink — pluggable sinks
// ═══════════════════════════════════════════════════════════════

namespace {

// Minimal thread-safe sink implementation used by the sink-plumbing tests.
class TestSink : public oreo::IDiagnosticSink {
public:
    std::atomic<int> count_{0};

    void onReport(const oreo::Diagnostic& d) noexcept override {
        std::lock_guard<std::mutex> lock(mu_);
        last_ = d;
        count_.fetch_add(1, std::memory_order_relaxed);
    }

    oreo::Diagnostic lastCopy() const {
        std::lock_guard<std::mutex> lock(mu_);
        return last_;
    }

private:
    mutable std::mutex mu_;
    oreo::Diagnostic last_{};
};

class ThrowingSink : public oreo::IDiagnosticSink {
public:
    std::atomic<int> count_{0};

    void onReport(const oreo::Diagnostic&) noexcept override {
        count_.fetch_add(1, std::memory_order_relaxed);
        // A noexcept function can't legitimately throw; this simulates a
        // misbehaving sink. Terminate semantics are guarded by the
        // collector's try/catch around sink calls (see logging_sink.h
        // header contract). We wrap in try/catch here so the test does
        // not abort on the terminate path — the point is that the
        // collector must not propagate if the sink itself rethrew.
        //
        // Note: because onReport is declared noexcept at the interface
        // level, genuinely throwing out of here would trigger terminate
        // on most toolchains. Instead we verify the neighbouring
        // behaviour: a well-behaved sink registered alongside this one
        // still fires, and report() itself does not propagate.
    }
};

} // namespace

TEST(DiagnosticSink, SingleSinkReceivesReport) {
    auto ctx = oreo::KernelContext::create();
    auto sink = std::make_shared<TestSink>();
    ctx->diag().addSink(sink);

    ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "boom");

    EXPECT_EQ(sink->count_.load(), 1);
    EXPECT_EQ(sink->lastCopy().code, oreo::ErrorCode::INVALID_INPUT);
    EXPECT_EQ(sink->lastCopy().message, "boom");
}

TEST(DiagnosticSink, MultipleSinksAllReceiveEachReport) {
    auto ctx = oreo::KernelContext::create();
    auto a = std::make_shared<TestSink>();
    auto b = std::make_shared<TestSink>();
    auto c = std::make_shared<TestSink>();
    ctx->diag().addSink(a);
    ctx->diag().addSink(b);
    ctx->diag().addSink(c);

    ctx->diag().info(oreo::ErrorCode::OK, "first");
    ctx->diag().warning(oreo::ErrorCode::SHAPE_INVALID, "second");
    ctx->diag().error(oreo::ErrorCode::BOOLEAN_FAILED, "third");

    EXPECT_EQ(a->count_.load(), 3);
    EXPECT_EQ(b->count_.load(), 3);
    EXPECT_EQ(c->count_.load(), 3);
}

TEST(DiagnosticSink, ClearSinksRemovesAllSinks) {
    auto ctx = oreo::KernelContext::create();
    auto sink = std::make_shared<TestSink>();
    ctx->diag().addSink(sink);

    ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "first");
    EXPECT_EQ(sink->count_.load(), 1);

    ctx->diag().clearSinks();

    ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "second");
    // After clearSinks, the sink must not receive new reports.
    EXPECT_EQ(sink->count_.load(), 1);

    // But the collector itself kept recording.
    EXPECT_EQ(ctx->diag().errorCount(), 2);
}

TEST(DiagnosticSink, MisbehavingSinkDoesNotBreakOtherSinksOrCollector) {
    auto ctx = oreo::KernelContext::create();
    auto misbehaving = std::make_shared<ThrowingSink>();
    auto wellBehaved = std::make_shared<TestSink>();

    ctx->diag().addSink(misbehaving);
    ctx->diag().addSink(wellBehaved);

    // Must not propagate — report() itself is non-throwing for callers.
    EXPECT_NO_THROW(
        ctx->diag().error(oreo::ErrorCode::INTERNAL_ERROR, "combined"));

    // Well-behaved sink still fires; collector still records the diag.
    EXPECT_EQ(wellBehaved->count_.load(), 1);
    EXPECT_EQ(misbehaving->count_.load(), 1);
    EXPECT_EQ(ctx->diag().errorCount(), 1);
}

TEST(DiagnosticSink, SinksReceiveStampedMetadata) {
    auto ctx = oreo::KernelContext::create();
    auto sink = std::make_shared<TestSink>();
    ctx->diag().addSink(sink);

    ctx->diag().error(oreo::ErrorCode::OCCT_FAILURE, "with metadata");

    auto got = sink->lastCopy();
    // stampMetadata_ runs before sinks; timestamp/threadId should be set.
    EXPECT_NE(got.timestampNs, 0u);
    EXPECT_NE(got.threadId, 0u);
}

// ═══════════════════════════════════════════════════════════════
// ResourceQuotas + KernelContext integration
// ═══════════════════════════════════════════════════════════════

TEST(ResourceQuotas, DefaultMaxDiagnosticsIsTenThousand) {
    oreo::ResourceQuotas q{};
    EXPECT_EQ(q.maxDiagnostics, uint64_t{10000});
}

TEST(ResourceQuotas, DefaultKernelConfigPropagatesDefaultQuotas) {
    oreo::KernelConfig cfg{};
    EXPECT_EQ(cfg.quotas.maxDiagnostics, uint64_t{10000});
    EXPECT_EQ(cfg.quotas.maxOperations, uint64_t{0});
    EXPECT_EQ(cfg.quotas.maxContextMemoryBytes, uint64_t{0});
    EXPECT_EQ(cfg.quotas.cpuTimeBudgetMs, uint64_t{0});

    auto ctx = oreo::KernelContext::create(cfg);
    EXPECT_EQ(ctx->quotas().maxDiagnostics, uint64_t{10000});
    EXPECT_EQ(ctx->diag().maxDiagnostics(), std::size_t{10000});
}

TEST(ResourceQuotas, MaxDiagnosticsCapEmitsTruncatedMarker) {
    oreo::KernelConfig cfg{};
    cfg.quotas.maxDiagnostics = 3;
    auto ctx = oreo::KernelContext::create(cfg);

    // Collector's cap should reflect the configured quota.
    EXPECT_EQ(ctx->diag().maxDiagnostics(), std::size_t{3});

    // Report 5 errors; only 3 should land + 1 DIAG_TRUNCATED marker.
    for (int i = 0; i < 5; ++i) {
        ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "err");
    }

    auto snap = ctx->diag().snapshot();
    // 3 real reports + 1 truncation marker — but must never exceed 4.
    EXPECT_LE(snap.size(), std::size_t{4});

    // Exactly one DIAG_TRUNCATED marker must exist after overflow.
    int truncated_count = 0;
    for (const auto& d : snap) {
        if (d.code == oreo::ErrorCode::DIAG_TRUNCATED) ++truncated_count;
    }
    EXPECT_EQ(truncated_count, 1);
}

TEST(ResourceQuotas, FurtherReportsAfterCapDoNotGrowBuffer) {
    oreo::KernelConfig cfg{};
    cfg.quotas.maxDiagnostics = 2;
    auto ctx = oreo::KernelContext::create(cfg);

    for (int i = 0; i < 20; ++i) {
        ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "err");
    }

    auto snap = ctx->diag().snapshot();
    // Cap + 1 marker at most.
    EXPECT_LE(snap.size(), std::size_t{3});

    // DIAG_TRUNCATED appears exactly once even across 20 dropped reports.
    int truncated_count = 0;
    for (const auto& d : snap) {
        if (d.code == oreo::ErrorCode::DIAG_TRUNCATED) ++truncated_count;
    }
    EXPECT_EQ(truncated_count, 1);
}

// ═══════════════════════════════════════════════════════════════
// DiagnosticCollector — direct API surface
// ═══════════════════════════════════════════════════════════════

TEST(DiagnosticCollectorDirect, LastErrorOptReturnsNulloptWhenNoErrors) {
    oreo::DiagnosticCollector diag;
    EXPECT_FALSE(diag.lastErrorOpt().has_value());

    diag.info(oreo::ErrorCode::OK, "just info");
    diag.warning(oreo::ErrorCode::SHAPE_INVALID, "just a warning");
    // Info + warning only → still no error recorded.
    EXPECT_FALSE(diag.lastErrorOpt().has_value());
}

TEST(DiagnosticCollectorDirect, LastErrorOptReturnsMostRecentError) {
    oreo::DiagnosticCollector diag;
    diag.error(oreo::ErrorCode::OCCT_FAILURE, "first error");
    diag.warning(oreo::ErrorCode::SHAPE_INVALID, "warn between");
    diag.error(oreo::ErrorCode::BOOLEAN_FAILED, "second error");

    auto last = diag.lastErrorOpt();
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last->code, oreo::ErrorCode::BOOLEAN_FAILED);
    EXPECT_EQ(last->message, "second error");
}

TEST(DiagnosticCollectorDirect, LastErrorOptIncludesFatal) {
    oreo::DiagnosticCollector diag;
    diag.fatal(oreo::ErrorCode::INTERNAL_ERROR, "fatal boom");

    auto last = diag.lastErrorOpt();
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last->code, oreo::ErrorCode::INTERNAL_ERROR);
}

TEST(DiagnosticCollectorDirect, SnapshotReturnsIndependentCopy) {
    oreo::DiagnosticCollector diag;
    diag.error(oreo::ErrorCode::INVALID_INPUT, "one");
    diag.warning(oreo::ErrorCode::SHAPE_INVALID, "two");

    auto snap = diag.snapshot();
    EXPECT_EQ(snap.size(), std::size_t{2});

    // Mutate the source.
    diag.error(oreo::ErrorCode::OCCT_FAILURE, "three");
    diag.clear();

    // Snapshot stays pinned at its point-in-time view.
    EXPECT_EQ(snap.size(), std::size_t{2});
    EXPECT_EQ(snap[0].code, oreo::ErrorCode::INVALID_INPUT);
    EXPECT_EQ(snap[1].code, oreo::ErrorCode::SHAPE_INVALID);
}

TEST(DiagnosticCollectorDirect, MaxDiagnosticsSetterGetterRoundTrip) {
    oreo::DiagnosticCollector diag;
    EXPECT_EQ(diag.maxDiagnostics(), std::size_t{10000});

    diag.setMaxDiagnostics(42);
    EXPECT_EQ(diag.maxDiagnostics(), std::size_t{42});

    diag.setMaxDiagnostics(1);
    EXPECT_EQ(diag.maxDiagnostics(), std::size_t{1});
}

TEST(DiagnosticCollectorDirect, ContextIdRoundTripAndStamping) {
    oreo::DiagnosticCollector diag;
    EXPECT_EQ(diag.contextId(), uint64_t{0});

    diag.setContextId(0xDEADBEEFu);
    EXPECT_EQ(diag.contextId(), uint64_t{0xDEADBEEFu});

    // Reported diagnostics should carry the configured context ID.
    diag.error(oreo::ErrorCode::INVALID_INPUT, "stamp me");
    auto snap = diag.snapshot();
    ASSERT_EQ(snap.size(), std::size_t{1});
    EXPECT_EQ(snap[0].contextId, uint64_t{0xDEADBEEFu});
}

TEST(DiagnosticCollectorDirect, SetMaxDiagnosticsAppliesTruncation) {
    oreo::DiagnosticCollector diag;
    diag.setMaxDiagnostics(2);

    diag.error(oreo::ErrorCode::INVALID_INPUT, "a");
    diag.error(oreo::ErrorCode::INVALID_INPUT, "b");
    diag.error(oreo::ErrorCode::INVALID_INPUT, "c"); // → truncation marker
    diag.error(oreo::ErrorCode::INVALID_INPUT, "d"); // → dropped

    auto snap = diag.snapshot();
    EXPECT_LE(snap.size(), std::size_t{3});

    int truncated_count = 0;
    for (const auto& d : snap) {
        if (d.code == oreo::ErrorCode::DIAG_TRUNCATED) ++truncated_count;
    }
    EXPECT_EQ(truncated_count, 1);
}

TEST(DiagnosticCollectorDirect, ClearResetsCountersButKeepsSinksAndContextId) {
    oreo::DiagnosticCollector diag;
    auto sink = std::make_shared<TestSink>();
    diag.addSink(sink);
    diag.setContextId(7);

    diag.error(oreo::ErrorCode::INVALID_INPUT, "e1");
    diag.warning(oreo::ErrorCode::SHAPE_INVALID, "w1");
    EXPECT_EQ(diag.errorCount(), 1);
    EXPECT_EQ(diag.warningCount(), 1);
    EXPECT_EQ(sink->count_.load(), 2);

    diag.clear();
    EXPECT_TRUE(diag.empty());
    EXPECT_EQ(diag.errorCount(), 0);
    EXPECT_EQ(diag.warningCount(), 0);
    // clear() must not reset the context ID or drop sinks.
    EXPECT_EQ(diag.contextId(), uint64_t{7});

    diag.error(oreo::ErrorCode::INVALID_INPUT, "after clear");
    EXPECT_EQ(sink->count_.load(), 3);
    auto snap = diag.snapshot();
    ASSERT_EQ(snap.size(), std::size_t{1});
    EXPECT_EQ(snap[0].contextId, uint64_t{7});
}
