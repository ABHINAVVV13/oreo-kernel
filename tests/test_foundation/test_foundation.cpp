// test_foundation.cpp — Comprehensive tests for all 7 foundation items.

#include <gtest/gtest.h>

#include "core/kernel_context.h"
#include "core/tag_allocator.h"
#include "core/diagnostic.h"
#include "core/units.h"
#include "core/schema.h"
#include "core/validation.h"
#include "core/oreo_error.h"
#include "naming/named_shape.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <cmath>

// ═══════════════════════════════════════════════════════════════
// 1. TagAllocator — Deterministic per-context IDs
// ═══════════════════════════════════════════════════════════════

TEST(TagAllocator, SequentialAllocation) {
    oreo::TagAllocator alloc;
    EXPECT_EQ(alloc.nextTag(), 1);
    EXPECT_EQ(alloc.nextTag(), 2);
    EXPECT_EQ(alloc.nextTag(), 3);
    EXPECT_EQ(alloc.currentValue(), 3);
}

TEST(TagAllocator, ResetForDeterministicReplay) {
    oreo::TagAllocator alloc;
    alloc.nextTag(); alloc.nextTag(); alloc.nextTag();  // 1, 2, 3
    alloc.reset();
    EXPECT_EQ(alloc.nextTag(), 1);  // Same sequence after reset
    EXPECT_EQ(alloc.nextTag(), 2);
    EXPECT_EQ(alloc.nextTag(), 3);
}

TEST(TagAllocator, SeedFromPersistence) {
    oreo::TagAllocator alloc;
    alloc.seed(100);
    EXPECT_EQ(alloc.nextTag(), 101);
    EXPECT_EQ(alloc.nextTag(), 102);
}

TEST(TagAllocator, TwoAllocatorsIndependent) {
    oreo::TagAllocator alloc1, alloc2;
    EXPECT_EQ(alloc1.nextTag(), 1);
    EXPECT_EQ(alloc2.nextTag(), 1);  // Independent — same sequence
    EXPECT_EQ(alloc1.nextTag(), 2);
    EXPECT_EQ(alloc2.nextTag(), 2);
}

TEST(TagAllocator, EmptyCheck) {
    oreo::TagAllocator alloc;
    EXPECT_TRUE(alloc.empty());
    alloc.nextTag();
    EXPECT_FALSE(alloc.empty());
    alloc.reset();
    EXPECT_TRUE(alloc.empty());
}

// ═══════════════════════════════════════════════════════════════
// 2. DiagnosticCollector — Structured diagnostics
// ═══════════════════════════════════════════════════════════════

TEST(Diagnostic, ReportAndQuery) {
    oreo::DiagnosticCollector diag;
    EXPECT_TRUE(diag.empty());

    diag.error(oreo::ErrorCode::INVALID_INPUT, "Bad input");
    EXPECT_FALSE(diag.empty());
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_FALSE(diag.hasWarnings());
    EXPECT_EQ(diag.count(), 1);
    EXPECT_EQ(diag.errorCount(), 1);
}

TEST(Diagnostic, MultipleDiagnostics) {
    oreo::DiagnosticCollector diag;
    diag.info(oreo::ErrorCode::OK, "Started operation");
    diag.warning(oreo::ErrorCode::SHAPE_INVALID, "Shape quality reduced");
    diag.error(oreo::ErrorCode::BOOLEAN_FAILED, "Boolean operation failed");

    EXPECT_EQ(diag.count(), 3);
    EXPECT_EQ(diag.errorCount(), 1);
    EXPECT_EQ(diag.warningCount(), 1);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(diag.hasWarnings());
}

TEST(Diagnostic, DegradedFlags) {
    oreo::DiagnosticCollector diag;
    EXPECT_FALSE(diag.isGeometryDegraded());
    EXPECT_FALSE(diag.isNamingDegraded());

    oreo::Diagnostic d;
    d.severity = oreo::Severity::Warning;
    d.code = oreo::ErrorCode::SHAPE_INVALID;
    d.message = "Healed geometry";
    d.geometryDegraded = true;
    diag.report(d);

    EXPECT_TRUE(diag.isGeometryDegraded());
    EXPECT_FALSE(diag.isNamingDegraded());
}

