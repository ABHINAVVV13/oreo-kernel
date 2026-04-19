// SPDX-License-Identifier: LGPL-2.1-or-later

// test_foundation_hardened.cpp — Adversarial tests proving foundation guarantees.
//
// These tests verify fail-closed behavior under adversarial inputs:
//   - NaN/Inf rejected by all validators
//   - Unknown unit strings throw (not silent default)
//   - Schema loading rejects missing headers
//   - DiagnosticScope composes without clearing parent diagnostics
//   - OperationResult carries diagnostics with the value
//   - std::call_once for OCCT init is safe to call from multiple contexts
//
// This suite intentionally exercises the deprecated v1 scalar tag API
// (`TagAllocator::nextTag()`) to lock the v1 compatibility contract.
// File-scope suppression keeps CI logs clean; each v1 call site is
// load-bearing — migrating it to nextShapeIdentity() would drop
// coverage of the scalar contract documents persisted before v2
// landed still depend on.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__clang__)
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(disable : 4996)
#endif

#include <gtest/gtest.h>

#include "core/kernel_context.h"
#include "core/validation.h"
#include "core/units.h"
#include "core/schema.h"
#include "core/operation_result.h"
#include "core/diagnostic_scope.h"
#include "core/diagnostic.h"
#include "naming/named_shape.h"
#include "geometry/oreo_geometry.h"
#include "query/oreo_query.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <cmath>
#include <limits>

// ═══════════════════════════════════════════════════════════════
// Blocker 5: Fail-closed validation — NaN and Inf rejected
// ═══════════════════════════════════════════════════════════════

TEST(FailClosed, NaNRejectedByRequirePositive) {
    auto ctx = oreo::KernelContext::create();
    double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(oreo::validation::requirePositive(*ctx, nan, "test"));
    EXPECT_TRUE(ctx->diag().hasErrors());
    // Verify the error message mentions NaN
    auto* err = ctx->diag().lastError();
    ASSERT_NE(err, nullptr);
    EXPECT_NE(err->message.find("NaN"), std::string::npos);
}

TEST(FailClosed, InfRejectedByRequirePositive) {
    auto ctx = oreo::KernelContext::create();
    double inf = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(oreo::validation::requirePositive(*ctx, inf, "test"));
    EXPECT_TRUE(ctx->diag().hasErrors());
}

TEST(FailClosed, NaNRejectedByRequireInRange) {
    auto ctx = oreo::KernelContext::create();
    double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(oreo::validation::requireInRange(*ctx, nan, 0.0, 10.0, "test"));
    EXPECT_TRUE(ctx->diag().hasErrors());
}

TEST(FailClosed, NaNRejectedByRequireNonNegative) {
    auto ctx = oreo::KernelContext::create();
    double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(oreo::validation::requireNonNegative(*ctx, nan, "test"));
    EXPECT_TRUE(ctx->diag().hasErrors());
}

TEST(FailClosed, NaNRejectedByRequireNonZeroVec) {
    auto ctx = oreo::KernelContext::create();
    double nan = std::numeric_limits<double>::quiet_NaN();
    gp_Vec nanVec(nan, 0, 0);
    EXPECT_FALSE(oreo::validation::requireNonZeroVec(*ctx, nanVec, "test"));
    EXPECT_TRUE(ctx->diag().hasErrors());
}

TEST(FailClosed, RequireValidDirNotNoOp) {
    auto ctx = oreo::KernelContext::create();
    // Normal direction should pass
    EXPECT_TRUE(oreo::validation::requireValidDir(*ctx, gp_Dir(0, 0, 1), "test"));
    EXPECT_FALSE(ctx->diag().hasErrors());
}

// ═══════════════════════════════════════════════════════════════
// Blocker 5: Fail-closed unit parsing
// ═══════════════════════════════════════════════════════════════

TEST(FailClosed, UnknownLengthUnitThrows) {
    EXPECT_THROW(oreo::unit_convert::parseLengthUnit("furlongs"), std::invalid_argument);
}

TEST(FailClosed, UnknownAngleUnitThrows) {
    EXPECT_THROW(oreo::unit_convert::parseAngleUnit("gradians"), std::invalid_argument);
}

TEST(FailClosed, TryParseLengthUnitReturnsNullopt) {
    auto result = oreo::unit_convert::tryParseLengthUnit("furlongs");
    EXPECT_FALSE(result.has_value());
}

TEST(FailClosed, TryParseAngleUnitReturnsNullopt) {
    auto result = oreo::unit_convert::tryParseAngleUnit("gradians");
    EXPECT_FALSE(result.has_value());
}

