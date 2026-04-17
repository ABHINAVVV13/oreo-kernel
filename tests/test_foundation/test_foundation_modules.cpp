// test_foundation_modules.cpp — Tests for new foundation modules.
//
// Covers: CancellationToken, FeatureFlags, MessageCatalog, DeterministicRNG,
//         ProfileScope + Metrics, Arena, Assertion framework.
//
// Progress (KernelContext-backed) is intentionally NOT covered here — it is
// handled by test_foundation_new_features.cpp.

#include <gtest/gtest.h>

#include "core/cancellation.h"
#include "core/feature_flags.h"
#include "core/localization.h"
#include "core/rng.h"
#include "core/profile.h"
#include "core/metrics.h"
#include "core/memory_pool.h"
#include "core/assertion.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

// Cross-platform env setter used by FeatureFlags.LoadFlagFromEnvironment.
namespace {
void setEnvVar(const char* name, const char* value) {
#if defined(_WIN32)
    std::string kv = std::string(name) + "=" + std::string(value ? value : "");
    _putenv(kv.c_str());
#else
    ::setenv(name, value ? value : "", 1);
#endif
}

void unsetEnvVar(const char* name) {
#if defined(_WIN32)
    std::string kv = std::string(name) + "=";
    _putenv(kv.c_str());
#else
    ::unsetenv(name);
#endif
}
} // namespace

// ─── CancellationToken ───

TEST(CancellationToken, DefaultNotCancelled) {
    oreo::CancellationToken tok;
    EXPECT_FALSE(tok.isCancelled());
}

TEST(CancellationToken, CancelFlipsFlag) {
    oreo::CancellationToken tok;
    tok.cancel();
    EXPECT_TRUE(tok.isCancelled());
}

TEST(CancellationToken, ResetClearsFlag) {
    oreo::CancellationToken tok;
    tok.cancel();
    EXPECT_TRUE(tok.isCancelled());
    tok.reset();
    EXPECT_FALSE(tok.isCancelled());
}

TEST(CancellationToken, MultiThreadProducerConsumer) {
    oreo::CancellationToken tok;
    std::atomic<bool> observed{false};

    std::thread consumer([&] {
        // Poll until we see the cancel signal or we spin too long.
        for (int i = 0; i < 1'000'000; ++i) {
            if (tok.isCancelled()) {
                observed.store(true, std::memory_order_release);
                return;
            }
            std::this_thread::yield();
        }
    });

    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        tok.cancel();
    });

    producer.join();
    consumer.join();
    EXPECT_TRUE(observed.load(std::memory_order_acquire));
    EXPECT_TRUE(tok.isCancelled());
}

TEST(CancellationToken, SharedPtrSemantics) {
    oreo::CancellationTokenPtr a = std::make_shared<oreo::CancellationToken>();
    oreo::CancellationTokenPtr b = a;  // Shared ownership.

    EXPECT_FALSE(a->isCancelled());
    EXPECT_FALSE(b->isCancelled());

    b->cancel();
    // Both views reference the same flag.
    EXPECT_TRUE(a->isCancelled());
    EXPECT_TRUE(b->isCancelled());

    a->reset();
    EXPECT_FALSE(b->isCancelled());
    EXPECT_EQ(a.use_count(), 2);
}

// ─── FeatureFlags ───

TEST(FeatureFlags, SetIsEnabledRoundTrip) {
    auto& ff = oreo::FeatureFlags::instance();
    ff.clear("module_test_flag_rt");  // Ensure clean slate.

    EXPECT_FALSE(ff.isEnabled("module_test_flag_rt"));
    ff.set("module_test_flag_rt", true);
    EXPECT_TRUE(ff.isEnabled("module_test_flag_rt"));
    ff.set("module_test_flag_rt", false);
    EXPECT_FALSE(ff.isEnabled("module_test_flag_rt"));

    ff.clear("module_test_flag_rt");
}

TEST(FeatureFlags, UnknownFlagReturnsFalse) {
    auto& ff = oreo::FeatureFlags::instance();
    EXPECT_FALSE(ff.isEnabled("module_test_never_registered_xyz_123"));
}

TEST(FeatureFlags, ClearRemovesFlag) {
    auto& ff = oreo::FeatureFlags::instance();
    ff.set("module_test_flag_clear", true);
    EXPECT_TRUE(ff.isEnabled("module_test_flag_clear"));

    ff.clear("module_test_flag_clear");
    EXPECT_FALSE(ff.isEnabled("module_test_flag_clear"));

    const auto snap = ff.snapshot();
    EXPECT_EQ(snap.count("module_test_flag_clear"), 0u);
}

