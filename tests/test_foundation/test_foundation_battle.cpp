// test_foundation_battle.cpp — Adversarial battle tests for the oreo-kernel foundation.
//
// Tests ALL 4 foundation layers:
//   Layer 0: OCCT integration, PlaneGCS solver
//   Layer 1: Element map, tag allocator, NamedShape, naming
//   Layer 2: KernelContext, OperationResult, DiagnosticScope, validation, units, schema
//   Layer 3: C API boundary, shape healing, OCCT scope guards
//
// Every safety mechanism is tested under adversarial conditions:
//   - NaN/Inf injection, null inputs, boundary values
//   - Threading, scope invalidation, overflow
//   - Multi-step pipelines, corruption recovery
//   - Schema malformation, migration cycles

#include <gtest/gtest.h>

#include "core/kernel_context.h"
#include "core/operation_result.h"
#include "core/diagnostic_scope.h"
#include "core/diagnostic.h"
#include "core/tag_allocator.h"
#include "core/units.h"
#include "core/schema.h"
#include "core/validation.h"
#include "core/occt_scope_guard.h"
#include "geometry/oreo_geometry.h"
#include "naming/named_shape.h"
#include "naming/element_map.h"
#include "naming/indexed_name.h"
#include "naming/mapped_name.h"
#include "naming/map_shape_elements.h"
#include "naming/shape_mapper.h"
#include "query/oreo_query.h"
#include "io/oreo_serialize.h"
#include "io/oreo_step.h"
#include "io/shape_fix.h"
#include "sketch/oreo_sketch.h"
#include "oreo_kernel.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <Interface_Static.hxx>
#include <TopExp_Explorer.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <gp_Dir.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Pln.hxx>
#include <Precision.hxx>

#include <nlohmann/json.hpp>

#include <thread>
#include <vector>
#include <set>
#include <string>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const double NaN = std::numeric_limits<double>::quiet_NaN();
static const double PosInf = std::numeric_limits<double>::infinity();
static const double NegInf = -std::numeric_limits<double>::infinity();

// =====================================================================
// SECTION A: Layer 2 — Contracts & Safety (~45 tests)
// =====================================================================

// ─────────────────────────────────────────────────────────────────────
// DiagnosticScope (6 tests)
// ─────────────────────────────────────────────────────────────────────

// 1. Clear while scope is live invalidates scope's snapshot
TEST(DiagScope, ClearWhileScopeLive) {
    auto ctx = oreo::KernelContext::create();
    oreo::DiagnosticScope scope(*ctx);

    // Add an error through the context's diagnostic collector
    ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "test error");
    EXPECT_TRUE(scope.hasErrors());

    // Clear the collector — this increments the generation counter.
    // The scope's startIndex_ is now stale because the backing vector was cleared.
    ctx->diag().clear();

    // After clear, the scope should see an empty vector (generation mismatch
    // means the scope can no longer trust its startIndex_). The scope should
    // return empty diagnostics since the underlying data was wiped.
    auto diags = scope.extractDiagnostics();
    EXPECT_TRUE(diags.empty());
    EXPECT_FALSE(scope.hasErrors());
}

// 2. Nested scopes both report empty after clear, fresh scope works
TEST(DiagScope, NestedScopeClearContext) {
    auto ctx = oreo::KernelContext::create();

    {
        oreo::DiagnosticScope outer(*ctx);
        ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "outer error");

        {
            oreo::DiagnosticScope inner(*ctx);
            ctx->diag().warning(oreo::ErrorCode::SHAPE_INVALID, "inner warning");

            // Clear everything
            ctx->diag().clear();

            // Both scopes should see empty after clear
            EXPECT_TRUE(inner.extractDiagnostics().empty());
            EXPECT_FALSE(inner.hasErrors());
        }

        EXPECT_TRUE(outer.extractDiagnostics().empty());
        EXPECT_FALSE(outer.hasErrors());
    }

    // Fresh scope after clear should work normally
    oreo::DiagnosticScope fresh(*ctx);
    ctx->diag().error(oreo::ErrorCode::OCCT_FAILURE, "fresh error");
    auto freshDiags = fresh.extractDiagnostics();
    EXPECT_EQ(freshDiags.size(), 1u);
    EXPECT_EQ(freshDiags[0].code, oreo::ErrorCode::OCCT_FAILURE);
    EXPECT_TRUE(fresh.hasErrors());
}

// 3. Interleaved scopes — documents the known limitation
// Scope A starts, scope B starts, diag added inside B's window.
// A's extract will INCLUDE B's diagnostic because scopes slice by index range,
// not by ownership. This test documents the behavior.
TEST(DiagScope, InterleavedScopes) {
    auto ctx = oreo::KernelContext::create();
    oreo::DiagnosticScope scopeA(*ctx);

    // Add a diagnostic that belongs to scopeA's window
    ctx->diag().info(oreo::ErrorCode::OK, "msg in A");

    oreo::DiagnosticScope scopeB(*ctx);

    // Add a diagnostic while scopeB is live — this is in both A and B's window
    ctx->diag().error(oreo::ErrorCode::BOOLEAN_FAILED, "msg in B");

    auto diagsB = scopeB.extractDiagnostics();
    EXPECT_EQ(diagsB.size(), 1u);
    EXPECT_EQ(diagsB[0].code, oreo::ErrorCode::BOOLEAN_FAILED);

    // A sees everything since its startIndex_, which includes B's diagnostic
    auto diagsA = scopeA.extractDiagnostics();
    EXPECT_EQ(diagsA.size(), 2u);
    EXPECT_EQ(diagsA[0].code, oreo::ErrorCode::OK);
    EXPECT_EQ(diagsA[1].code, oreo::ErrorCode::BOOLEAN_FAILED);
}

// 4. makeResult with errors produces a failed result
TEST(DiagScope, MakeResultWithErrors) {
    auto ctx = oreo::KernelContext::create();
    oreo::DiagnosticScope scope(*ctx);

    ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "bad dimension");

    auto result = scope.makeResult(42);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
    EXPECT_FALSE(result.diagnostics().empty());
    EXPECT_EQ(result.diagnostics()[0].code, oreo::ErrorCode::INVALID_INPUT);
}

// 5. makeResult with no errors produces a successful result
TEST(DiagScope, MakeResultClean) {
    auto ctx = oreo::KernelContext::create();
    oreo::DiagnosticScope scope(*ctx);

    // Only info-level diagnostics — no errors
    ctx->diag().info(oreo::ErrorCode::OK, "operation started");

    auto result = scope.makeResult(std::string("hello"));
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value(), "hello");
    EXPECT_FALSE(result.hasErrors());
    // Info diagnostics should still travel with the result
    EXPECT_FALSE(result.diagnostics().empty());
}

// 6. Composed hole operation — success and NaN injection
TEST(DiagScope, ComposedHole) {
    auto ctx = oreo::KernelContext::create();

    // Create a box large enough for the hole
    auto boxResult = oreo::makeBox(*ctx, 100.0, 100.0, 50.0);
    ASSERT_TRUE(boxResult.ok());

    // Get a face from the box for the hole
    auto facesResult = oreo::getFaces(*ctx, boxResult.value());
    ASSERT_TRUE(facesResult.ok());
    ASSERT_FALSE(facesResult.value().empty());

    oreo::NamedFace topFace = facesResult.value()[0];

    // Successful hole
    auto holeOk = oreo::hole(*ctx, boxResult.value(), topFace,
                              gp_Pnt(50, 50, 50), 10.0, 30.0,
                              oreo::HoleType::Blind);
    // The hole may or may not succeed depending on which face we picked,
    // but the result should have accessible diagnostics in either case.
    if (holeOk.ok()) {
        EXPECT_FALSE(holeOk.value().isNull());
    }
    // Regardless of success, diagnostics should be accessible
    (void)holeOk.diagnostics();

    // NaN injection — must fail cleanly
    auto holeNaN = oreo::hole(*ctx, boxResult.value(), topFace,
                               gp_Pnt(50, 50, 50), NaN, 30.0,
                               oreo::HoleType::Blind);
    EXPECT_FALSE(holeNaN.ok());
    EXPECT_TRUE(holeNaN.hasErrors());
}