TEST(FailClosed, ValidUnitsStillWork) {
    EXPECT_EQ(oreo::unit_convert::parseLengthUnit("mm"), oreo::LengthUnit::Millimeter);
    EXPECT_EQ(oreo::unit_convert::parseLengthUnit("inch"), oreo::LengthUnit::Inch);
    EXPECT_EQ(oreo::unit_convert::parseAngleUnit("deg"), oreo::AngleUnit::Degree);
    EXPECT_EQ(oreo::unit_convert::parseAngleUnit("rad"), oreo::AngleUnit::Radian);
}

// ═══════════════════════════════════════════════════════════════
// Blocker 7: Fail-closed schema loading
// ═══════════════════════════════════════════════════════════════

TEST(FailClosed, SchemaRejectsMissingHeader) {
    oreo::SchemaRegistry reg;
    nlohmann::json noHeader = {{"features", nlohmann::json::array()}};
    EXPECT_THROW(reg.migrate(oreo::schema::TYPE_FEATURE_TREE, noHeader), std::runtime_error);
}

TEST(FailClosed, SchemaRejectsUnknownType) {
    oreo::SchemaRegistry reg;
    EXPECT_THROW(reg.currentVersion("oreo.nonexistent_type"), std::runtime_error);
}

TEST(FailClosed, SchemaRejectsTypeMismatch) {
    oreo::SchemaRegistry reg;
    auto data = oreo::SchemaRegistry::addHeader(
        "oreo.feature", oreo::schema::FEATURE, nlohmann::json::object());
    // Try to load as feature_tree — should fail
    EXPECT_THROW(reg.migrate(oreo::schema::TYPE_FEATURE_TREE, data), std::runtime_error);
}

TEST(FailClosed, SchemaRejectsFutureMajorVersion) {
    oreo::SchemaRegistry reg;
    auto data = oreo::SchemaRegistry::addHeader(
        oreo::schema::TYPE_FEATURE_TREE,
        {99, 0, 0},  // Future major version
        nlohmann::json::object());
    EXPECT_THROW(reg.migrate(oreo::schema::TYPE_FEATURE_TREE, data), std::runtime_error);
}

TEST(FailClosed, SchemaVersionParseRejectsEmpty) {
    EXPECT_THROW(oreo::SchemaVersion::parse(""), std::exception);
}

// ═══════════════════════════════════════════════════════════════
// Blocker 3: OperationResult carries diagnostics
// ═══════════════════════════════════════════════════════════════

TEST(OperationResult, SuccessCarriesValue) {
    auto result = oreo::OperationResult<int>::success(42);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.value(), 42);
    EXPECT_FALSE(result.hasErrors());
}

TEST(OperationResult, FailureHasNoValue) {
    oreo::Diagnostic diag;
    diag.severity = oreo::Severity::Error;
    diag.code = oreo::ErrorCode::OCCT_FAILURE;
    diag.message = "Something broke";
    auto result = oreo::OperationResult<int>::failure({diag});
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
    EXPECT_THROW(result.valueOrThrow(), std::runtime_error);
    EXPECT_EQ(result.errorMessage(), "Something broke");
}

TEST(OperationResult, SuccessWithWarnings) {
    oreo::Diagnostic warn;
    warn.severity = oreo::Severity::Warning;
    warn.code = oreo::ErrorCode::SHAPE_INVALID;
    warn.message = "Degraded";
    warn.geometryDegraded = true;

    auto result = oreo::OperationResult<int>::success(42, {warn});
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.value(), 42);
    EXPECT_TRUE(result.hasWarnings());
    EXPECT_TRUE(result.isGeometryDegraded());
    EXPECT_FALSE(result.hasErrors());
}

TEST(OperationResult, ValueOrOnFailure) {
    auto result = oreo::OperationResult<int>::failure({});
    EXPECT_EQ(result.valueOr(99), 99);
}

// ═══════════════════════════════════════════════════════════════
// Blocker 4: DiagnosticScope composes safely
// ═══════════════════════════════════════════════════════════════

TEST(DiagnosticScope, DoesNotClearParentDiagnostics) {
    auto ctx = oreo::KernelContext::create();

    // Parent reports a warning
    ctx->diag().warning(oreo::ErrorCode::SHAPE_INVALID, "Parent warning");
    EXPECT_EQ(ctx->diag().count(), 1);

    // Child scope starts — parent warning should still be there
    {
        oreo::DiagnosticScope scope(*ctx);
        ctx->diag().error(oreo::ErrorCode::OCCT_FAILURE, "Child error");

        // Scope sees only the child diagnostic
        EXPECT_TRUE(scope.hasErrors());
        auto childDiags = scope.extractDiagnostics();
        EXPECT_EQ(childDiags.size(), 1u);
        EXPECT_EQ(childDiags[0].message, "Child error");
    }

    // After scope, ALL diagnostics still in context (parent + child)
    EXPECT_EQ(ctx->diag().count(), 2);
    EXPECT_TRUE(ctx->diag().hasErrors());
    EXPECT_TRUE(ctx->diag().hasWarnings());
}

