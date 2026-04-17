// test_foundation_audit_fixes.cpp — Regression tests for the 5 issues
// raised by an external contributor review after the initial foundation
// audit merge. Each TEST corresponds to one fix.
//
// Contributor issues:
//   P1-A  OcctStaticGuard: repeated set() on the same key left the
//         global at the intermediate value on destruction.
//   P1-B  freezeSchemas() was advisory — legacy schemas() overload
//         and mutableSchemas() still allowed mutation.
//   P2-C  ConfigLoader truncated KernelConfig::documentId (uint64_t)
//         to uint32_t in both env and JSON paths.
//   P2-D  TagAllocator tag encoding uses only low-32 bits of docId,
//         so two distinct 64-bit documentIds with equal low 32 bits
//         produce identical encoded tags — undetected.
//   P2-E  DiagnosticCollector cap was not strict: the DIAG_TRUNCATED
//         marker was appended to the same vector, growing size to
//         cap+1. setMaxDiagnostics(0) also inconsistent with quotas.

#include "core/diagnostic.h"
#include "core/kernel_context.h"
#include "core/schema.h"
#include "core/tag_allocator.h"
#include "core/occt_scope_guard.h"
#include "core/config_loader.h"

#include <Interface_Static.hxx>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {

// Cross-platform env helpers — duplicated locally so this TU is self-contained.
void setEnv(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void unsetEnv(const char* name) {
#if defined(_WIN32)
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

struct EnvGuard {
    std::string name;
    EnvGuard(const char* n, const char* v) : name(n) { setEnv(n, v); }
    ~EnvGuard() { unsetEnv(name.c_str()); }
};

} // namespace

// ═══════════════════════════════════════════════════════════════
// Fix 1 (P1-A) — OcctStaticGuard must de-dup saved keys so a
// repeated set() on the same key restores the ORIGINAL, not the
// intermediate, on scope exit.
// ═══════════════════════════════════════════════════════════════

namespace {
// Ensure a string-valued OCCT static key exists with the given initial
// value. Interface_Static requires keys to be declared via Init before
// Set/CVal will accept them — STEP-subsystem keys are lazy-initialised
// by STEP I/O, which hasn't run in a bare foundation test.
void initStringKey(const char* family, const char* key, const char* init) {
    if (Interface_Static::IsSet(key) == Standard_False) {
        Interface_Static::Init(family, key, 't', init);
    } else {
        Interface_Static::SetCVal(key, init);
    }
}
void initIntKey(const char* family, const char* key, int init) {
    if (Interface_Static::IsSet(key) == Standard_False) {
        // 'i' = integer. Init expects a string default; convert.
        Interface_Static::Init(family, key, 'i',
                               std::to_string(init).c_str());
    } else {
        Interface_Static::SetIVal(key, init);
    }
}
} // namespace

TEST(AuditFix_OcctStaticGuard, RepeatedSameKeySetRestoresOriginal) {
    // Use a test-only OCCT static — STEP's real keys are only registered
    // when the STEP subsystem runs, which we don't in a foundation test.
    const char* KEY = "oreo.audit.fix1.strkey";
    initStringKey("oreo-audit-fix", KEY, "ORIGINAL");

    {
        oreo::OcctStaticGuard guard;
        guard.set(KEY, "INTERMEDIATE");   // saves "ORIGINAL"
        guard.set(KEY, "LATEST");         // MUST NOT save "INTERMEDIATE"
        EXPECT_STREQ(Interface_Static::CVal(KEY), "LATEST");
    }
    // After the guard, back to the ORIGINAL, not left at "INTERMEDIATE".
    EXPECT_STREQ(Interface_Static::CVal(KEY), "ORIGINAL");
}

TEST(AuditFix_OcctStaticGuard, RepeatedIntKeySetRestoresOriginal) {
    const char* KEY = "oreo.audit.fix1.intkey";
    initIntKey("oreo-audit-fix", KEY, 5);

    {
        oreo::OcctStaticGuard guard;
        guard.set(KEY, 1);
        guard.set(KEY, 2);
        guard.set(KEY, 3);
        EXPECT_EQ(Interface_Static::IVal(KEY), 3);
    }
    EXPECT_EQ(Interface_Static::IVal(KEY), 5);
}

TEST(AuditFix_OcctStaticGuard, MixedKeysStillWork) {
    // Make sure the de-dup doesn't break the ordinary "different keys"
    // path — each gets saved once and all restore correctly.
    const char* K1 = "oreo.audit.fix1.mixed.str";
    const char* K2 = "oreo.audit.fix1.mixed.int";
    initStringKey("oreo-audit-fix", K1, "A");
    initIntKey("oreo-audit-fix", K2, 5);

    {
        oreo::OcctStaticGuard guard;
        guard.set(K1, "B");
        guard.set(K2, 2);
        guard.set(K1, "C");     // repeat on K1 — still first-wins
    }
    EXPECT_STREQ(Interface_Static::CVal(K1), "A");
    EXPECT_EQ(Interface_Static::IVal(K2), 5);
}

// ═══════════════════════════════════════════════════════════════
// Fix 2 (P1-B) — SchemaRegistry freeze must be ENFORCED at the
// registry level, so mutations through any accessor fail.
// ═══════════════════════════════════════════════════════════════

TEST(AuditFix_SchemaFreeze, RegisterMigrationAfterFreezeThrows) {
    oreo::SchemaRegistry reg;
    EXPECT_FALSE(reg.isFrozen());
    reg.freeze();
    EXPECT_TRUE(reg.isFrozen());

    EXPECT_THROW(
        reg.registerMigration("oreo.feature_tree",
                              oreo::SchemaVersion{0, 1, 0},
                              oreo::SchemaVersion{1, 0, 0},
                              [](const nlohmann::json& d) { return d; }),
        std::logic_error);
}

TEST(AuditFix_SchemaFreeze, UnregisterMigrationAfterFreezeThrows) {
    oreo::SchemaRegistry reg;
    reg.registerMigration("oreo.feature_tree",
                          oreo::SchemaVersion{0, 1, 0},
                          oreo::SchemaVersion{1, 0, 0},
                          [](const nlohmann::json& d) { return d; });
    reg.freeze();

    EXPECT_THROW(
        reg.unregisterMigration("oreo.feature_tree",
                                oreo::SchemaVersion{0, 1, 0}),
        std::logic_error);
}

TEST(AuditFix_SchemaFreeze, SetMaxMigrationStepsAfterFreezeThrows) {
    oreo::SchemaRegistry reg;
    reg.freeze();
    EXPECT_THROW(reg.setMaxMigrationSteps(500), std::logic_error);
}

TEST(AuditFix_SchemaFreeze, LegacyContextSchemasOverloadCannotBypass) {
    // The legacy non-const ctx->schemas() used to be a silent mutation
    // bypass. With registry-level enforcement, even that path throws.
    auto ctx = oreo::KernelContext::create();
    ctx->freezeSchemas();
    EXPECT_THROW(
        ctx->schemas().registerMigration(
            "oreo.feature_tree",
            oreo::SchemaVersion{0, 1, 0},
            oreo::SchemaVersion{1, 0, 0},
            [](const nlohmann::json& d) { return d; }),
        std::logic_error);
}

TEST(AuditFix_SchemaFreeze, MutableSchemasCannotBypass) {
    auto ctx = oreo::KernelContext::create();
    ctx->freezeSchemas();
    // mutableSchemas still reports a diagnostic AND the returned ref's
    // mutations still throw.
    auto& reg = ctx->mutableSchemas();
    EXPECT_THROW(
        reg.registerMigration("oreo.feature_tree",
                              oreo::SchemaVersion{0, 1, 0},
                              oreo::SchemaVersion{1, 0, 0},
                              [](const nlohmann::json& d) { return d; }),
        std::logic_error);
}

TEST(AuditFix_SchemaFreeze, ReadOnlyQueriesStillWorkAfterFreeze) {
    // Freeze must not break const methods — tooling depends on them.
    oreo::SchemaRegistry reg;
    reg.registerMigration("oreo.feature_tree",
                          oreo::SchemaVersion{0, 1, 0},
                          oreo::SchemaVersion{1, 0, 0},
                          [](const nlohmann::json& d) { return d; });
    reg.freeze();

    EXPECT_FALSE(reg.registeredTypes().empty());
    EXPECT_FALSE(reg.versionsFor("oreo.feature_tree").empty());
    EXPECT_NO_THROW(reg.currentVersion("oreo.feature_tree"));
    EXPECT_GT(reg.maxMigrationSteps(), size_t{0});
}

// ═══════════════════════════════════════════════════════════════
// Fix 3 (P2-C) — ConfigLoader must preserve the full 64 bits of
// KernelConfig::documentId (env + JSON paths).
// ═══════════════════════════════════════════════════════════════

TEST(AuditFix_ConfigLoader, EnvDocumentIdPreservesHighBits) {
    // A value that loses information if truncated to uint32_t.
    // int64_t -1 reinterpreted as uint64_t is 0xFFFFFFFFFFFFFFFF.
    // A uint32_t cast of that would yield 0xFFFFFFFF, missing 32 high bits.
    EnvGuard g("OREO_DOCUMENT_ID", "-1");

    oreo::KernelConfig cfg;
    oreo::ConfigLoader::applyEnvOverlay(cfg);
    EXPECT_EQ(cfg.documentId,
              static_cast<uint64_t>(0xFFFFFFFFFFFFFFFFull));
}

TEST(AuditFix_ConfigLoader, JsonDocumentIdPreservesHighBits) {
    const auto path = std::filesystem::temp_directory_path()
                    / "oreo_audit_fix_docid.json";
    {
        std::ofstream f(path);
        // JSON number above UINT32_MAX — must not be truncated.
        f << R"({"documentId": 281474976710657})";  // 0x0001000000000001
    }
    oreo::KernelConfig cfg;
    oreo::ConfigLoader::applyJsonFile(cfg, path.string());
    EXPECT_EQ(cfg.documentId, uint64_t{0x0001000000000001ull});
    std::filesystem::remove(path);
}

// ═══════════════════════════════════════════════════════════════
// Fix 4 (P2-D) — TagAllocator must detect a distinct 64-bit
// documentId collision in the low-32-bit encoded-tag slot.
// ═══════════════════════════════════════════════════════════════

TEST(AuditFix_TagCollisionDetection, TwoIdsWithSameLow32Collide) {
    // Use a fresh registry to avoid interference from prior tests.
    oreo::TagAllocator::clearDocumentIdRegistry();

    // Both have low32 = 0x00000001; high 32 bits differ.
    const uint64_t a = 0x0000000100000001ull;
    const uint64_t b = 0x0000000200000001ull;

    EXPECT_NO_THROW(oreo::TagAllocator::registerDocumentId(a));
    EXPECT_THROW(oreo::TagAllocator::registerDocumentId(b),
                 std::logic_error);

    oreo::TagAllocator::clearDocumentIdRegistry();
}

TEST(AuditFix_TagCollisionDetection, SameIdReregisterIsIdempotent) {
    oreo::TagAllocator::clearDocumentIdRegistry();
    const uint64_t id = 0xDEADBEEFCAFEBABEull;
    EXPECT_NO_THROW(oreo::TagAllocator::registerDocumentId(id));
    EXPECT_NO_THROW(oreo::TagAllocator::registerDocumentId(id));
    EXPECT_NO_THROW(oreo::TagAllocator::registerDocumentId(id));
    oreo::TagAllocator::clearDocumentIdRegistry();
}

TEST(AuditFix_TagCollisionDetection, ConstructorCollisionDetected) {
    oreo::TagAllocator::clearDocumentIdRegistry();

    // Two distinct 64-bit documentIds sharing low-32 bits 0x12345678.
    const uint64_t a = 0x0000000012345678ull;
    const uint64_t b = 0x1111111112345678ull;

    oreo::TagAllocator allocA(a);
    EXPECT_THROW(oreo::TagAllocator allocB(b), std::logic_error);

    oreo::TagAllocator::clearDocumentIdRegistry();
}

TEST(AuditFix_TagCollisionDetection, SetDocumentIdCollisionDetected) {
    oreo::TagAllocator::clearDocumentIdRegistry();
    oreo::TagAllocator allocA;
    oreo::TagAllocator allocB;
    allocA.setDocumentId(0x0000000000000042ull);
    EXPECT_THROW(allocB.setDocumentId(0xFFFFFFFF00000042ull),
                 std::logic_error);
    oreo::TagAllocator::clearDocumentIdRegistry();
}

// ═══════════════════════════════════════════════════════════════
// Fix 5 (P2-E) — DiagnosticCollector cap must be STRICT: size
// never exceeds maxDiagnostics_. Also, setMaxDiagnostics(0) maps
// to unlimited (consistent with KernelContext quotas).
// ═══════════════════════════════════════════════════════════════

TEST(AuditFix_DiagnosticHardCap, SizeNeverExceedsCap) {
    oreo::DiagnosticCollector diag;
    diag.setMaxDiagnostics(3);

    // Report far more than the cap.
    for (int i = 0; i < 20; ++i) {
        diag.error(oreo::ErrorCode::INVALID_INPUT, "err");
    }
    auto snap = diag.snapshot();
    EXPECT_EQ(snap.size(), size_t{3});  // hard cap, exactly 3
    EXPECT_TRUE(diag.isTruncated());
    EXPECT_GT(diag.overflowCount(), size_t{0});
}

TEST(AuditFix_DiagnosticHardCap, TruncatedMarkerReplacesLastSlot) {
    oreo::DiagnosticCollector diag;
    diag.setMaxDiagnostics(4);

    for (int i = 0; i < 10; ++i) {
        diag.error(oreo::ErrorCode::INVALID_INPUT, "err-" + std::to_string(i));
    }

    auto snap = diag.snapshot();
    ASSERT_EQ(snap.size(), size_t{4});

    // Exactly one marker, occupying the LAST slot (we replace-last on
    // overflow so snapshot consumers immediately see the loss).
    int markerCount = 0;
    for (const auto& d : snap) {
        if (d.code == oreo::ErrorCode::DIAG_TRUNCATED) ++markerCount;
    }
    EXPECT_EQ(markerCount, 1);
    EXPECT_EQ(snap.back().code, oreo::ErrorCode::DIAG_TRUNCATED);
}

TEST(AuditFix_DiagnosticHardCap, ZeroCapMeansUnlimited) {
    // Consistent with KernelContext quota semantics where 0 = unlimited.
    oreo::DiagnosticCollector diag;
    diag.setMaxDiagnostics(0);
    EXPECT_EQ(diag.maxDiagnostics(),
              std::numeric_limits<std::size_t>::max());

    for (int i = 0; i < 1000; ++i) {
        diag.warning(oreo::ErrorCode::INVALID_INPUT, "w");
    }
    EXPECT_FALSE(diag.isTruncated());
    EXPECT_EQ(diag.snapshot().size(), size_t{1000});
}