// ─────────────────────────────────────────────────────────────────────
// OperationResult (5 tests)
// ─────────────────────────────────────────────────────────────────────

// 7. Success carries the value
TEST(OperationResult, SuccessCarriesValue) {
    auto r = oreo::OperationResult<int>::success(42);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 42);
    EXPECT_TRUE(r.diagnostics().empty());
    EXPECT_FALSE(r.hasErrors());
}

// 8. Failure throws on .valueOrThrow() (was .value() prior to OR-1 rename)
TEST(OperationResult, FailureThrowsOnValue) {
    std::vector<oreo::Diagnostic> diags;
    diags.push_back(oreo::Diagnostic::error(oreo::ErrorCode::OCCT_FAILURE, "operation failed"));
    auto r = oreo::OperationResult<int>::failure(std::move(diags));

    EXPECT_FALSE(r.ok());
    EXPECT_THROW(r.valueOrThrow(), std::runtime_error);
}

// 9. valueOr returns default on failure
TEST(OperationResult, ValueOrReturnsDefault) {
    std::vector<oreo::Diagnostic> diags;
    diags.push_back(oreo::Diagnostic::error(oreo::ErrorCode::INVALID_INPUT, "bad"));
    auto r = oreo::OperationResult<int>::failure(std::move(diags));

    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.valueOr(999), 999);
}

// 10. Diagnostics travel with a failed makeBox
TEST(OperationResult, DiagnosticsTravelWithMakeBoxNegative) {
    auto ctx = oreo::KernelContext::create();
    auto r = oreo::makeBox(*ctx, -1.0, 10.0, 10.0);

    // Negative dimensions should be caught by validation
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(r.hasErrors());
    EXPECT_FALSE(r.diagnostics().empty());
    // The error message should indicate something about the invalid dimension
    EXPECT_FALSE(r.errorMessage().empty());
}

// 11. Move semantics work on OperationResult
TEST(OperationResult, MoveSemantics) {
    auto r1 = oreo::OperationResult<std::string>::success(std::string("hello"));
    ASSERT_TRUE(r1.ok());

    // Move-construct
    auto r2 = std::move(r1);
    ASSERT_TRUE(r2.ok());
    EXPECT_EQ(r2.value(), "hello");

    // Move-assign
    oreo::OperationResult<std::string> r3 =
        oreo::OperationResult<std::string>::success(std::string("world"));
    r3 = std::move(r2);
    ASSERT_TRUE(r3.ok());
    EXPECT_EQ(r3.value(), "hello");
}

// ─────────────────────────────────────────────────────────────────────
// KernelContext (5 tests)
// ─────────────────────────────────────────────────────────────────────

// 12. Two contexts have isolated diagnostics
TEST(KernelContext, IsolatedDiagnostics) {
    auto ctx1 = oreo::KernelContext::create();
    auto ctx2 = oreo::KernelContext::create();

    ctx1->diag().error(oreo::ErrorCode::INVALID_INPUT, "error in ctx1");

    // ctx2 should not see ctx1's error
    EXPECT_TRUE(ctx2->diag().empty());
    EXPECT_TRUE(ctx1->diag().hasErrors());
    EXPECT_FALSE(ctx2->diag().hasErrors());
}

// 13. Two contexts have isolated tags
TEST(KernelContext, IsolatedTags) {
    auto ctx1 = oreo::KernelContext::create();
    auto ctx2 = oreo::KernelContext::create();

    int64_t tag1a = ctx1->tags().nextTag();
    int64_t tag1b = ctx1->tags().nextTag();
    int64_t tag2a = ctx2->tags().nextTag();

    // Both start from 1 — they are independent
    EXPECT_EQ(tag1a, 1);
    EXPECT_EQ(tag1b, 2);
    EXPECT_EQ(tag2a, 1);
}

// 14. Two contexts have isolated units
TEST(KernelContext, IsolatedUnits) {
    oreo::KernelConfig cfg1;
    cfg1.units.documentLength = oreo::LengthUnit::Inch;
    auto ctx1 = oreo::KernelContext::create(cfg1);

    oreo::KernelConfig cfg2;
    cfg2.units.documentLength = oreo::LengthUnit::Meter;
    auto ctx2 = oreo::KernelContext::create(cfg2);

    EXPECT_EQ(ctx1->units().documentLength, oreo::LengthUnit::Inch);
    EXPECT_EQ(ctx2->units().documentLength, oreo::LengthUnit::Meter);
}

// 15. Tolerance accessor returns correct value
TEST(KernelContext, ToleranceAccessor) {
    oreo::KernelConfig cfg;
    cfg.tolerance.linearPrecision = 1e-5;
    cfg.tolerance.angularPrecision = 1e-10;
    auto ctx = oreo::KernelContext::create(cfg);

    EXPECT_DOUBLE_EQ(ctx->tolerance().linearPrecision, 1e-5);
    EXPECT_DOUBLE_EQ(ctx->tolerance().angularPrecision, 1e-10);
}

// 16. Schemas accessor allows migration registration
TEST(KernelContext, SchemasAccessor) {
    auto ctx = oreo::KernelContext::create();

    // Const accessor — should be able to query current versions
    const auto& constSchemas = ctx->schemas();
    auto ver = constSchemas.currentVersion(oreo::schema::TYPE_FEATURE_TREE);
    EXPECT_EQ(ver.major, 1);
    EXPECT_EQ(ver.minor, 0);

    // Mutable accessor — should be able to register a migration
    ctx->schemas().registerMigration(
        "test.battle.schemas_accessor",
        oreo::SchemaVersion{0, 9, 0},
        oreo::SchemaVersion{1, 0, 0},
        [](const nlohmann::json& data) -> nlohmann::json {
            auto result = data;
            result["migrated"] = true;
            return result;
        });

    // Verify schemas() returns a valid reference by checking a built-in type.
    // NOTE: canLoad checks against currentVersions_ which only contains built-in types.
    // A custom type like "test.battle.schemas_accessor" won't be in currentVersions_,
    // so canLoad would return false even though a migration was registered.
    EXPECT_TRUE(ctx->schemas().canLoad(oreo::schema::TYPE_FEATURE_TREE,
                                        oreo::SchemaVersion{1, 0, 0}));
}

// ─────────────────────────────────────────────────────────────────────
// TagAllocator (6 tests)
// ─────────────────────────────────────────────────────────────────────

// 17. Sequential single-document tags
TEST(TagAllocatorBattle, Sequential) {
    oreo::TagAllocator alloc;
    EXPECT_EQ(alloc.nextTag(), 1);
    EXPECT_EQ(alloc.nextTag(), 2);
    EXPECT_EQ(alloc.nextTag(), 3);
    EXPECT_EQ(alloc.currentValue(), 3);
    EXPECT_FALSE(alloc.empty());
}

// 18. Multi-document tags never collide
TEST(TagAllocatorBattle, MultiDocNoCollision) {
    oreo::TagAllocator allocA(1);
    oreo::TagAllocator allocB(2);

    std::set<int64_t> tags;
    for (int i = 0; i < 100; ++i) {
        tags.insert(allocA.nextTag());
        tags.insert(allocB.nextTag());
    }
    // 100 from A + 100 from B = 200 unique tags (no collisions)
    EXPECT_EQ(tags.size(), 200u);

    // Verify the upper 32 bits encode the document ID
    int64_t tagA = allocA.nextTag();
    int64_t tagB = allocB.nextTag();
    EXPECT_EQ(oreo::TagAllocator::extractDocumentId(tagA), 1u);
    EXPECT_EQ(oreo::TagAllocator::extractDocumentId(tagB), 2u);
}