TEST(Diagnostic, Clear) {
    oreo::DiagnosticCollector diag;
    diag.error(oreo::ErrorCode::OCCT_FAILURE, "Failure");
    diag.warning(oreo::ErrorCode::SHAPE_INVALID, "Warning");
    EXPECT_EQ(diag.count(), 2);

    diag.clear();
    EXPECT_TRUE(diag.empty());
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_FALSE(diag.hasWarnings());
}

TEST(Diagnostic, LastError) {
    oreo::DiagnosticCollector diag;
    EXPECT_EQ(diag.lastError(), nullptr);

    diag.warning(oreo::ErrorCode::SHAPE_INVALID, "First warning");
    EXPECT_EQ(diag.lastError(), nullptr);  // No errors yet

    diag.error(oreo::ErrorCode::OCCT_FAILURE, "First error");
    diag.error(oreo::ErrorCode::BOOLEAN_FAILED, "Second error");

    auto* last = diag.lastError();
    ASSERT_NE(last, nullptr);
    EXPECT_EQ(last->code, oreo::ErrorCode::BOOLEAN_FAILED);
}

// ═══════════════════════════════════════════════════════════════
// 3. UnitSystem — Units model with conversions
// ═══════════════════════════════════════════════════════════════

TEST(Units, MillimeterIdentity) {
    oreo::UnitSystem units;
    units.documentLength = oreo::LengthUnit::Millimeter;
    EXPECT_DOUBLE_EQ(units.toKernelLength(10.0), 10.0);
    EXPECT_DOUBLE_EQ(units.fromKernelLength(10.0), 10.0);
}

TEST(Units, InchToMillimeter) {
    oreo::UnitSystem units;
    units.documentLength = oreo::LengthUnit::Inch;
    EXPECT_NEAR(units.toKernelLength(1.0), 25.4, 1e-10);
    EXPECT_NEAR(units.fromKernelLength(25.4), 1.0, 1e-10);
}

TEST(Units, MeterToMillimeter) {
    oreo::UnitSystem units;
    units.documentLength = oreo::LengthUnit::Meter;
    EXPECT_NEAR(units.toKernelLength(1.0), 1000.0, 1e-10);
    EXPECT_NEAR(units.fromKernelLength(1000.0), 1.0, 1e-10);
}

TEST(Units, DegreeToRadian) {
    oreo::UnitSystem units;
    units.documentAngle = oreo::AngleUnit::Degree;
    EXPECT_NEAR(units.toKernelAngle(180.0), M_PI, 1e-10);
    EXPECT_NEAR(units.fromKernelAngle(M_PI), 180.0, 1e-10);
}

TEST(Units, RadianIdentity) {
    oreo::UnitSystem units;
    units.documentAngle = oreo::AngleUnit::Radian;
    EXPECT_DOUBLE_EQ(units.toKernelAngle(1.0), 1.0);
    EXPECT_DOUBLE_EQ(units.fromKernelAngle(1.0), 1.0);
}

TEST(Units, LengthConversions) {
    using namespace oreo::unit_convert;
    EXPECT_NEAR(convertLength(1.0, oreo::LengthUnit::Foot, oreo::LengthUnit::Inch), 12.0, 1e-10);
    EXPECT_NEAR(convertLength(1.0, oreo::LengthUnit::Meter, oreo::LengthUnit::Centimeter), 100.0, 1e-10);
    EXPECT_NEAR(convertLength(1000.0, oreo::LengthUnit::Micrometer, oreo::LengthUnit::Millimeter), 1.0, 1e-10);
}

TEST(Units, UnitNameParsing) {
    using namespace oreo::unit_convert;
    EXPECT_EQ(parseLengthUnit("mm"), oreo::LengthUnit::Millimeter);
    EXPECT_EQ(parseLengthUnit("inch"), oreo::LengthUnit::Inch);
    EXPECT_EQ(parseLengthUnit("METER"), oreo::LengthUnit::Meter);
    EXPECT_EQ(parseAngleUnit("deg"), oreo::AngleUnit::Degree);
    EXPECT_EQ(parseAngleUnit("radian"), oreo::AngleUnit::Radian);
}

// ═══════════════════════════════════════════════════════════════
// 4. SchemaVersion — Versioned persistence
// ═══════════════════════════════════════════════════════════════