TEST(FeatureFlags, SnapshotReturnsCopy) {
    auto& ff = oreo::FeatureFlags::instance();
    ff.clear("module_test_snap_a");
    ff.clear("module_test_snap_b");

    ff.set("module_test_snap_a", true);
    ff.set("module_test_snap_b", false);

    auto snap = ff.snapshot();
    ASSERT_EQ(snap.count("module_test_snap_a"), 1u);
    ASSERT_EQ(snap.count("module_test_snap_b"), 1u);
    EXPECT_TRUE(snap["module_test_snap_a"]);
    EXPECT_FALSE(snap["module_test_snap_b"]);

    // Mutating the copy does not affect the live registry.
    snap["module_test_snap_a"] = false;
    EXPECT_TRUE(ff.isEnabled("module_test_snap_a"));

    ff.clear("module_test_snap_a");
    ff.clear("module_test_snap_b");
}

TEST(FeatureFlags, LoadFlagFromEnvironment) {
    auto& ff = oreo::FeatureFlags::instance();
    const char* envName  = "OREO_TEST_LOAD_FLAG_ENV";
    const char* flagName = "module_test_env_flag";

    ff.clear(flagName);
    unsetEnvVar(envName);

    // No env var => loadFlag reports no load.
    EXPECT_FALSE(ff.loadFlag(envName, flagName));
    EXPECT_FALSE(ff.isEnabled(flagName));

    // "true" => enabled.
    setEnvVar(envName, "true");
    EXPECT_TRUE(ff.loadFlag(envName, flagName));
    EXPECT_TRUE(ff.isEnabled(flagName));

    // "0" => disabled.
    setEnvVar(envName, "0");
    EXPECT_TRUE(ff.loadFlag(envName, flagName));
    EXPECT_FALSE(ff.isEnabled(flagName));

    unsetEnvVar(envName);
    ff.clear(flagName);
}

// ─── MessageCatalog ───

TEST(MessageCatalog, AddAndGetRoundTripEn) {
    auto& mc = oreo::MessageCatalog::instance();
    mc.setLocale("en");
    mc.add("en", "module_test.greet", "Hello");
    EXPECT_EQ(mc.get("module_test.greet"), "Hello");
}

TEST(MessageCatalog, LocaleFallbackToEn) {
    auto& mc = oreo::MessageCatalog::instance();
    mc.add("en", "module_test.fallback", "English value");
    // Intentionally DO NOT add a French entry.
    mc.setLocale("fr");
    EXPECT_EQ(mc.get("module_test.fallback"), "English value");
    mc.setLocale("en");
}

TEST(MessageCatalog, UnknownIdReturnsId) {
    auto& mc = oreo::MessageCatalog::instance();
    mc.setLocale("en");
    const std::string id = "module_test.definitely_missing_id_987";
    EXPECT_EQ(mc.get(id), id);
}

TEST(MessageCatalog, PlaceholderSubstitutionSingle) {
    auto& mc = oreo::MessageCatalog::instance();
    mc.setLocale("en");
    mc.add("en", "module_test.single_ph", "Hello {0}");
    EXPECT_EQ(mc.get("module_test.single_ph", {"world"}), "Hello world");
}

TEST(MessageCatalog, PlaceholderMultipleAndOutOfRange) {
    auto& mc = oreo::MessageCatalog::instance();
    mc.setLocale("en");
    mc.add("en", "module_test.multi_ph", "{0} and {1}, but not {5}");
    // Out-of-range index {5} preserved literally per the header contract.
    EXPECT_EQ(mc.get("module_test.multi_ph", {"a", "b"}),
              "a and b, but not {5}");
}

TEST(MessageCatalog, DoubledBraceEscape) {
    auto& mc = oreo::MessageCatalog::instance();
    mc.setLocale("en");
    mc.add("en", "module_test.escape", "{{");
    EXPECT_EQ(mc.get("module_test.escape"), "{");
}

// ─── DeterministicRNG ───

TEST(DeterministicRNG, SameSeedSameSequence) {
    oreo::DeterministicRNG a(42);
    oreo::DeterministicRNG b(42);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(a.next(), b.next()) << "Divergence at step " << i;
    }
}