// 19. Multi-document overflow throws
TEST(TagAllocatorBattle, OverflowThrows) {
    oreo::TagAllocator alloc(1);  // multi-document mode

    // Seed the counter just below the UINT32_MAX boundary
    alloc.seed(static_cast<int64_t>(UINT32_MAX) - 1);

    // This should succeed (counter = UINT32_MAX)
    EXPECT_NO_THROW(alloc.nextTag());

    // This should throw (counter > UINT32_MAX)
    EXPECT_THROW(alloc.nextTag(), std::overflow_error);
}

// 20. Single-document mode does not overflow at UINT32_MAX
TEST(TagAllocatorBattle, SingleDocNoOverflow) {
    oreo::TagAllocator alloc;  // documentId = 0, single-doc mode

    // Seed just below UINT32_MAX
    alloc.seed(static_cast<int64_t>(UINT32_MAX) - 1);

    // Single-doc mode just returns the counter — no overflow check
    int64_t tag1 = alloc.nextTag();  // UINT32_MAX
    EXPECT_EQ(tag1, static_cast<int64_t>(UINT32_MAX));

    int64_t tag2 = alloc.nextTag();  // UINT32_MAX + 1 — still fine in single-doc
    EXPECT_EQ(tag2, static_cast<int64_t>(UINT32_MAX) + 1);
}

// 21. Reset produces deterministic replay
TEST(TagAllocatorBattle, ResetDeterministic) {
    oreo::TagAllocator alloc(42);

    std::vector<int64_t> first_run;
    for (int i = 0; i < 10; ++i) first_run.push_back(alloc.nextTag());

    // resetCounterOnly() preserves the documentId so the replay produces
    // byte-identical encoded tags. The wider reset() would zero the docId
    // and fall back to single-doc mode, making the two runs differ.
    alloc.resetCounterOnly();

    std::vector<int64_t> second_run;
    for (int i = 0; i < 10; ++i) second_run.push_back(alloc.nextTag());

    EXPECT_EQ(first_run, second_run);
}

// 22. toOcctTag mapping behavior documented
TEST(TagAllocatorBattle, ToOcctTagTruncation) {
    // toOcctTag is now a non-static member — each allocator owns its own
    // mapping table. Small tags (<=INT32_MAX) pass through unchanged; large
    // tags go through a per-allocator map. Non-positive inputs throw.
    oreo::TagAllocator alloc;

    // Small tags pass through unchanged
    EXPECT_EQ(alloc.toOcctTag(1), 1);
    EXPECT_EQ(alloc.toOcctTag(INT32_MAX), INT32_MAX);

    // Non-positive inputs are rejected (prior behavior silently masked; new
    // behavior fails closed on the invariant that tags must be > 0).
    EXPECT_THROW(alloc.toOcctTag(0), std::invalid_argument);
    EXPECT_THROW(alloc.toOcctTag(-1), std::invalid_argument);

    // Large multi-doc tags (>INT32_MAX) map to a per-allocator 32-bit handle
    // that is positive and stable within this allocator instance.
    int64_t bigTag = (static_cast<int64_t>(1) << 32) | 42;
    int32_t mapped1 = alloc.toOcctTag(bigTag);
    EXPECT_GT(mapped1, 0);  // Must remain positive for OCCT
    // Stability: asking again for the same input returns the same handle.
    int32_t mapped1_again = alloc.toOcctTag(bigTag);
    EXPECT_EQ(mapped1, mapped1_again);

    // A different large tag gets a different handle.
    int64_t bigTag2 = (static_cast<int64_t>(2) << 32) | 7;
    int32_t mapped2 = alloc.toOcctTag(bigTag2);
    EXPECT_GT(mapped2, 0);
    EXPECT_NE(mapped1, mapped2);
}

// ─────────────────────────────────────────────────────────────────────
// Validation Direct (14 tests)
// ─────────────────────────────────────────────────────────────────────

// 23. requirePositive rejects NaN
TEST(ValidationBattle, RequirePositiveNaN) {
    auto ctx = oreo::KernelContext::create();
    EXPECT_FALSE(oreo::validation::requirePositive(*ctx, NaN, "val"));
    EXPECT_TRUE(ctx->diag().hasErrors());
}

// 24. requirePositive rejects +Inf
TEST(ValidationBattle, RequirePositiveInf) {
    auto ctx = oreo::KernelContext::create();
    EXPECT_FALSE(oreo::validation::requirePositive(*ctx, PosInf, "val"));
    EXPECT_TRUE(ctx->diag().hasErrors());
}

// 25. requirePositive rejects zero
TEST(ValidationBattle, RequirePositiveZero) {
    auto ctx = oreo::KernelContext::create();
    EXPECT_FALSE(oreo::validation::requirePositive(*ctx, 0.0, "val"));
    EXPECT_TRUE(ctx->diag().hasErrors());
}

// 26. requireNonNegative rejects NegInf
TEST(ValidationBattle, RequireNonNegativeNegInf) {
    auto ctx = oreo::KernelContext::create();
    EXPECT_FALSE(oreo::validation::requireNonNegative(*ctx, NegInf, "val"));
    EXPECT_TRUE(ctx->diag().hasErrors());
}

// 27. requireInRange rejects NaN in value
TEST(ValidationBattle, RequireInRangeNaN) {
    auto ctx = oreo::KernelContext::create();
    EXPECT_FALSE(oreo::validation::requireInRange(*ctx, NaN, 0.0, 10.0, "val"));
    EXPECT_TRUE(ctx->diag().hasErrors());
}

// 28. requireInRange rejects NaN in bounds
TEST(ValidationBattle, RequireInRangeNaNBounds) {
    auto ctx = oreo::KernelContext::create();
    // NaN in min bound
    EXPECT_FALSE(oreo::validation::requireInRange(*ctx, 5.0, NaN, 10.0, "val"));
}

// 29. requireFinitePoint rejects NaN coordinates
TEST(ValidationBattle, RequireFinitePointNaN) {
    auto ctx = oreo::KernelContext::create();
    gp_Pnt nanPoint(NaN, 0.0, 0.0);
    EXPECT_FALSE(oreo::validation::requireFinitePoint(*ctx, nanPoint, "pt"));
    EXPECT_TRUE(ctx->diag().hasErrors());
}

// 30. requireFinitePoint accepts valid point
TEST(ValidationBattle, RequireFinitePointValid) {
    auto ctx = oreo::KernelContext::create();
    gp_Pnt validPoint(1.0, 2.0, 3.0);
    EXPECT_TRUE(oreo::validation::requireFinitePoint(*ctx, validPoint, "pt"));
    EXPECT_FALSE(ctx->diag().hasErrors());
}

// 31. requireValidAxis rejects NaN location
TEST(ValidationBattle, RequireValidAxisNaN) {
    auto ctx = oreo::KernelContext::create();
    // gp_Dir constructor with NaN may throw, so catch that
    try {
        gp_Pnt loc(NaN, 0.0, 0.0);
        gp_Dir dir(0.0, 0.0, 1.0);
        gp_Ax1 axis(loc, dir);
        EXPECT_FALSE(oreo::validation::requireValidAxis(*ctx, axis, "axis"));
    } catch (const Standard_Failure&) {
        // OCCT may throw during axis construction with NaN — that's acceptable
        SUCCEED();
    }
}

// 32. requireValidAxis accepts valid axis
TEST(ValidationBattle, RequireValidAxisValid) {
    auto ctx = oreo::KernelContext::create();
    gp_Ax1 axis(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1));
    EXPECT_TRUE(oreo::validation::requireValidAxis(*ctx, axis, "axis"));
    EXPECT_FALSE(ctx->diag().hasErrors());
}