TEST(Schema, VersionCompatibility) {
    oreo::SchemaVersion v1_0{1, 0, 0};
    oreo::SchemaVersion v1_1{1, 1, 0};
    oreo::SchemaVersion v2_0{2, 0, 0};

    EXPECT_TRUE(v1_1.canLoad(v1_0));   // 1.1 can load 1.0
    EXPECT_FALSE(v1_0.canLoad(v1_1));  // 1.0 cannot load 1.1
    EXPECT_FALSE(v1_1.canLoad(v2_0));  // Different major
    EXPECT_FALSE(v2_0.canLoad(v1_0));  // Different major
}

TEST(Schema, VersionParse) {
    auto v = oreo::SchemaVersion::parse("2.3.7");
    EXPECT_EQ(v.major, 2);
    EXPECT_EQ(v.minor, 3);
    EXPECT_EQ(v.patch, 7);
}

TEST(Schema, VersionToString) {
    oreo::SchemaVersion v{1, 2, 3};
    EXPECT_EQ(v.toString(), "1.2.3");
}

TEST(Schema, VersionJSON) {
    oreo::SchemaVersion v{3, 1, 4};
    auto j = v.toJSON();
    auto restored = oreo::SchemaVersion::fromJSON(j);
    EXPECT_EQ(restored, v);
}

TEST(Schema, RegistryCurrentVersion) {
    oreo::SchemaRegistry reg;
    auto v = reg.currentVersion(oreo::schema::TYPE_FEATURE_TREE);
    EXPECT_EQ(v, oreo::schema::FEATURE_TREE);
}

TEST(Schema, AddHeader) {
    nlohmann::json data = {{"features", nlohmann::json::array()}};
    auto withHeader = oreo::SchemaRegistry::addHeader(
        oreo::schema::TYPE_FEATURE_TREE, oreo::schema::FEATURE_TREE, data);

    EXPECT_TRUE(withHeader.contains("_schema"));
    EXPECT_TRUE(withHeader.contains("_version"));
    EXPECT_TRUE(withHeader.contains("_kernelVersion"));
    EXPECT_EQ(withHeader["_schema"], oreo::schema::TYPE_FEATURE_TREE);
}

TEST(Schema, ReadHeader) {
    nlohmann::json data = {
        {"_schema", "oreo.feature_tree"},
        {"_version", {{"major", 1}, {"minor", 0}, {"patch", 0}}},
        {"features", nlohmann::json::array()}
    };
    auto header = oreo::SchemaRegistry::readHeader(data);
    EXPECT_TRUE(header.valid);
    EXPECT_EQ(header.type, "oreo.feature_tree");
    EXPECT_EQ(header.version.major, 1);
}

TEST(Schema, ReadHeaderMissing) {
    nlohmann::json data = {{"features", nlohmann::json::array()}};
    auto header = oreo::SchemaRegistry::readHeader(data);
    EXPECT_FALSE(header.valid);
}

// ═══════════════════════════════════════════════════════════════
// 5. Validation — Strict input validation
// ═══════════════════════════════════════════════════════════════

TEST(Validation, RequireNonNull) {
    auto ctx = oreo::KernelContext::create();
    oreo::NamedShape null;
    EXPECT_FALSE(oreo::validation::requireNonNull(*ctx, null, "shape"));
    EXPECT_TRUE(ctx->diag.hasErrors());

    ctx->diag.clear();
    TopoDS_Shape box = BRepPrimAPI_MakeBox(10, 10, 10).Shape();
    oreo::NamedShape valid(box, 1);
    EXPECT_TRUE(oreo::validation::requireNonNull(*ctx, valid, "shape"));
    EXPECT_FALSE(ctx->diag.hasErrors());
}

TEST(Validation, RequirePositive) {
    auto ctx = oreo::KernelContext::create();
    EXPECT_FALSE(oreo::validation::requirePositive(*ctx, -5.0, "radius"));
    EXPECT_TRUE(ctx->diag.hasErrors());

    ctx->diag.clear();
    EXPECT_FALSE(oreo::validation::requirePositive(*ctx, 0.0, "radius"));
    EXPECT_TRUE(ctx->diag.hasErrors());

    ctx->diag.clear();
    EXPECT_TRUE(oreo::validation::requirePositive(*ctx, 5.0, "radius"));
    EXPECT_FALSE(ctx->diag.hasErrors());
}

TEST(Validation, RequireInRange) {
    auto ctx = oreo::KernelContext::create();
    EXPECT_TRUE(oreo::validation::requireInRange(*ctx, 5.0, 0.0, 10.0, "angle"));
    EXPECT_FALSE(oreo::validation::requireInRange(*ctx, 15.0, 0.0, 10.0, "angle"));
}