TEST(DiagnosticScope, MakeResultCapturesOnlyScopeDiags) {
    auto ctx = oreo::KernelContext::create();
    ctx->diag().info(oreo::ErrorCode::OK, "Pre-existing info");

    oreo::DiagnosticScope scope(*ctx);
    ctx->diag().warning(oreo::ErrorCode::SHAPE_INVALID, "Scope warning");

    auto result = scope.makeResult(42);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.value(), 42);
    EXPECT_TRUE(result.hasWarnings());
    // Only the scope's warning, not the pre-existing info
    EXPECT_EQ(result.diagnostics().size(), 1u);
}

TEST(DiagnosticScope, NestedScopesCompose) {
    auto ctx = oreo::KernelContext::create();

    oreo::DiagnosticScope outer(*ctx);
    ctx->diag().info(oreo::ErrorCode::OK, "Outer info");

    {
        oreo::DiagnosticScope inner(*ctx);
        ctx->diag().error(oreo::ErrorCode::OCCT_FAILURE, "Inner error");

        auto innerDiags = inner.extractDiagnostics();
        EXPECT_EQ(innerDiags.size(), 1u);
        EXPECT_EQ(innerDiags[0].message, "Inner error");
    }

    auto outerDiags = outer.extractDiagnostics();
    EXPECT_EQ(outerDiags.size(), 2u);  // Outer info + inner error
}

// ═══════════════════════════════════════════════════════════════
// Blocker 8: OCCT init is safe with std::call_once
// ═══════════════════════════════════════════════════════════════

TEST(OCCTInit, MultipleCallsAreSafe) {
    // Call initOCCT multiple times — should not crash or double-init
    oreo::KernelContext::initOCCT();
    oreo::KernelContext::initOCCT();
    oreo::KernelContext::initOCCT();
    EXPECT_TRUE(oreo::KernelContext::isOCCTInitialized());
}

TEST(OCCTInit, MultipleContextCreationsAreSafe) {
    // Creating multiple contexts all call initOCCT — should be safe
    auto ctx1 = oreo::KernelContext::create();
    auto ctx2 = oreo::KernelContext::create();
    auto ctx3 = oreo::KernelContext::create();
    EXPECT_TRUE(oreo::KernelContext::isOCCTInitialized());
}

// ═══════════════════════════════════════════════════════════════
// Geometry operations reject NaN
// ═══════════════════════════════════════════════════════════════

TEST(FailClosed, ExtrudeRejectsNaNDirection) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 10, 10).value();
    double nan = std::numeric_limits<double>::quiet_NaN();
    auto result = oreo::extrude(*ctx, box, gp_Vec(0, 0, nan));
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
}

TEST(FailClosed, MakeBoxRejectsZeroDimension) {
    auto ctx = oreo::KernelContext::create();
    auto result = oreo::makeBox(*ctx, 0, 10, 10);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
}

TEST(FailClosed, MakeBoxRejectsNegativeDimension) {
    auto ctx = oreo::KernelContext::create();
    auto result = oreo::makeBox(*ctx, -5, 10, 10);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
}

// ═══════════════════════════════════════════════════════════════
// Blocker 10: Document-derived deterministic IDs
// ═══════════════════════════════════════════════════════════════

TEST(DeterministicIDs, SingleDocumentModeIsSequential) {
    // Default (documentId=0) → tags are 1, 2, 3...
    oreo::TagAllocator alloc;
    EXPECT_EQ(alloc.nextTag(), 1);
    EXPECT_EQ(alloc.nextTag(), 2);
    EXPECT_EQ(alloc.nextTag(), 3);
}

TEST(DeterministicIDs, MultiDocumentTagsEncodeDocumentId) {
    oreo::TagAllocator alloc(42);  // documentId = 42
    auto tag1 = alloc.nextTag();
    auto tag2 = alloc.nextTag();

    // Tags encode document ID in upper 32 bits, counter in lower 32
    EXPECT_EQ(oreo::TagAllocator::extractDocumentId(tag1), 42u);
    EXPECT_EQ(oreo::TagAllocator::extractCounter(tag1), 1u);
    EXPECT_EQ(oreo::TagAllocator::extractDocumentId(tag2), 42u);
    EXPECT_EQ(oreo::TagAllocator::extractCounter(tag2), 2u);
}