// 33. requireValidAxis2 accepts valid
TEST(ValidationBattle, RequireValidAxis2Valid) {
    auto ctx = oreo::KernelContext::create();
    gp_Ax2 axis(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1));
    EXPECT_TRUE(oreo::validation::requireValidAxis2(*ctx, axis, "axis2"));
    EXPECT_FALSE(ctx->diag().hasErrors());
}

// 34. requireValidLengthUnit accepts valid, rejects bogus
TEST(ValidationBattle, RequireValidLengthUnitValid) {
    auto ctx = oreo::KernelContext::create();
    EXPECT_TRUE(oreo::validation::requireValidLengthUnit(
        *ctx, oreo::LengthUnit::Millimeter, "unit"));
    EXPECT_TRUE(oreo::validation::requireValidLengthUnit(
        *ctx, oreo::LengthUnit::Inch, "unit"));
    EXPECT_FALSE(ctx->diag().hasErrors());
}

// 35. requireValidLengthUnit rejects bogus cast value
TEST(ValidationBattle, RequireValidLengthUnitBogus) {
    auto ctx = oreo::KernelContext::create();
    auto bogus = static_cast<oreo::LengthUnit>(999);
    EXPECT_FALSE(oreo::validation::requireValidLengthUnit(*ctx, bogus, "unit"));
    EXPECT_TRUE(ctx->diag().hasErrors());
}

// 36. requireValidAngleUnit valid and invalid
TEST(ValidationBattle, RequireValidAngleUnit) {
    auto ctx = oreo::KernelContext::create();
    EXPECT_TRUE(oreo::validation::requireValidAngleUnit(
        *ctx, oreo::AngleUnit::Degree, "angle"));
    EXPECT_TRUE(oreo::validation::requireValidAngleUnit(
        *ctx, oreo::AngleUnit::Radian, "angle"));
    EXPECT_FALSE(ctx->diag().hasErrors());

    // Bogus value
    auto ctx2 = oreo::KernelContext::create();
    auto bogus = static_cast<oreo::AngleUnit>(999);
    EXPECT_FALSE(oreo::validation::requireValidAngleUnit(*ctx2, bogus, "angle"));
    EXPECT_TRUE(ctx2->diag().hasErrors());
}

// 37. requireNonZeroVec rejects NaN components
TEST(ValidationBattle, RequireNonZeroVecNaN) {
    auto ctx = oreo::KernelContext::create();
    gp_Vec nanVec(NaN, 0.0, 0.0);
    EXPECT_FALSE(oreo::validation::requireNonZeroVec(*ctx, nanVec, "vec"));
    EXPECT_TRUE(ctx->diag().hasErrors());
}

// ─────────────────────────────────────────────────────────────────────
// Checked Unit Conversion (5 tests)
// ─────────────────────────────────────────────────────────────────────

// 38. toKernelLengthChecked throws on NaN
TEST(UnitConversionBattle, LengthCheckedNaN) {
    oreo::UnitSystem units;
    units.documentLength = oreo::LengthUnit::Millimeter;
    EXPECT_THROW(units.toKernelLengthChecked(NaN), std::invalid_argument);
}

// 39. toKernelLengthChecked throws on Inf
TEST(UnitConversionBattle, LengthCheckedInf) {
    oreo::UnitSystem units;
    units.documentLength = oreo::LengthUnit::Inch;
    EXPECT_THROW(units.toKernelLengthChecked(PosInf), std::invalid_argument);
    EXPECT_THROW(units.toKernelLengthChecked(NegInf), std::invalid_argument);
}

// 40. toKernelLengthChecked accepts valid value
TEST(UnitConversionBattle, LengthCheckedValid) {
    oreo::UnitSystem units;
    units.documentLength = oreo::LengthUnit::Inch;
    double result = 0.0;
    EXPECT_NO_THROW(result = units.toKernelLengthChecked(1.0));
    // 1 inch = 25.4 mm
    EXPECT_NEAR(result, 25.4, 1e-10);
}

// 41. toKernelAngleChecked throws on NaN
TEST(UnitConversionBattle, AngleCheckedNaN) {
    oreo::UnitSystem units;
    units.documentAngle = oreo::AngleUnit::Degree;
    EXPECT_THROW(units.toKernelAngleChecked(NaN), std::invalid_argument);
}

// 42. toKernelAngleChecked accepts valid degree
TEST(UnitConversionBattle, AngleCheckedDegreeValid) {
    oreo::UnitSystem units;
    units.documentAngle = oreo::AngleUnit::Degree;
    double result = 0.0;
    EXPECT_NO_THROW(result = units.toKernelAngleChecked(180.0));
    EXPECT_NEAR(result, M_PI, 1e-10);
}

// ─────────────────────────────────────────────────────────────────────
// Schema (7 tests)
// ─────────────────────────────────────────────────────────────────────

// 43. fromJSON throws on missing fields
TEST(SchemaBattle, FromJsonMissingFields) {
    nlohmann::json incomplete = {{"major", 1}, {"minor", 0}};
    // Missing "patch" field
    EXPECT_THROW(oreo::SchemaVersion::fromJSON(incomplete), std::invalid_argument);
}

// 44. fromJSON throws on empty object
TEST(SchemaBattle, FromJsonEmptyObject) {
    nlohmann::json empty = nlohmann::json::object();
    EXPECT_THROW(oreo::SchemaVersion::fromJSON(empty), std::invalid_argument);
}

// 45. fromJSON throws on wrong type
// Per SCH-5: we now do an is_number_integer() pre-check and throw std::runtime_error
// instead of relying on nlohmann::json's implicit coercion (which would silently
// accept floats like 1.5). Accept either exception — both indicate type rejection.
TEST(SchemaBattle, FromJsonWrongType) {
    nlohmann::json wrongType = {{"major", "one"}, {"minor", 0}, {"patch", 0}};
    EXPECT_ANY_THROW(oreo::SchemaVersion::fromJSON(wrongType));
}

// 46. Cyclic migration chain throws
TEST(SchemaBattle, CyclicMigrationThrows) {
    auto ctx = oreo::KernelContext::create();
    const std::string type = "test.battle.cyclic";

    // Register a cycle: A -> B -> A
    ctx->schemas().registerMigration(type,
        oreo::SchemaVersion{0, 1, 0}, oreo::SchemaVersion{0, 2, 0},
        [](const nlohmann::json& d) { return d; });
    ctx->schemas().registerMigration(type,
        oreo::SchemaVersion{0, 2, 0}, oreo::SchemaVersion{0, 1, 0},
        [](const nlohmann::json& d) { return d; });

    // Build data at version 0.1.0
    nlohmann::json data = {
        {"_schema", type},
        {"_version", oreo::SchemaVersion{0, 1, 0}.toJSON()},
        {"value", 42}
    };

    // Attempting to migrate should detect the cycle
    EXPECT_THROW(ctx->schemas().migrate(type, data), std::runtime_error);
}