TEST(DeterministicRNG, DifferentSeedsDifferentSequences) {
    oreo::DeterministicRNG a(1);
    oreo::DeterministicRNG b(2);
    std::vector<uint64_t> va, vb;
    for (int i = 0; i < 100; ++i) {
        va.push_back(a.next());
        vb.push_back(b.next());
    }
    EXPECT_NE(va, vb);
    // And there should be many distinct values between the two streams.
    std::set<uint64_t> diffs;
    for (int i = 0; i < 100; ++i) diffs.insert(va[i] ^ vb[i]);
    EXPECT_GT(diffs.size(), 50u);
}

TEST(DeterministicRNG, NextUniformInRange) {
    oreo::DeterministicRNG rng(123);
    for (int i = 0; i < 1000; ++i) {
        const double v = rng.nextUniform();
        EXPECT_GE(v, 0.0);
        EXPECT_LT(v, 1.0);
    }
}

TEST(DeterministicRNG, NextRangeInBounds) {
    oreo::DeterministicRNG rng(0xC0FFEEULL);
    const uint64_t lo = 10, hi = 20;
    bool sawLo = false, sawHi_minus_1 = false;
    for (int i = 0; i < 5000; ++i) {
        const uint64_t v = rng.nextRange(lo, hi);
        EXPECT_GE(v, lo);
        EXPECT_LT(v, hi);
        if (v == lo)     sawLo = true;
        if (v == hi - 1) sawHi_minus_1 = true;
    }
    // With 5000 draws over a 10-wide range, coverage should be total.
    EXPECT_TRUE(sawLo);
    EXPECT_TRUE(sawHi_minus_1);
}

TEST(DeterministicRNG, NextRangeDegenerateReturnsLo) {
    oreo::DeterministicRNG rng(7);
    EXPECT_EQ(rng.nextRange(5, 5), 5u);
    EXPECT_EQ(rng.nextRange(9, 3), 9u);
}

// ─── ProfileScope + Metrics ───

TEST(ProfileMetrics, ProfileScopeRecordsObservation) {
    oreo::Metrics m;
    {
        oreo::ProfileScope scope(m, "module_test.op");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // averageNs > 0 confirms observeNanoseconds was invoked.
    EXPECT_GT(m.averageNs("module_test.op"), 0.0);
}

TEST(ProfileMetrics, IncrementAccumulates) {
    oreo::Metrics m;
    m.increment("module_test.ctr");
    m.increment("module_test.ctr", 4);
    m.increment("module_test.ctr");
    EXPECT_EQ(m.counter("module_test.ctr"), 6u);
}

TEST(ProfileMetrics, SnapshotReturnsCounters) {
    oreo::Metrics m;
    m.increment("module_test.snap_a", 3);
    m.increment("module_test.snap_b", 7);

    auto snap = m.snapshot();
    ASSERT_EQ(snap.count("module_test.snap_a"), 1u);
    ASSERT_EQ(snap.count("module_test.snap_b"), 1u);
    EXPECT_EQ(snap["module_test.snap_a"], 3u);
    EXPECT_EQ(snap["module_test.snap_b"], 7u);

    // Mutating the snapshot does not touch live counters.
    snap["module_test.snap_a"] = 99;
    EXPECT_EQ(m.counter("module_test.snap_a"), 3u);
}

TEST(ProfileMetrics, AverageNsCorrectness) {
    oreo::Metrics m;
    m.observeNanoseconds("module_test.avg", 100);
    m.observeNanoseconds("module_test.avg", 200);
    m.observeNanoseconds("module_test.avg", 300);
    EXPECT_DOUBLE_EQ(m.averageNs("module_test.avg"), 200.0);

    // Unknown name => 0.0.
    EXPECT_DOUBLE_EQ(m.averageNs("module_test.avg_missing"), 0.0);
}

TEST(ProfileMetrics, ConcurrentIncrementsProduceCorrectTotal) {
    oreo::Metrics m;
    constexpr int kPerThread = 10'000;

    std::thread t1([&] {
        for (int i = 0; i < kPerThread; ++i) m.increment("module_test.concur");
    });
    std::thread t2([&] {
        for (int i = 0; i < kPerThread; ++i) m.increment("module_test.concur");
    });
    t1.join();
    t2.join();

    EXPECT_EQ(m.counter("module_test.concur"),
              static_cast<uint64_t>(2 * kPerThread));
}

// ─── Arena (memory_pool) ───

TEST(Arena, AllocateReturnsAlignedPointer) {
    oreo::Arena arena;
    void* p16 = arena.allocate(7, 16);
    void* p64 = arena.allocate(3, 64);
    EXPECT_NE(p16, nullptr);
    EXPECT_NE(p64, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p16) % 16u, 0u);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p64) % 64u, 0u);
}