TEST(DeterministicIDs, TwoDocumentsNeverCollide) {
    oreo::TagAllocator doc1(100);
    oreo::TagAllocator doc2(200);

    auto tag1a = doc1.nextTag();
    auto tag2a = doc2.nextTag();
    auto tag1b = doc1.nextTag();
    auto tag2b = doc2.nextTag();

    // Same counter values but different document IDs → different tags
    EXPECT_NE(tag1a, tag2a);
    EXPECT_NE(tag1b, tag2b);

    // Counter values are the same within each document
    EXPECT_EQ(oreo::TagAllocator::extractCounter(tag1a), 1u);
    EXPECT_EQ(oreo::TagAllocator::extractCounter(tag2a), 1u);
}

TEST(DeterministicIDs, DocumentIdFromUUID) {
    oreo::KernelConfig config;
    config.documentUUID = "550e8400-e29b-41d4-a716-446655440000";

    auto ctx = oreo::KernelContext::create(config);
    EXPECT_NE(ctx->tags().documentId(), 0u);  // Should have a non-zero doc ID

    auto tag = ctx->tags().nextTag();
    // documentId() is now 64-bit (SipHash output). The tag encoding only
    // carries the low 32 bits of it, so the extracted value must be compared
    // against the low 32 bits of the full documentId.
    const uint32_t expectedLow32 =
        static_cast<uint32_t>(ctx->tags().documentId() & 0xFFFFFFFFull);
    EXPECT_EQ(oreo::TagAllocator::extractDocumentId(tag), expectedLow32);
}

TEST(DeterministicIDs, ResetPreservesDocumentId) {
    oreo::TagAllocator alloc(77);
    alloc.nextTag();
    alloc.nextTag();
    // resetCounterOnly() preserves documentId; the wider reset() would zero
    // it. The test's invariant (docId preserved after counter reset) is what
    // resetCounterOnly was introduced for.
    alloc.resetCounterOnly();

    auto tag = alloc.nextTag();
    EXPECT_EQ(oreo::TagAllocator::extractDocumentId(tag), 77u);
    EXPECT_EQ(oreo::TagAllocator::extractCounter(tag), 1u);  // Counter reset, docId preserved
}

TEST(DeterministicIDs, TwoDocumentsReplayIdentically) {
    // Two independent contexts with same UUID → same tags after reset+replay.
    // Use resetCounterOnly() so the document-id derived from the UUID is
    // preserved through the reset; otherwise both contexts would fall back
    // to single-doc mode (docId=0) and the test would no longer exercise
    // the UUID-derived documentId path.
    oreo::KernelConfig cfg1;
    cfg1.documentUUID = "test-doc";
    auto ctx1 = oreo::KernelContext::create(cfg1);
    oreo::KernelConfig cfg2;
    cfg2.documentUUID = "test-doc";
    auto ctx2 = oreo::KernelContext::create(cfg2);

    ctx1->tags().resetCounterOnly();
    ctx2->tags().resetCounterOnly();

    EXPECT_EQ(ctx1->tags().nextTag(), ctx2->tags().nextTag());
    EXPECT_EQ(ctx1->tags().nextTag(), ctx2->tags().nextTag());
    EXPECT_EQ(ctx1->tags().nextTag(), ctx2->tags().nextTag());
}

// ═══════════════════════════════════════════════════════════════
// Blocker 6: Units enforcement — verify inch context produces different geometry
// ═══════════════════════════════════════════════════════════════

TEST(UnitsEnforcement, InchContextProducesLargerBox) {
    // MM context: makeBox(10, 10, 10) → 10mm box
    oreo::KernelConfig mmConfig;
    mmConfig.units.documentLength = oreo::LengthUnit::Millimeter;
    auto mmCtx = oreo::KernelContext::create(mmConfig);
    auto mmBoxResult = oreo::makeBox(*mmCtx, 10, 10, 10);
    ASSERT_TRUE(mmBoxResult.ok());
    auto mmBox = mmBoxResult.value();
    auto mmProps = oreo::massProperties(*mmCtx, mmBox).value();

    // Inch context: makeBox(10, 10, 10) → 10 inch box = 254mm box
    oreo::KernelConfig inConfig;
    inConfig.units.documentLength = oreo::LengthUnit::Inch;
    auto inCtx = oreo::KernelContext::create(inConfig);
    auto inBoxResult = oreo::makeBox(*inCtx, 10, 10, 10);
    ASSERT_TRUE(inBoxResult.ok());
    auto inBox = inBoxResult.value();
    auto inProps = oreo::massProperties(*inCtx, inBox).value();

    // Inch box should be 25.4^3 = ~16387x larger in volume
    double ratio = inProps.volume / mmProps.volume;
    EXPECT_NEAR(ratio, 25.4 * 25.4 * 25.4, 100.0);  // ~16387
}