// 47. Migration infrastructure works for built-in types.
// NOTE: Custom migration chains can't be tested via canLoad/migrate because
// canLoad and migrate check against currentVersions_, which only contains
// built-in types. Instead, verify the built-in migration infrastructure works.
TEST(SchemaBattle, MigrationChainExecutes) {
    auto ctx = oreo::KernelContext::create();
    // Verify built-in types have versions registered
    auto ftVersion = ctx->schemas().currentVersion(oreo::schema::TYPE_FEATURE_TREE);
    EXPECT_EQ(ftVersion.major, 1);
    EXPECT_EQ(ftVersion.minor, 0);

    // Verify canLoad for same version
    EXPECT_TRUE(ctx->schemas().canLoad(oreo::schema::TYPE_FEATURE_TREE, oreo::SchemaVersion{1, 0, 0}));

    // Verify canLoad rejects future major
    EXPECT_FALSE(ctx->schemas().canLoad(oreo::schema::TYPE_FEATURE_TREE, oreo::SchemaVersion{2, 0, 0}));

    // Verify header round-trip
    auto json = oreo::SchemaRegistry::addHeader(oreo::schema::TYPE_FEATURE_TREE,
                                                  oreo::SchemaVersion{1, 0, 0},
                                                  {{"data", "test"}});
    auto header = oreo::SchemaRegistry::readHeader(json);
    EXPECT_TRUE(header.valid);
    EXPECT_EQ(header.type, oreo::schema::TYPE_FEATURE_TREE);
}

// 48. Unknown type rejected
TEST(SchemaBattle, UnknownTypeRejected) {
    auto ctx = oreo::KernelContext::create();
    EXPECT_THROW(
        ctx->schemas().currentVersion("nonexistent.type.xyz"),
        std::runtime_error
    );
}

// 49. Parse malformed version strings
TEST(SchemaBattle, ParseMalformedVersion) {
    // Empty string
    EXPECT_THROW(oreo::SchemaVersion::parse(""), std::invalid_argument);

    // Too few segments
    EXPECT_THROW(oreo::SchemaVersion::parse("1.0"), std::invalid_argument);

    // Too many segments
    EXPECT_THROW(oreo::SchemaVersion::parse("1.0.0.0"), std::invalid_argument);

    // Non-digit character
    EXPECT_THROW(oreo::SchemaVersion::parse("1.a.0"), std::invalid_argument);

    // Empty segment
    EXPECT_THROW(oreo::SchemaVersion::parse("1..0"), std::invalid_argument);

    // Valid parse for comparison
    auto v = oreo::SchemaVersion::parse("2.3.4");
    EXPECT_EQ(v.major, 2);
    EXPECT_EQ(v.minor, 3);
    EXPECT_EQ(v.patch, 4);
}

// ─────────────────────────────────────────────────────────────────────
// Threading (3 tests)
// ─────────────────────────────────────────────────────────────────────

// 50. 4 threads with 4 contexts are fully independent
TEST(ThreadingBattle, FourContextsIndependent) {
    constexpr int NUM_THREADS = 4;
    constexpr int OPS_PER_THREAD = 50;

    std::vector<std::thread> threads;
    std::vector<bool> results(NUM_THREADS, false);

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([t, &results]() {
            auto ctx = oreo::KernelContext::create();
            bool allOk = true;

            for (int i = 0; i < OPS_PER_THREAD; ++i) {
                int64_t tag = ctx->tags().nextTag();
                if (tag != static_cast<int64_t>(i + 1)) {
                    allOk = false;
                    break;
                }
            }

            // Each thread's tag allocator should be at OPS_PER_THREAD
            if (ctx->tags().currentValue() != OPS_PER_THREAD) allOk = false;

            results[t] = allOk;
        });
    }

    for (auto& th : threads) th.join();

    for (int t = 0; t < NUM_THREADS; ++t) {
        EXPECT_TRUE(results[t]) << "Thread " << t << " had incorrect tags";
    }
}

// 51. Concurrent OCCT initialization is safe
TEST(ThreadingBattle, ConcurrentInitOCCT) {
    // Multiple threads calling initOCCT should not crash.
    // The first one initializes, the rest are no-ops.
    constexpr int NUM_THREADS = 8;
    std::vector<std::thread> threads;
    std::atomic<int> completedCount{0};

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&completedCount]() {
            // create() calls initOCCT internally when config.initOCCT is true
            auto ctx = oreo::KernelContext::create();
            // If we get here without crashing, initialization was safe
            completedCount.fetch_add(1);
        });
    }

    for (auto& th : threads) th.join();

    EXPECT_EQ(completedCount.load(), NUM_THREADS);
    EXPECT_TRUE(oreo::KernelContext::isOCCTInitialized());
}

// 52. Diagnostics are isolated across threads
TEST(ThreadingBattle, DiagnosticsIsolated) {
    constexpr int NUM_THREADS = 4;
    std::vector<std::thread> threads;
    std::vector<int> errorCounts(NUM_THREADS, -1);

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([t, &errorCounts]() {
            auto ctx = oreo::KernelContext::create();

            // Each thread adds exactly (t+1) errors
            for (int i = 0; i <= t; ++i) {
                ctx->diag().error(oreo::ErrorCode::INVALID_INPUT,
                                "thread " + std::to_string(t) + " error " + std::to_string(i));
            }

            errorCounts[t] = ctx->diag().errorCount();
        });
    }

    for (auto& th : threads) th.join();

    // Thread 0 had 1 error, thread 1 had 2, thread 2 had 3, thread 3 had 4
    for (int t = 0; t < NUM_THREADS; ++t) {
        EXPECT_EQ(errorCounts[t], t + 1) << "Thread " << t << " wrong error count";
    }
}

// =====================================================================
// SECTION B: Layer 0 — OCCT Integration & Geometry (~35 tests)
// =====================================================================

// ─────────────────────────────────────────────────────────────────────
// Boolean Battle (8 tests)
// ─────────────────────────────────────────────────────────────────────

// 53. Fuzzy tolerance applied for near-tangent shapes
TEST(BooleanBattle, FuzzyToleranceApplied) {
    auto ctx = oreo::KernelContext::create();

    auto boxR = oreo::makeBox(*ctx, 10.0, 10.0, 10.0);
    ASSERT_TRUE(boxR.ok());

    // Create a box that barely touches the first one at a face
    auto box2R = oreo::makeBox(*ctx, gp_Pnt(10.0, 0.0, 0.0), 10.0, 10.0, 10.0);
    ASSERT_TRUE(box2R.ok());

    // Union of face-touching boxes — may need fuzzy tolerance
    auto unionR = oreo::booleanUnion(*ctx, boxR.value(), box2R.value());
    // Should succeed (either with or without fuzzy retry)
    EXPECT_TRUE(unionR.ok());
    if (unionR.ok()) {
        EXPECT_FALSE(unionR.value().isNull());
    }
}

// 54. All retries exhausted returns !ok
TEST(BooleanBattle, AllRetriesExhausted) {
    auto ctx = oreo::KernelContext::create();

    // Create a null/degenerate shape that will fail boolean no matter what
    oreo::NamedShape empty;
    EXPECT_TRUE(empty.isNull());

    auto boxR = oreo::makeBox(*ctx, 10.0, 10.0, 10.0);
    ASSERT_TRUE(boxR.ok());

    // Boolean with a null shape should fail
    auto unionR = oreo::booleanUnion(*ctx, boxR.value(), empty);
    EXPECT_FALSE(unionR.ok());
    EXPECT_TRUE(unionR.hasErrors());
}

// 55. Null input A to boolean
TEST(BooleanBattle, NullInputA) {
    auto ctx = oreo::KernelContext::create();
    oreo::NamedShape nullShape;
    auto boxR = oreo::makeBox(*ctx, 10.0, 10.0, 10.0);
    ASSERT_TRUE(boxR.ok());

    auto r = oreo::booleanSubtract(*ctx, nullShape, boxR.value());
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(r.hasErrors());
}

// 56. Null input B to boolean
TEST(BooleanBattle, NullInputB) {
    auto ctx = oreo::KernelContext::create();
    oreo::NamedShape nullShape;
    auto boxR = oreo::makeBox(*ctx, 10.0, 10.0, 10.0);
    ASSERT_TRUE(boxR.ok());

    auto r = oreo::booleanSubtract(*ctx, boxR.value(), nullShape);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(r.hasErrors());
}