TEST(Arena, TotalAllocatedSumsRequests) {
    oreo::Arena arena;
    EXPECT_EQ(arena.totalAllocated(), 0u);

    arena.allocate(17);
    EXPECT_EQ(arena.totalAllocated(), 17u);

    arena.allocate(25);
    EXPECT_EQ(arena.totalAllocated(), 17u + 25u);
}

TEST(Arena, ResetZeroesTotalAllocated) {
    oreo::Arena arena;
    arena.allocate(100);
    arena.allocate(200);
    EXPECT_GT(arena.totalAllocated(), 0u);
    EXPECT_GT(arena.blockCount(), 0u);

    arena.reset();
    EXPECT_EQ(arena.totalAllocated(), 0u);
    // After reset we can allocate again — the arena remains usable.
    void* p = arena.allocate(8);
    EXPECT_NE(p, nullptr);
    EXPECT_EQ(arena.totalAllocated(), 8u);
}

TEST(Arena, ConstructTriviallyDestructibleStruct) {
    struct Point {
        int x;
        int y;
        double z;
    };
    static_assert(std::is_trivially_destructible_v<Point>,
                  "Point must be trivially destructible for Arena::construct");

    oreo::Arena arena;
    Point* p = arena.construct<Point>(Point{1, 2, 3.5});
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->x, 1);
    EXPECT_EQ(p->y, 2);
    EXPECT_DOUBLE_EQ(p->z, 3.5);
    // The pointer must meet Point's alignment requirement.
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % alignof(Point), 0u);
}

TEST(Arena, LargeAllocationSpillsToNewBlock) {
    oreo::Arena arena(/*blockSize=*/64);
    const std::size_t blocksBefore = arena.blockCount();

    // First small allocation forces one block.
    arena.allocate(8);
    const std::size_t blocksAfterSmall = arena.blockCount();
    EXPECT_GT(blocksAfterSmall, blocksBefore);

    // A request larger than the current block size must spill.
    arena.allocate(4096);
    EXPECT_GT(arena.blockCount(), blocksAfterSmall);
    EXPECT_EQ(arena.totalAllocated(), 8u + 4096u);
}

// ─── Assertion ───

namespace {
// RAII helper — restores the previous AssertAction so test bleed-through
// doesn't alter the policy seen by later tests.
struct AssertActionGuard {
    oreo::AssertAction prev;
    AssertActionGuard() : prev(oreo::AssertionConfig::action()) {}
    ~AssertActionGuard() { oreo::AssertionConfig::setAction(prev); }
};
} // namespace

TEST(Assertion, VerifyTrueDoesNothing) {
    AssertActionGuard guard;
    oreo::AssertionConfig::setAction(oreo::AssertAction::Throw);
    EXPECT_NO_THROW(OREO_VERIFY(1 + 1 == 2, "trivial"));
}

TEST(Assertion, VerifyFalseThrowsWhenThrowActionSet) {
    AssertActionGuard guard;
    oreo::AssertionConfig::setAction(oreo::AssertAction::Throw);
    EXPECT_THROW(OREO_VERIFY(false, "intentional"), std::logic_error);
}

TEST(Assertion, ReportAndContinueDoesNotThrow) {
    AssertActionGuard guard;
    oreo::AssertionConfig::setAction(oreo::AssertAction::ReportAndContinue);
    EXPECT_NO_THROW(OREO_VERIFY(false, "swallowed"));
}

TEST(Assertion, AssertCompilesOutInReleaseBuild) {
    // In NDEBUG (release) OREO_ASSERT is ((void)0) — it must never evaluate
    // its expression, let alone fire the handler. In debug we just confirm
    // that OREO_ASSERT with a true expression is a no-op.
#ifdef NDEBUG
    // Even with Throw configured, a release-compiled OREO_ASSERT(false,...)
    // must not throw — it's been preprocessed away.
    AssertActionGuard guard;
    oreo::AssertionConfig::setAction(oreo::AssertAction::Throw);
    EXPECT_NO_THROW(OREO_ASSERT(false, "compiled out"));
#else
    AssertActionGuard guard;
    oreo::AssertionConfig::setAction(oreo::AssertAction::ReportAndContinue);
    EXPECT_NO_THROW(OREO_ASSERT(true, "trivial true"));
#endif
}