TEST(Validation, RequireNonZeroVec) {
    auto ctx = oreo::KernelContext::create();
    EXPECT_FALSE(oreo::validation::requireNonZeroVec(*ctx, gp_Vec(0, 0, 0), "direction"));
    EXPECT_TRUE(ctx->diag.hasErrors());

    ctx->diag.clear();
    EXPECT_TRUE(oreo::validation::requireNonZeroVec(*ctx, gp_Vec(1, 0, 0), "direction"));
    EXPECT_FALSE(ctx->diag.hasErrors());
}

// ═══════════════════════════════════════════════════════════════
// 6. KernelContext — Integration tests
// ═══════════════════════════════════════════════════════════════

TEST(KernelContext, CreateWithDefaults) {
    auto ctx = oreo::KernelContext::create();
    ASSERT_NE(ctx, nullptr);
    EXPECT_FALSE(ctx->id().empty());
    EXPECT_TRUE(ctx->tags.empty());
    EXPECT_TRUE(ctx->diag.empty());
}

TEST(KernelContext, CreateWithConfig) {
    oreo::KernelConfig config;
    config.tolerance.linearPrecision = 1e-6;
    config.units.documentLength = oreo::LengthUnit::Inch;
    config.tagSeed = 500;

    auto ctx = oreo::KernelContext::create(config);
    EXPECT_NEAR(ctx->tolerance.linearPrecision, 1e-6, 1e-12);
    EXPECT_EQ(ctx->units.documentLength, oreo::LengthUnit::Inch);
    EXPECT_EQ(ctx->tags.nextTag(), 501);
}

TEST(KernelContext, TwoContextsIndependent) {
    auto ctx1 = oreo::KernelContext::create();
    auto ctx2 = oreo::KernelContext::create();

    ctx1->tags.nextTag();
    ctx1->tags.nextTag();
    ctx1->diag.error(oreo::ErrorCode::OCCT_FAILURE, "Error in ctx1");

    // ctx2 is completely independent
    EXPECT_TRUE(ctx2->tags.empty());
    EXPECT_TRUE(ctx2->diag.empty());
}

TEST(KernelContext, BeginOperation) {
    auto ctx = oreo::KernelContext::create();
    ctx->diag.error(oreo::ErrorCode::OCCT_FAILURE, "Some old error");
    EXPECT_TRUE(ctx->diag.hasErrors());

    ctx->beginOperation();
    EXPECT_TRUE(ctx->diag.empty());
    EXPECT_TRUE(ctx->lastOperationOK());
}

TEST(KernelContext, CreateContextHasId) {
    auto ctx = oreo::KernelContext::create();
    EXPECT_FALSE(ctx->id().empty());
}

// ═══════════════════════════════════════════════════════════════
// 7. Deterministic Tag Allocation — Cross-context isolation
// ═══════════════════════════════════════════════════════════════

TEST(Determinism, SameOperationsSameTags) {
    // Two contexts with same config → same tag sequences
    for (int iter = 0; iter < 5; ++iter) {
        auto ctx = oreo::KernelContext::create();
        EXPECT_EQ(ctx->tags.nextTag(), 1);
        EXPECT_EQ(ctx->tags.nextTag(), 2);
        EXPECT_EQ(ctx->tags.nextTag(), 3);
    }
}

TEST(Determinism, ResetAndReplayIdentical) {
    auto ctx = oreo::KernelContext::create();

    // First run
    long t1 = ctx->tags.nextTag();
    long t2 = ctx->tags.nextTag();
    long t3 = ctx->tags.nextTag();

    // Reset and replay
    ctx->tags.reset();
    EXPECT_EQ(ctx->tags.nextTag(), t1);
    EXPECT_EQ(ctx->tags.nextTag(), t2);
    EXPECT_EQ(ctx->tags.nextTag(), t3);
}

TEST(Determinism, TwoDocumentsNeverCollide) {
    auto doc1 = oreo::KernelContext::create();
    auto doc2 = oreo::KernelContext::create();

    // Both get tag 1, 2, 3 — independent spaces, no collision
    EXPECT_EQ(doc1->tags.nextTag(), 1);
    EXPECT_EQ(doc2->tags.nextTag(), 1);

    // Each context's tags are isolated
    EXPECT_EQ(doc1->tags.currentValue(), 1);
    EXPECT_EQ(doc2->tags.currentValue(), 1);
}