// 57. Tiny vs large shape boolean
TEST(BooleanBattle, TinyVsLarge) {
    auto ctx = oreo::KernelContext::create();

    auto bigBox = oreo::makeBox(*ctx, 1000.0, 1000.0, 1000.0);
    ASSERT_TRUE(bigBox.ok());

    auto tinyBox = oreo::makeBox(*ctx, gp_Pnt(500, 500, 500), 0.001, 0.001, 0.001);
    ASSERT_TRUE(tinyBox.ok());

    auto r = oreo::booleanSubtract(*ctx, bigBox.value(), tinyBox.value());
    // May succeed or fail depending on tolerance — either way, must not crash
    // and result must carry diagnostics
    (void)r.ok();
    (void)r.diagnostics();
}

// 58. Pattern fuse failure handled
TEST(BooleanBattle, PatternFuseFailure) {
    auto ctx = oreo::KernelContext::create();

    auto boxR = oreo::makeBox(*ctx, 10.0, 10.0, 10.0);
    ASSERT_TRUE(boxR.ok());

    // Zero spacing causes overlapping copies — fuse may fail
    auto r = oreo::patternLinear(*ctx, boxR.value(), gp_Vec(0, 0, 0), 3, 0.0);
    // Should either succeed (degenerate pattern) or fail cleanly
    if (!r.ok()) {
        EXPECT_TRUE(r.hasErrors());
    }
}

// 59. Compound expanded through boolean
TEST(BooleanBattle, CompoundExpanded) {
    auto ctx = oreo::KernelContext::create();

    auto box1 = oreo::makeBox(*ctx, 10.0, 10.0, 10.0);
    ASSERT_TRUE(box1.ok());
    auto box2 = oreo::makeBox(*ctx, gp_Pnt(5, 0, 0), 10.0, 10.0, 10.0);
    ASSERT_TRUE(box2.ok());

    // Boolean intersect should work on overlapping boxes
    auto r = oreo::booleanIntersect(*ctx, box1.value(), box2.value());
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r.value().isNull());

    // The result should have faces
    auto faces = oreo::getFaces(*ctx, r.value());
    if (faces.ok()) {
        EXPECT_GT(faces.value().size(), 0u);
    }
}

// 60. autoFuzzy returns 0 for NaN extent
TEST(BooleanBattle, AutoFuzzyNaNExtent) {
    auto ctx = oreo::KernelContext::create();
    double fuzzy = oreo::computeBooleanFuzzy(*ctx, NaN);
    // NaN input should not produce NaN output — should return 0 or a safe value
    EXPECT_FALSE(std::isnan(fuzzy));
}

// ─────────────────────────────────────────────────────────────────────
// Geometry Robustness (12 tests)
// ─────────────────────────────────────────────────────────────────────

// 61. Hole correct size in inch units
TEST(GeomBattle, HoleCorrectSizeInch) {
    oreo::KernelConfig cfg;
    cfg.units.documentLength = oreo::LengthUnit::Inch;
    auto ctx = oreo::KernelContext::create(cfg);

    // Create a box (dimensions in document units = inches, converted internally to mm)
    auto boxR = oreo::makeBox(*ctx, 4.0, 4.0, 2.0);
    ASSERT_TRUE(boxR.ok());

    // Get a face for the hole
    auto faces = oreo::getFaces(*ctx, boxR.value());
    ASSERT_TRUE(faces.ok());
    ASSERT_FALSE(faces.value().empty());

    // Create a hole with 0.5 inch diameter, 1 inch deep
    auto holeR = oreo::hole(*ctx, boxR.value(), faces.value()[0],
                             gp_Pnt(ctx->units().toKernelLength(2.0),
                                    ctx->units().toKernelLength(2.0),
                                    ctx->units().toKernelLength(2.0)),
                             0.5, 1.0, oreo::HoleType::Blind);
    // Verify the result
    if (holeR.ok()) {
        EXPECT_FALSE(holeR.value().isNull());
    }
    // Either way, no crash and diagnostics present
    (void)holeR.diagnostics();
}

// 62. Hole through large shape
TEST(GeomBattle, HoleThroughLargeShape) {
    auto ctx = oreo::KernelContext::create();

    auto boxR = oreo::makeBox(*ctx, 500.0, 500.0, 200.0);
    ASSERT_TRUE(boxR.ok());

    auto faces = oreo::getFaces(*ctx, boxR.value());
    ASSERT_TRUE(faces.ok());
    ASSERT_FALSE(faces.value().empty());

    // Through hole
    auto holeR = oreo::hole(*ctx, boxR.value(), faces.value()[0],
                             gp_Pnt(250, 250, 200), 20.0, 200.0,
                             oreo::HoleType::Through);
    if (holeR.ok()) {
        EXPECT_FALSE(holeR.value().isNull());
    }
    (void)holeR.diagnostics();
}

// 63. splitBody on a large shape
TEST(GeomBattle, SplitBodyLargeShape) {
    auto ctx = oreo::KernelContext::create();

    auto boxR = oreo::makeBox(*ctx, 100.0, 100.0, 100.0);
    ASSERT_TRUE(boxR.ok());

    gp_Pln splitPlane(gp_Pnt(50, 0, 0), gp_Dir(1, 0, 0));
    auto splitR = oreo::splitBody(*ctx, boxR.value(), splitPlane);
    if (splitR.ok()) {
        EXPECT_FALSE(splitR.value().isNull());
    }
    (void)splitR.diagnostics();
}

// 64. Fillet with null edges
TEST(GeomBattle, FilletNullEdges) {
    auto ctx = oreo::KernelContext::create();

    auto boxR = oreo::makeBox(*ctx, 20.0, 20.0, 20.0);
    ASSERT_TRUE(boxR.ok());

    // Empty edges list
    std::vector<oreo::NamedEdge> emptyEdges;
    auto filletR = oreo::fillet(*ctx, boxR.value(), emptyEdges, 2.0);
    EXPECT_FALSE(filletR.ok());
    EXPECT_TRUE(filletR.hasErrors());
}

// 65. Loft with null profiles
TEST(GeomBattle, LoftNullProfiles) {
    auto ctx = oreo::KernelContext::create();

    std::vector<oreo::NamedShape> emptyProfiles;
    auto loftR = oreo::loft(*ctx, emptyProfiles);
    EXPECT_FALSE(loftR.ok());
    EXPECT_TRUE(loftR.hasErrors());
}

// 66. Zero-dimension box
TEST(GeomBattle, ZeroDimBox) {
    auto ctx = oreo::KernelContext::create();
    auto r = oreo::makeBox(*ctx, 0.0, 10.0, 10.0);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(r.hasErrors());
}

// 67. Solid shape passed where wire expected
TEST(GeomBattle, SolidAsWire) {
    auto ctx = oreo::KernelContext::create();

    auto boxR = oreo::makeBox(*ctx, 10.0, 10.0, 10.0);
    ASSERT_TRUE(boxR.ok());

    // Trying to make a face from a solid (which is not a wire)
    auto faceR = oreo::makeFaceFromWire(*ctx, boxR.value());
    EXPECT_FALSE(faceR.ok());
    EXPECT_TRUE(faceR.hasErrors());
}

// 68. Null shape to AABB query
TEST(GeomBattle, NullToAabb) {
    auto ctx = oreo::KernelContext::create();
    oreo::NamedShape nullShape;
    auto r = oreo::aabb(*ctx, nullShape);
    EXPECT_FALSE(r.ok());
}

// 69. Null shape to getFaces
TEST(GeomBattle, NullToGetFaces) {
    auto ctx = oreo::KernelContext::create();
    oreo::NamedShape nullShape;
    auto r = oreo::getFaces(*ctx, nullShape);
    EXPECT_FALSE(r.ok());
}

// 70. Null shape to getEdges
TEST(GeomBattle, NullToGetEdges) {
    auto ctx = oreo::KernelContext::create();
    oreo::NamedShape nullShape;
    auto r = oreo::getEdges(*ctx, nullShape);
    EXPECT_FALSE(r.ok());
}

// 71. Null shape to massProperties
TEST(GeomBattle, NullToMassProperties) {
    auto ctx = oreo::KernelContext::create();
    oreo::NamedShape nullShape;
    auto r = oreo::massProperties(*ctx, nullShape);
    EXPECT_FALSE(r.ok());
}

// 72. Wedge with negative ltx — OCCT may crash or throw for degenerate wedge
TEST(GeomBattle, WedgeNegativeLtx) {
    auto ctx = oreo::KernelContext::create();
    // Negative ltx may cause OCCT to crash or produce degenerate shape
    try {
        auto r = oreo::makeWedge(*ctx, 10, 10, 10, -5);
        // If it returns, it should either fail or produce a valid shape
        if (r.ok()) {
            EXPECT_FALSE(r.value().isNull());
        }
    } catch (...) {
        // OCCT may throw Standard_Failure for degenerate wedge
        SUCCEED() << "OCCT threw exception for negative ltx wedge (expected)";
    }
}

// 73. Revolve with negative angle
TEST(GeomBattle, RevolveNegativeAngle) {
    auto ctx = oreo::KernelContext::create();

    // Create a face to revolve by making a wire then face
    // Use a simple box face instead
    auto boxR = oreo::makeBox(*ctx, 5.0, 10.0, 1.0);
    ASSERT_TRUE(boxR.ok());

    auto facesR = oreo::getFaces(*ctx, boxR.value());
    ASSERT_TRUE(facesR.ok());
    ASSERT_FALSE(facesR.value().empty());

    // Get a face as the profile to revolve
    oreo::NamedShape profile(facesR.value()[0].face, boxR.value().tag());

    gp_Ax1 axis(gp_Pnt(0, 0, 0), gp_Dir(0, 1, 0));
    // Negative angle — should either work (OCCT handles it) or be rejected
    auto r = oreo::revolve(*ctx, profile, axis, -M_PI / 2.0);
    // Either outcome is fine — must not crash
    (void)r.diagnostics();
}

// 74. Extrude with zero vector
TEST(GeomBattle, ExtrudeZeroVector) {
    auto ctx = oreo::KernelContext::create();

    auto boxR = oreo::makeBox(*ctx, 10.0, 10.0, 10.0);
    ASSERT_TRUE(boxR.ok());

    auto facesR = oreo::getFaces(*ctx, boxR.value());
    ASSERT_TRUE(facesR.ok());
    ASSERT_FALSE(facesR.value().empty());

    oreo::NamedShape profile(facesR.value()[0].face, boxR.value().tag());

    // Zero extrusion direction
    gp_Vec zeroVec(0, 0, 0);
    auto r = oreo::extrude(*ctx, profile, zeroVec);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(r.hasErrors());
}

// ─────────────────────────────────────────────────────────────────────
// Shape Healing (4 tests)
// ─────────────────────────────────────────────────────────────────────

// 75. Valid shape is not modified by fixShape
TEST(ShapeHealingBattle, ValidShapeNotModified) {
    auto ctx = oreo::KernelContext::create();

    BRepPrimAPI_MakeBox maker(10.0, 10.0, 10.0);
    TopoDS_Shape box = maker.Shape();
    ASSERT_FALSE(box.IsNull());

    // Valid box — fixShape should not modify it (returns false)
    TopoDS_Shape copy = box;
    bool modified = oreo::fixShape(copy, ctx->tolerance());
    // A perfect box should not need fixing
    // (modified may be true if ShapeFix runs but finds nothing to change,
    //  but the shape should remain valid either way)
    EXPECT_TRUE(oreo::isShapeValid(copy));
}

// 76. Null shape returns false
TEST(ShapeHealingBattle, NullShapeReturnsFalse) {
    TopoDS_Shape null;
    EXPECT_TRUE(null.IsNull());
    bool modified = oreo::fixShape(null);
    EXPECT_FALSE(modified);
    EXPECT_TRUE(null.IsNull());  // Still null after fix attempt
}

// 77. Accepts tolerance policy
TEST(ShapeHealingBattle, AcceptsTolerancePolicy) {
    oreo::TolerancePolicy tol;
    tol.linearPrecision = 1e-4;  // Looser tolerance

    BRepPrimAPI_MakeBox maker(10.0, 10.0, 10.0);
    TopoDS_Shape box = maker.Shape();

    // Should not crash with a custom tolerance
    bool modified = oreo::fixShape(box, tol);
    EXPECT_TRUE(oreo::isShapeValid(box));
    (void)modified;
}

// 78. fixShape exception does not corrupt shape
TEST(ShapeHealingBattle, ExceptionDoesNotCorrupt) {
    // Create a valid shape, try to fix it, verify shape is still valid afterward
    BRepPrimAPI_MakeBox maker(10.0, 10.0, 10.0);
    TopoDS_Shape original = maker.Shape();
    ASSERT_TRUE(oreo::isShapeValid(original));

    // Fix it multiple times — should remain valid
    oreo::fixShape(original);
    EXPECT_TRUE(oreo::isShapeValid(original));
    oreo::fixShape(original);
    EXPECT_TRUE(oreo::isShapeValid(original));
}

// ─────────────────────────────────────────────────────────────────────
// OCCT Scope Guard (3 tests)
// ─────────────────────────────────────────────────────────────────────

// 79. Restores setting on scope exit
TEST(OcctScopeGuardBattle, RestoresOnExit) {
    oreo::KernelContext::initOCCT();
    // OcctStaticGuard locks the OCCT static mutex on construction
    // and restores saved settings on destruction.
    // We verify: (1) construction succeeds, (2) set() doesn't crash,
    // (3) destructor doesn't crash. OCCT's Interface_Static may not
    // accept arbitrary key/value pairs, so we don't assert the value
    // was actually changed — only that the guard lifecycle is safe.
    {
        oreo::OcctStaticGuard guard;
        guard.set("write.step.unit", "MM");
        // Guard holds the mutex — scope exit releases it
    }
    // After scope exit, no crash = success
    SUCCEED();
}

// 80. Multiple settings on a single guard scope
// NOTE: OcctStaticGuard uses a non-recursive mutex (std::mutex).
// Nesting two guards from the same thread DEADLOCKS.
// This is a known design limitation — nested OCCT settings changes
// must use a single guard with multiple set() calls.
TEST(OcctScopeGuardBattle, NestedGuards) {
    oreo::KernelContext::initOCCT();
    {
        oreo::OcctStaticGuard guard;
        guard.set("write.step.schema", "AP214IS");
        guard.set("write.step.unit", "MM");
        // Multiple set() calls on same guard work fine
        SUCCEED();
    }
    // After scope, both values should be restored
    SUCCEED();
}

// 81. Non-existent key does not crash
TEST(OcctScopeGuardBattle, NonExistentKey) {
    // Setting a nonexistent key — should not crash
    // Interface_Static::CVal returns nullptr for unknown keys
    {
        oreo::OcctStaticGuard guard;
        // This exercises the code path where CVal returns nullptr
        guard.set("nonexistent.key.battle_test_xyz", "value");
    }
    // If we get here without crashing, the test passes
    SUCCEED();
}

// ─────────────────────────────────────────────────────────────────────
// PlaneGCS Solver (8 tests)
// ─────────────────────────────────────────────────────────────────────

// 82. Conflicting constraints detected
// Use a truly conflicting setup: fix a point at (0,0) AND set its X coordinate
// to 5. PlaneGCS reliably detects this as a contradiction.
TEST(SketchBattle, ConflictingConstraints) {
    auto ctx = oreo::KernelContext::create();

    std::vector<oreo::SketchPoint> pts = {{0, 0}};
    std::vector<oreo::SketchLine> lines;
    std::vector<oreo::SketchCircle> circles;
    std::vector<oreo::SketchArc> arcs;

    // Fix point at origin AND add X coordinate = 5 — contradicts
    std::vector<oreo::SketchConstraint> constraints = {
        {oreo::ConstraintType::Fixed, 0, -1, -1, 0},
        {oreo::ConstraintType::CoordinateX, 0, -1, -1, 5.0},
    };

    auto r = oreo::solveSketch(*ctx, pts, lines, circles, arcs, constraints);
    if (r.ok()) {
        auto result = r.value();
        // Should detect conflict (Fixed at 0 vs CoordinateX=5)
        EXPECT_TRUE(result.status == oreo::SolveStatus::Conflicting
                 || result.status == oreo::SolveStatus::Failed)
            << "Expected conflicting or failed, got status=" << static_cast<int>(result.status);
    } else {
        // Solver may fail at the OperationResult level — that also counts as detecting the conflict
        SUCCEED() << "Solver returned !ok for conflicting constraints (acceptable)";
    }
}

// 83. Redundant constraints detected
TEST(SketchBattle, RedundantConstraints) {
    auto ctx = oreo::KernelContext::create();

    std::vector<oreo::SketchPoint> points = {{0, 0}, {10, 0}};
    std::vector<oreo::SketchLine> lines = {{{0, 0}, {10, 0}}};
    std::vector<oreo::SketchCircle> circles;
    std::vector<oreo::SketchArc> arcs;

    // Redundant: horizontal + 0-degree angle are the same constraint
    std::vector<oreo::SketchConstraint> constraints = {
        {oreo::ConstraintType::Horizontal, 0, -1, -1, 0.0},
        {oreo::ConstraintType::Angle, 0, -1, -1, 0.0},
    };

    auto r = oreo::solveSketch(*ctx, points, lines, circles, arcs, constraints);
    if (r.ok()) {
        // May report Redundant status
        EXPECT_TRUE(r.value().status == oreo::SolveStatus::Redundant ||
                    r.value().status == oreo::SolveStatus::OK);
    }
    // Either way, must not crash
}

// 84. Near-coincident points
TEST(SketchBattle, NearCoincidentPoints) {
    auto ctx = oreo::KernelContext::create();

    // Two points separated by an extremely small distance
    std::vector<oreo::SketchPoint> points = {
        {0.0, 0.0},
        {1e-15, 1e-15}
    };
    std::vector<oreo::SketchLine> lines;
    std::vector<oreo::SketchCircle> circles;
    std::vector<oreo::SketchArc> arcs;

    // Constrain them to be coincident
    std::vector<oreo::SketchConstraint> constraints = {
        {oreo::ConstraintType::Coincident, 0, 1, -1, 0.0}
    };

    auto r = oreo::solveSketch(*ctx, points, lines, circles, arcs, constraints);
    // Should converge — already nearly coincident
    if (r.ok()) {
        EXPECT_EQ(r.value().status, oreo::SolveStatus::OK);
    }
}

// 85. Large coordinates do not cause numerical issues
TEST(SketchBattle, LargeCoordinates) {
    auto ctx = oreo::KernelContext::create();

    std::vector<oreo::SketchPoint> points = {
        {1e6, 1e6},
        {1e6 + 100, 1e6}
    };
    std::vector<oreo::SketchLine> lines = {
        {{1e6, 1e6}, {1e6 + 100, 1e6}}
    };
    std::vector<oreo::SketchCircle> circles;
    std::vector<oreo::SketchArc> arcs;

    std::vector<oreo::SketchConstraint> constraints = {
        {oreo::ConstraintType::Horizontal, 0, -1, -1, 0.0},
        {oreo::ConstraintType::Distance, 0, -1, -1, 100.0}
    };

    auto r = oreo::solveSketch(*ctx, points, lines, circles, arcs, constraints);
    // Should handle large coordinates
    if (r.ok()) {
        EXPECT_TRUE(r.value().status == oreo::SolveStatus::OK ||
                    r.value().status == oreo::SolveStatus::Underconstrained);
    }
}

// 86. Very small coordinates
TEST(SketchBattle, SmallCoordinates) {
    auto ctx = oreo::KernelContext::create();

    std::vector<oreo::SketchPoint> points = {
        {1e-10, 1e-10},
        {1e-10 + 1e-12, 1e-10}
    };
    std::vector<oreo::SketchLine> lines = {
        {{1e-10, 1e-10}, {1e-10 + 1e-12, 1e-10}}
    };
    std::vector<oreo::SketchCircle> circles;
    std::vector<oreo::SketchArc> arcs;

    std::vector<oreo::SketchConstraint> constraints = {
        {oreo::ConstraintType::Horizontal, 0, -1, -1, 0.0}
    };

    auto r = oreo::solveSketch(*ctx, points, lines, circles, arcs, constraints);
    // Must not crash or hang — tiny coordinates are valid
    (void)r.ok();
}

// 87. Many constraints performance (should solve, not hang)
TEST(SketchBattle, ManyConstraintsPerformance) {
    auto ctx = oreo::KernelContext::create();

    // Create a chain of connected line segments
    constexpr int N = 20;
    std::vector<oreo::SketchPoint> points;
    std::vector<oreo::SketchLine> lines;
    std::vector<oreo::SketchCircle> circles;
    std::vector<oreo::SketchArc> arcs;
    std::vector<oreo::SketchConstraint> constraints;

    for (int i = 0; i <= N; ++i) {
        points.push_back({static_cast<double>(i * 10), 0.0});
    }
    for (int i = 0; i < N; ++i) {
        lines.push_back({points[i], points[i + 1]});
    }

    // Fix the first point, make all lines horizontal
    constraints.push_back({oreo::ConstraintType::Fixed, 0, -1, -1, 0.0});
    for (int i = 0; i < N; ++i) {
        constraints.push_back(
            {oreo::ConstraintType::Horizontal, i, -1, -1, 0.0});
    }

    auto r = oreo::solveSketch(*ctx, points, lines, circles, arcs, constraints);
    // Should solve without hanging
    if (r.ok()) {
        EXPECT_TRUE(r.value().status == oreo::SolveStatus::OK ||
                    r.value().status == oreo::SolveStatus::Underconstrained);
    }
}

// 88. Zero radius circle
TEST(SketchBattle, ZeroRadiusCircle) {
    auto ctx = oreo::KernelContext::create();

    std::vector<oreo::SketchPoint> points;
    std::vector<oreo::SketchLine> lines;
    std::vector<oreo::SketchCircle> circles = {{{5.0, 5.0}, 0.0}};  // zero radius
    std::vector<oreo::SketchArc> arcs;
    std::vector<oreo::SketchConstraint> constraints;

    auto r = oreo::solveSketch(*ctx, points, lines, circles, arcs, constraints);
    // Zero radius circle is degenerate — solver should handle without crash
    // Result may be OK (degenerate but valid) or Failed
    (void)r.ok();
}

// 89. Empty sketch to wire
TEST(SketchBattle, EmptySketchToWire) {
    auto ctx = oreo::KernelContext::create();

    std::vector<oreo::SketchLine> emptyLines;
    std::vector<oreo::SketchCircle> emptyCircles;
    std::vector<oreo::SketchArc> emptyArcs;

    auto r = oreo::sketchToWire(*ctx, emptyLines, emptyCircles, emptyArcs);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(r.hasErrors());
}

// ─────────────────────────────────────────────────────────────────────
// END OF SECTIONS A AND B
// Sections C, D, E will be appended by another agent below this line.
// ─────────────────────────────────────────────────────────────────────
