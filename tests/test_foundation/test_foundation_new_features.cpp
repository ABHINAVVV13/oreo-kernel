// SPDX-License-Identifier: LGPL-2.1-or-later

// test_foundation_new_features.cpp — Targeted tests for the newly-landed
// KernelContext / OperationResult / occtSafeCall / validation / schema /
// diagnostic-metadata APIs.
//
// Every test exercises a specific commitment called out in the API
// surface. Where the documented behaviour cannot be reached through the
// public surface (e.g. SchemaRegistry does not expose a way to add a
// type to `currentVersions_`), the test substitutes a type that is
// registered by the SchemaRegistry ctor — the round-trip semantics of
// `testMigration` are identical.
//
// Match the style of tests/test_foundation/test_foundation.cpp: one
// GoogleTest case per invariant, minimal fixtures, `oreo::` qualification.
//
// Grouping:
//   - KernelContext features: clone, merge, safeReset, schema freeze,
//     cancellation, config sanitization
//   - Progress callback: clamping + exception containment
//   - OperationResult combinators: map, flatMap, andThen, messages, ==
//   - occtSafeCall: happy path + exception conversion
//   - Validation helpers: requireTolerance, requireCoplanar,
//     requirePerpendicular, requireCollinear, requireAllPositive
//   - SchemaRegistry: step cap, introspection, testMigration round-trip
//   - Diagnostic metadata (D-9): timestamp / thread / context stamping

#include <gtest/gtest.h>

#include "core/kernel_context.h"
#include "core/operation_result.h"
#include "core/diagnostic.h"
#include "core/diagnostic_scope.h"
#include "core/occt_try.h"
#include "core/schema.h"
#include "core/validation.h"
#include "core/oreo_error.h"

#include <gp_Pnt.hxx>
#include <gp_Dir.hxx>
#include <gp_Vec.hxx>

#include <Standard_Failure.hxx>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr double NaND = std::numeric_limits<double>::quiet_NaN();
constexpr double InfD = std::numeric_limits<double>::infinity();

// Helper — count diagnostics with matching (severity, code).
int countDiag(const oreo::DiagnosticCollector& diag,
              oreo::Severity sev, oreo::ErrorCode code) {
    int n = 0;
    for (const auto& d : diag.all()) {
        if (d.severity == sev && d.code == code) ++n;
    }
    return n;
}

bool hasDiag(const oreo::DiagnosticCollector& diag,
             oreo::Severity sev, oreo::ErrorCode code) {
    return countDiag(diag, sev, code) > 0;
}

// Helper — search diagnostic messages for a substring.
bool anyMessageContains(const oreo::DiagnosticCollector& diag,
                        const std::string& needle) {
    for (const auto& d : diag.all()) {
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

} // namespace

// ═══════════════════════════════════════════════════════════════
// KernelContext — clone / merge / safeReset
// ═══════════════════════════════════════════════════════════════

TEST(KernelContextClone, IndependentCopyPreservesPolicy) {
    oreo::KernelConfig cfg;
    cfg.tolerance.linearPrecision = 2.5e-6;
    cfg.units.documentLength = oreo::LengthUnit::Inch;
    cfg.quotas.maxDiagnostics = 5000;
    auto orig = oreo::KernelContext::create(cfg);

    auto copy = orig->clone();
    ASSERT_NE(copy, nullptr);

    // Same policy as the original.
    EXPECT_DOUBLE_EQ(copy->tolerance().linearPrecision,
                     orig->tolerance().linearPrecision);
    EXPECT_EQ(copy->units().documentLength, orig->units().documentLength);
    EXPECT_EQ(copy->quotas().maxDiagnostics, orig->quotas().maxDiagnostics);

    // Independent mutation: diagnostic added after the clone must not
    // bleed back into the original.
    copy->diag().error(oreo::ErrorCode::INVALID_INPUT, "post-clone");
    EXPECT_TRUE(copy->diag().hasErrors());
    EXPECT_FALSE(orig->diag().hasErrors());

    // Clone has its own numeric id.
    EXPECT_NE(copy->numericId(), orig->numericId());
}

TEST(KernelContextClone, CarriesDiagnosticSnapshot) {
    auto orig = oreo::KernelContext::create();
    orig->diag().warning(oreo::ErrorCode::SHAPE_INVALID, "first");
    orig->diag().error(oreo::ErrorCode::OCCT_FAILURE, "second");

    auto copy = orig->clone();

    EXPECT_EQ(copy->diag().count(), 2);
    EXPECT_EQ(copy->diag().errorCount(), 1);
    EXPECT_EQ(copy->diag().warningCount(), 1);
    EXPECT_TRUE(anyMessageContains(copy->diag(), "first"));
    EXPECT_TRUE(anyMessageContains(copy->diag(), "second"));
}

TEST(KernelContextMerge, AppendsOtherDiagnostics) {
    auto a = oreo::KernelContext::create();
    auto b = oreo::KernelContext::create();
    a->diag().error(oreo::ErrorCode::INVALID_INPUT, "from-a");
    b->diag().error(oreo::ErrorCode::BOOLEAN_FAILED, "from-b1");
    b->diag().warning(oreo::ErrorCode::SHAPE_INVALID, "from-b2");

    a->merge(*b);

    EXPECT_EQ(a->diag().count(), 3);
    EXPECT_TRUE(anyMessageContains(a->diag(), "from-a"));
    EXPECT_TRUE(anyMessageContains(a->diag(), "from-b1"));
    EXPECT_TRUE(anyMessageContains(a->diag(), "from-b2"));

    // b is left intact.
    EXPECT_EQ(b->diag().count(), 2);
}

TEST(KernelContextSafeReset, ClearsDiagnosticsPreservesPolicy) {
    oreo::KernelConfig cfg;
    cfg.tolerance.linearPrecision = 7.5e-6;
    cfg.units.documentLength = oreo::LengthUnit::Centimeter;
    auto ctx = oreo::KernelContext::create(cfg);

    ctx->diag().error(oreo::ErrorCode::OCCT_FAILURE, "pre-reset");
    ctx->tags().nextTag();
    ctx->tags().nextTag();

    const double tolBefore = ctx->tolerance().linearPrecision;
    const auto lengthUnitBefore = ctx->units().documentLength;
    const auto tagBefore = ctx->tags().currentValue();

    ctx->safeReset();

    EXPECT_TRUE(ctx->diag().empty());
    EXPECT_DOUBLE_EQ(ctx->tolerance().linearPrecision, tolBefore);
    EXPECT_EQ(ctx->units().documentLength, lengthUnitBefore);
    // Tag allocator is structural state; safeReset preserves it.
    EXPECT_EQ(ctx->tags().currentValue(), tagBefore);
}

// ═══════════════════════════════════════════════════════════════
// KernelContext — cancellation
// ═══════════════════════════════════════════════════════════════

TEST(KernelContextCancellation, LazyTokenCreation) {
    auto ctx = oreo::KernelContext::create();
    auto t1 = ctx->cancellationToken();
    ASSERT_NE(t1, nullptr);
    EXPECT_FALSE(t1->isCancelled());

    // Same token is returned on repeated access.
    auto t2 = ctx->cancellationToken();
    EXPECT_EQ(t1.get(), t2.get());
}

TEST(KernelContextCancellation, CheckCancellationReportsDiagnostic) {
    auto ctx = oreo::KernelContext::create();

    // Before cancellation, checkCancellation returns false and emits
    // nothing.
    EXPECT_FALSE(ctx->checkCancellation());
    EXPECT_TRUE(ctx->diag().empty());

    // Flip the token.
    ctx->cancellationToken()->cancel();
    EXPECT_TRUE(ctx->checkCancellation());
    EXPECT_TRUE(hasDiag(ctx->diag(), oreo::Severity::Error,
                        oreo::ErrorCode::CANCELLED));
}

// ═══════════════════════════════════════════════════════════════
// KernelContext — schema freeze
// ═══════════════════════════════════════════════════════════════

TEST(KernelContextSchemaFreeze, MutableAfterFreezeReportsInvalidState) {
    auto ctx = oreo::KernelContext::create();
    EXPECT_FALSE(ctx->schemasFrozen());

    ctx->freezeSchemas();
    EXPECT_TRUE(ctx->schemasFrozen());

    // The legacy schemas() overload stays silent.
    (void)ctx->schemas();
    EXPECT_FALSE(hasDiag(ctx->diag(), oreo::Severity::Error,
                         oreo::ErrorCode::INVALID_STATE));

    // mutableSchemas() reports a diagnostic but still hands back a ref.
    auto& reg = ctx->mutableSchemas();
    (void)reg;  // silence unused-warning
    EXPECT_TRUE(hasDiag(ctx->diag(), oreo::Severity::Error,
                        oreo::ErrorCode::INVALID_STATE));
}

// ═══════════════════════════════════════════════════════════════
// KernelContext — config sanitization
// ═══════════════════════════════════════════════════════════════

TEST(KernelContextSanitize, NonFiniteToleranceReportsDiagnostic) {
    oreo::KernelConfig cfg;
    cfg.tolerance.linearPrecision = NaND;
    auto ctx = oreo::KernelContext::create(cfg);

    // A Fatal-severity diagnostic referencing the clamped field.
    EXPECT_TRUE(hasDiag(ctx->diag(), oreo::Severity::Fatal,
                        oreo::ErrorCode::INVALID_INPUT));
    EXPECT_TRUE(anyMessageContains(ctx->diag(), "linearPrecision"));

    // Tolerance was clamped to a sane positive default.
    EXPECT_GT(ctx->tolerance().linearPrecision, 0.0);
    EXPECT_TRUE(std::isfinite(ctx->tolerance().linearPrecision));
}

TEST(KernelContextSanitize, InfiniteAngularToleranceAlsoClamped) {
    oreo::KernelConfig cfg;
    cfg.tolerance.angularPrecision = InfD;
    auto ctx = oreo::KernelContext::create(cfg);

    EXPECT_TRUE(hasDiag(ctx->diag(), oreo::Severity::Fatal,
                        oreo::ErrorCode::INVALID_INPUT));
    EXPECT_TRUE(anyMessageContains(ctx->diag(), "angularPrecision"));
    EXPECT_TRUE(std::isfinite(ctx->tolerance().angularPrecision));
    EXPECT_GT(ctx->tolerance().angularPrecision, 0.0);
}

// ═══════════════════════════════════════════════════════════════
// Progress callback
// ═══════════════════════════════════════════════════════════════

TEST(KernelContextProgress, InvokesCallbackWithFraction) {
    auto ctx = oreo::KernelContext::create();
    double seen = -1.0;
    std::string seenPhase;
    ctx->setProgressCallback([&](double f, const std::string& p) {
        seen = f;
        seenPhase = p;
    });

    EXPECT_TRUE(ctx->hasProgressCallback());
    ctx->reportProgress(0.42, "phase-a");
    EXPECT_NEAR(seen, 0.42, 1e-12);
    EXPECT_EQ(seenPhase, "phase-a");
}

TEST(KernelContextProgress, FractionClampedAboveOne) {
    auto ctx = oreo::KernelContext::create();
    double seen = -1.0;
    ctx->setProgressCallback([&](double f, const std::string&) { seen = f; });
    ctx->reportProgress(2.5, "over");
    EXPECT_DOUBLE_EQ(seen, 1.0);
}

TEST(KernelContextProgress, FractionClampedBelowZero) {
    auto ctx = oreo::KernelContext::create();
    double seen = 42.0;
    ctx->setProgressCallback([&](double f, const std::string&) { seen = f; });
    ctx->reportProgress(-0.5, "under");
    EXPECT_DOUBLE_EQ(seen, 0.0);
}

TEST(KernelContextProgress, NaNFractionBecomesZero) {
    auto ctx = oreo::KernelContext::create();
    double seen = 42.0;
    ctx->setProgressCallback([&](double f, const std::string&) { seen = f; });
    ctx->reportProgress(NaND, "nan");
    EXPECT_DOUBLE_EQ(seen, 0.0);
}

TEST(KernelContextProgress, ThrowingCallbackDoesNotPropagate) {
    auto ctx = oreo::KernelContext::create();
    ctx->setProgressCallback([](double, const std::string&) {
        throw std::runtime_error("callback blew up");
    });

    // The context must swallow the exception; no UB, no crash.
    EXPECT_NO_THROW(ctx->reportProgress(0.5, "phase"));
    // A Warning diagnostic is expected (swallow-with-notice).
    EXPECT_TRUE(ctx->diag().hasWarnings());
}

TEST(KernelContextProgress, NoCallbackIsNoop) {
    auto ctx = oreo::KernelContext::create();
    EXPECT_FALSE(ctx->hasProgressCallback());
    EXPECT_NO_THROW(ctx->reportProgress(0.5, "no-cb"));
}

// ═══════════════════════════════════════════════════════════════
// OperationResult — map / flatMap / andThen
// ═══════════════════════════════════════════════════════════════

TEST(OperationResultMap, SuccessTransformsValue) {
    auto ok = oreo::OperationResult<int>::success(3);
    auto doubled = ok.map([](int x) { return x * 2; });
    EXPECT_TRUE(doubled.ok());
    EXPECT_EQ(doubled.value(), 6);
}

TEST(OperationResultMap, FailurePropagatesDiagnostics) {
    std::vector<oreo::Diagnostic> diags;
    diags.push_back(oreo::Diagnostic::error(oreo::ErrorCode::INVALID_INPUT, "bad"));
    auto bad = oreo::OperationResult<int>::failure(diags);

    auto mapped = bad.map([](int x) { return x * 2; });
    EXPECT_FALSE(mapped.ok());
    ASSERT_EQ(mapped.diagnostics().size(), 1u);
    EXPECT_EQ(mapped.diagnostics()[0].code, oreo::ErrorCode::INVALID_INPUT);
    EXPECT_EQ(mapped.diagnostics()[0].severity, oreo::Severity::Error);
    // The lambda must not have run on a failure.
    // (Cannot assert directly, but the diagnostic count not growing is a
    // proxy; a success path would have appended nothing anyway, so we
    // just confirm the value is absent.)
    EXPECT_EQ(mapped.tryValue(), nullptr);
}

TEST(OperationResultFlatMap, ChainMergesDiagnostics) {
    std::vector<oreo::Diagnostic> firstDiags;
    firstDiags.push_back(
        oreo::Diagnostic::warning(oreo::ErrorCode::SHAPE_INVALID, "w-outer"));
    auto first = oreo::OperationResult<int>::success(10, firstDiags);

    auto chained = first.flatMap([](int x) {
        std::vector<oreo::Diagnostic> ds;
        ds.push_back(
            oreo::Diagnostic::warning(oreo::ErrorCode::SHAPE_INVALID, "w-inner"));
        return oreo::OperationResult<std::string>::success(
            std::to_string(x + 1), ds);
    });

    EXPECT_TRUE(chained.ok());
    EXPECT_EQ(chained.value(), "11");
    // Outer diagnostics come first, inner appended.
    ASSERT_EQ(chained.diagnostics().size(), 2u);
    EXPECT_EQ(chained.diagnostics()[0].message, "w-outer");
    EXPECT_EQ(chained.diagnostics()[1].message, "w-inner");
}

TEST(OperationResultFlatMap, FailureShortCircuits) {
    std::vector<oreo::Diagnostic> diags;
    diags.push_back(
        oreo::Diagnostic::error(oreo::ErrorCode::INVALID_INPUT, "outer-err"));
    auto failed = oreo::OperationResult<int>::failure(diags);

    bool lambdaRan = false;
    auto next = failed.flatMap([&](int) {
        lambdaRan = true;
        return oreo::OperationResult<int>::success(0);
    });

    EXPECT_FALSE(lambdaRan);
    EXPECT_FALSE(next.ok());
    ASSERT_EQ(next.diagnostics().size(), 1u);
    EXPECT_EQ(next.diagnostics()[0].message, "outer-err");
}

TEST(OperationResultAndThen, RunsOnlyOnSuccess) {
    int sideEffect = 0;
    auto ok = oreo::OperationResult<int>::success(7);
    auto r1 = ok.andThen([&](const int& v) { sideEffect = v; });
    EXPECT_TRUE(r1.ok());
    EXPECT_EQ(r1.value(), 7);
    EXPECT_EQ(sideEffect, 7);

    // Failure path must NOT invoke the callable.
    sideEffect = -1;
    std::vector<oreo::Diagnostic> diags;
    diags.push_back(
        oreo::Diagnostic::error(oreo::ErrorCode::INVALID_INPUT, "nope"));
    auto bad = oreo::OperationResult<int>::failure(diags);
    auto r2 = bad.andThen([&](const int& v) { sideEffect = v; });
    EXPECT_FALSE(r2.ok());
    EXPECT_EQ(sideEffect, -1);  // unchanged
}

// ═══════════════════════════════════════════════════════════════
// OperationResult — messages, accessors, equality
// ═══════════════════════════════════════════════════════════════

TEST(OperationResultMessages, AllErrorMessagesCollectsErrorsAndFatals) {
    std::vector<oreo::Diagnostic> diags;
    diags.push_back(
        oreo::Diagnostic::info(oreo::ErrorCode::OK, "info-msg"));
    diags.push_back(
        oreo::Diagnostic::warning(oreo::ErrorCode::SHAPE_INVALID, "warn-msg"));
    diags.push_back(
        oreo::Diagnostic::error(oreo::ErrorCode::INVALID_INPUT, "err1"));
    diags.push_back(
        oreo::Diagnostic::fatal(oreo::ErrorCode::INTERNAL_ERROR, "fatal1"));

    auto r = oreo::OperationResult<int>::failure(diags);
    auto msgs = r.allErrorMessages();
    ASSERT_EQ(msgs.size(), 2u);
    EXPECT_EQ(msgs[0], "err1");
    EXPECT_EQ(msgs[1], "fatal1");
}

TEST(OperationResultAccessors, TryValueAndValueOrThrow) {
    auto ok = oreo::OperationResult<int>::success(99);
    const int* p = ok.tryValue();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 99);
    EXPECT_EQ(ok.valueOrThrow(), 99);

    std::vector<oreo::Diagnostic> diags;
    diags.push_back(
        oreo::Diagnostic::error(oreo::ErrorCode::INVALID_INPUT, "bad"));
    auto bad = oreo::OperationResult<int>::failure(diags);
    EXPECT_EQ(bad.tryValue(), nullptr);
    EXPECT_THROW((void)bad.valueOrThrow(), std::runtime_error);
}

TEST(OperationResultEquality, SameShapeEqual) {
    std::vector<oreo::Diagnostic> da;
    da.push_back(
        oreo::Diagnostic::error(oreo::ErrorCode::INVALID_INPUT, "first msg"));
    std::vector<oreo::Diagnostic> db;
    db.push_back(
        oreo::Diagnostic::error(oreo::ErrorCode::INVALID_INPUT, "different msg"));

    auto a = oreo::OperationResult<int>::failure(da);
    auto b = oreo::OperationResult<int>::failure(db);

    // operator== ignores message differences — (ok, severity, code) match.
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
}

TEST(OperationResultEquality, DifferentOkFlagsNotEqual) {
    auto ok = oreo::OperationResult<int>::success(5);
    std::vector<oreo::Diagnostic> d;
    auto bad = oreo::OperationResult<int>::failure(d);
    EXPECT_FALSE(ok == bad);
    EXPECT_TRUE(ok != bad);
}

TEST(OperationResultEquality, DifferentCodesNotEqual) {
    std::vector<oreo::Diagnostic> da;
    da.push_back(
        oreo::Diagnostic::error(oreo::ErrorCode::INVALID_INPUT, "m"));
    std::vector<oreo::Diagnostic> db;
    db.push_back(
        oreo::Diagnostic::error(oreo::ErrorCode::BOOLEAN_FAILED, "m"));

    auto a = oreo::OperationResult<int>::failure(da);
    auto b = oreo::OperationResult<int>::failure(db);
    EXPECT_FALSE(a == b);
}

// ═══════════════════════════════════════════════════════════════
// occtSafeCall
// ═══════════════════════════════════════════════════════════════

TEST(OcctSafeCall, HappyPathReturnsSuccess) {
    auto ctx = oreo::KernelContext::create();
    oreo::DiagnosticScope scope(*ctx);
    auto r = oreo::occtSafeCall(*ctx, scope, "unit-test", [] {
        return 7;
    });
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 7);
}

TEST(OcctSafeCall, RuntimeErrorConvertsToInternalError) {
    auto ctx = oreo::KernelContext::create();
    oreo::DiagnosticScope scope(*ctx);
    auto r = oreo::occtSafeCall(*ctx, scope, "runtime-op", []() -> int {
        throw std::runtime_error("boom");
    });
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(hasDiag(ctx->diag(), oreo::Severity::Error,
                        oreo::ErrorCode::INTERNAL_ERROR));
    EXPECT_TRUE(anyMessageContains(ctx->diag(), "boom"));
}

TEST(OcctSafeCall, BadAllocConvertsToResourceExhaustedFatal) {
    auto ctx = oreo::KernelContext::create();
    oreo::DiagnosticScope scope(*ctx);
    auto r = oreo::occtSafeCall(*ctx, scope, "alloc-op", []() -> int {
        throw std::bad_alloc();
    });
    EXPECT_FALSE(r.ok());
    // Severity is Fatal and code is RESOURCE_EXHAUSTED per the catch
    // clause.
    EXPECT_TRUE(hasDiag(ctx->diag(), oreo::Severity::Fatal,
                        oreo::ErrorCode::RESOURCE_EXHAUSTED));
}

TEST(OcctSafeCall, StandardFailureConvertsToOcctFailure) {
    auto ctx = oreo::KernelContext::create();
    oreo::DiagnosticScope scope(*ctx);
    auto r = oreo::occtSafeCall(*ctx, scope, "occt-op", []() -> int {
        // Throw an OCCT exception directly. The catch handler in
        // occtSafeCall matches `const Standard_Failure&` and stamps an
        // OCCT_FAILURE diagnostic.
        throw Standard_Failure("simulated OCCT failure");
        return 0;  // unreachable
    });
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(hasDiag(ctx->diag(), oreo::Severity::Error,
                        oreo::ErrorCode::OCCT_FAILURE));
}

// ═══════════════════════════════════════════════════════════════
// Validation — requireTolerance
// ═══════════════════════════════════════════════════════════════

TEST(ValidationTolerance, RejectsNaN) {
    auto ctx = oreo::KernelContext::create();
    EXPECT_FALSE(oreo::validation::requireTolerance(*ctx, NaND, "tol"));
    EXPECT_TRUE(ctx->diag().hasErrors());
}

TEST(ValidationTolerance, RejectsInf) {
    auto ctx = oreo::KernelContext::create();
    EXPECT_FALSE(oreo::validation::requireTolerance(*ctx, InfD, "tol"));
    EXPECT_TRUE(ctx->diag().hasErrors());
}

TEST(ValidationTolerance, RejectsNegative) {
    auto ctx = oreo::KernelContext::create();
    EXPECT_FALSE(oreo::validation::requireTolerance(*ctx, -1e-6, "tol"));
    EXPECT_TRUE(ctx->diag().hasErrors());
}

TEST(ValidationTolerance, AcceptsMiddleOfDefaultRange) {
    auto ctx = oreo::KernelContext::create();
    EXPECT_TRUE(oreo::validation::requireTolerance(*ctx, 0.5, "tol"));
    EXPECT_FALSE(ctx->diag().hasErrors());
}

TEST(ValidationTolerance, RejectsBelowMinAllowed) {
    auto ctx = oreo::KernelContext::create();
    // Default minAllowed is 1e-12; 1e-15 is below that.
    EXPECT_FALSE(oreo::validation::requireTolerance(*ctx, 1e-15, "tol"));
    EXPECT_TRUE(ctx->diag().hasErrors());
}

// ═══════════════════════════════════════════════════════════════
// Validation — requirePerpendicular
// ═══════════════════════════════════════════════════════════════

TEST(ValidationPerpendicular, OrthogonalAxesAccepted) {
    auto ctx = oreo::KernelContext::create();
    EXPECT_TRUE(oreo::validation::requirePerpendicular(
        *ctx, gp_Dir(1, 0, 0), gp_Dir(0, 1, 0), 1e-6, "dirs"));
    EXPECT_FALSE(ctx->diag().hasErrors());
}

TEST(ValidationPerpendicular, ParallelAxesRejected) {
    auto ctx = oreo::KernelContext::create();
    EXPECT_FALSE(oreo::validation::requirePerpendicular(
        *ctx, gp_Dir(1, 0, 0), gp_Dir(1, 0, 0), 1e-6, "dirs"));
    EXPECT_TRUE(ctx->diag().hasErrors());
}

// ═══════════════════════════════════════════════════════════════
// Validation — requireCoplanar
// ═══════════════════════════════════════════════════════════════

TEST(ValidationCoplanar, FourPointsInXYPlaneAccepted) {
    auto ctx = oreo::KernelContext::create();
    std::vector<gp_Pnt> pts = {
        gp_Pnt(0, 0, 0),
        gp_Pnt(1, 0, 0),
        gp_Pnt(0, 1, 0),
        gp_Pnt(1, 1, 0),
    };
    EXPECT_TRUE(oreo::validation::requireCoplanar(*ctx, pts, 1e-6, "pts"));
    EXPECT_FALSE(ctx->diag().hasErrors());
}

TEST(ValidationCoplanar, OutOfPlanePointRejectedAtTightTol) {
    auto ctx = oreo::KernelContext::create();
    std::vector<gp_Pnt> pts = {
        gp_Pnt(0, 0, 0),
        gp_Pnt(1, 0, 0),
        gp_Pnt(0, 1, 0),
        gp_Pnt(1, 1, 0),
        gp_Pnt(0.5, 0.5, 0.5),  // z = 0.5 — well off the XY plane
    };
    EXPECT_FALSE(oreo::validation::requireCoplanar(*ctx, pts, 1e-6, "pts"));
    EXPECT_TRUE(ctx->diag().hasErrors());
}

TEST(ValidationCoplanar, OutOfPlanePointAcceptedAtLooseTol) {
    auto ctx = oreo::KernelContext::create();
    std::vector<gp_Pnt> pts = {
        gp_Pnt(0, 0, 0),
        gp_Pnt(1, 0, 0),
        gp_Pnt(0, 1, 0),
        gp_Pnt(1, 1, 0),
        gp_Pnt(0.5, 0.5, 0.5),
    };
    // At tol = 1.0 the 0.5 offset is within bounds.
    EXPECT_TRUE(oreo::validation::requireCoplanar(*ctx, pts, 1.0, "pts"));
}

// ═══════════════════════════════════════════════════════════════
// Validation — requireCollinear
// ═══════════════════════════════════════════════════════════════

TEST(ValidationCollinear, ThreePointsOnXAxisAccepted) {
    auto ctx = oreo::KernelContext::create();
    std::vector<gp_Pnt> pts = {
        gp_Pnt(0, 0, 0),
        gp_Pnt(1, 0, 0),
        gp_Pnt(2, 0, 0),
    };
    EXPECT_TRUE(oreo::validation::requireCollinear(*ctx, pts, 1e-6, "pts"));
    EXPECT_FALSE(ctx->diag().hasErrors());
}

TEST(ValidationCollinear, OffAxisPointRejected) {
    auto ctx = oreo::KernelContext::create();
    std::vector<gp_Pnt> pts = {
        gp_Pnt(0, 0, 0),
        gp_Pnt(1, 0, 0),
        gp_Pnt(2, 0, 0),
        gp_Pnt(3, 0.5, 0),  // off-axis
    };
    EXPECT_FALSE(oreo::validation::requireCollinear(*ctx, pts, 1e-6, "pts"));
    EXPECT_TRUE(ctx->diag().hasErrors());
}

// ═══════════════════════════════════════════════════════════════
// Validation — requireAllPositive
// ═══════════════════════════════════════════════════════════════

TEST(ValidationAllPositive, AcceptsAllPositive) {
    auto ctx = oreo::KernelContext::create();
    double vals[] = {1.0, 2.5, 3.14, 42.0};
    EXPECT_TRUE(oreo::validation::requireAllPositive(
        *ctx, vals, 4, "arr"));
    EXPECT_FALSE(ctx->diag().hasErrors());
}

TEST(ValidationAllPositive, RejectsNegativeAndReportsIndex) {
    auto ctx = oreo::KernelContext::create();
    double vals[] = {1.0, 2.0, -3.0, 4.0};
    EXPECT_FALSE(oreo::validation::requireAllPositive(
        *ctx, vals, 4, "arr"));
    EXPECT_TRUE(ctx->diag().hasErrors());
    EXPECT_TRUE(anyMessageContains(ctx->diag(), "[2]"));
}

TEST(ValidationAllPositive, RejectsNaNAndReportsIndex) {
    auto ctx = oreo::KernelContext::create();
    double vals[] = {1.0, NaND, 3.0};
    EXPECT_FALSE(oreo::validation::requireAllPositive(
        *ctx, vals, 3, "arr"));
    EXPECT_TRUE(ctx->diag().hasErrors());
    EXPECT_TRUE(anyMessageContains(ctx->diag(), "[1]"));
}

// ═══════════════════════════════════════════════════════════════
// SchemaRegistry — introspection
// ═══════════════════════════════════════════════════════════════

TEST(SchemaRegistryIntrospection, MaxMigrationStepsRoundTrip) {
    oreo::SchemaRegistry reg;
    EXPECT_GT(reg.maxMigrationSteps(), 0u);
    reg.setMaxMigrationSteps(17);
    EXPECT_EQ(reg.maxMigrationSteps(), 17u);
    reg.setMaxMigrationSteps(1);
    EXPECT_EQ(reg.maxMigrationSteps(), 1u);
}

TEST(SchemaRegistryIntrospection, RegisteredTypesContainsBuiltIns) {
    oreo::SchemaRegistry reg;
    auto types = reg.registeredTypes();
    // At minimum the built-ins registered by the ctor must be there.
    auto has = [&](const std::string& name) {
        return std::find(types.begin(), types.end(), name) != types.end();
    };
    EXPECT_TRUE(has(oreo::schema::TYPE_FEATURE_TREE));
    EXPECT_TRUE(has(oreo::schema::TYPE_FEATURE));
    EXPECT_TRUE(has(oreo::schema::TYPE_ELEMENT_MAP));
    EXPECT_TRUE(has(oreo::schema::TYPE_NAMED_SHAPE));
}

TEST(SchemaRegistryIntrospection, VersionsForListsRegisteredMigrations) {
    oreo::SchemaRegistry reg;
    const std::string type = oreo::schema::TYPE_FEATURE_TREE;

    // No migrations registered yet — empty list.
    EXPECT_TRUE(reg.versionsFor(type).empty());

    reg.registerMigration(type, oreo::SchemaVersion{0, 1, 0},
                          oreo::SchemaVersion{0, 2, 0},
                          [](const nlohmann::json& d) { return d; });
    reg.registerMigration(type, oreo::SchemaVersion{0, 2, 0},
                          oreo::SchemaVersion{1, 0, 0},
                          [](const nlohmann::json& d) { return d; });

    auto vs = reg.versionsFor(type);
    ASSERT_EQ(vs.size(), 2u);
    EXPECT_EQ(vs[0], (oreo::SchemaVersion{0, 1, 0}));
    EXPECT_EQ(vs[1], (oreo::SchemaVersion{0, 2, 0}));
}

TEST(SchemaRegistryIntrospection, UnregisterMigrationRemovesAndReports) {
    oreo::SchemaRegistry reg;
    const std::string type = oreo::schema::TYPE_FEATURE_TREE;
    reg.registerMigration(type, oreo::SchemaVersion{0, 1, 0},
                          oreo::SchemaVersion{1, 0, 0},
                          [](const nlohmann::json& d) { return d; });

    EXPECT_TRUE(reg.unregisterMigration(type, oreo::SchemaVersion{0, 1, 0}));
    // Second call: already gone, returns false.
    EXPECT_FALSE(reg.unregisterMigration(type, oreo::SchemaVersion{0, 1, 0}));

    // versionsFor now empty again.
    EXPECT_TRUE(reg.versionsFor(type).empty());
}

// ═══════════════════════════════════════════════════════════════
// SchemaRegistry — testMigration round-trip
// ═══════════════════════════════════════════════════════════════

TEST(SchemaRegistryTestMigration, HappyPathReturnsTrue) {
    // Note: SchemaRegistry::migrate() fails closed on unknown types —
    // currentVersion(type) throws for anything outside the built-in set.
    // We therefore attach the migration to a built-in type whose current
    // version is 1.0.0. A 0.1.0 -> 1.0.0 migrator routes fine through
    // migrate(), which is the machinery testMigration() drives.
    oreo::SchemaRegistry reg;
    const std::string type = oreo::schema::TYPE_FEATURE_TREE;  // current = 1.0.0
    reg.registerMigration(type, oreo::SchemaVersion{0, 1, 0},
                          oreo::SchemaVersion{1, 0, 0},
                          [](const nlohmann::json& in) {
                              nlohmann::json out = in;
                              out["migrated"] = true;
                              return out;
                          });

    nlohmann::json sample = {{"payload", 123}};
    std::string reason;
    EXPECT_TRUE(reg.testMigration(type,
                                  oreo::SchemaVersion{0, 1, 0},
                                  oreo::SchemaVersion{1, 0, 0},
                                  sample,
                                  &reason));
    EXPECT_EQ(reason, "ok");
}

TEST(SchemaRegistryTestMigration, MismatchingTargetReturnsFalseWithReason) {
    oreo::SchemaRegistry reg;
    const std::string type = oreo::schema::TYPE_FEATURE_TREE;  // current = 1.0.0
    reg.registerMigration(type, oreo::SchemaVersion{0, 1, 0},
                          oreo::SchemaVersion{1, 0, 0},
                          [](const nlohmann::json& d) { return d; });

    nlohmann::json sample = {{"payload", 1}};
    std::string reason;
    // Claim the target is 9.9.9 — migrate() will take the data to 1.0.0
    // (the real current version), so the shape-check fails.
    EXPECT_FALSE(reg.testMigration(type,
                                   oreo::SchemaVersion{0, 1, 0},
                                   oreo::SchemaVersion{9, 9, 9},
                                   sample,
                                   &reason));
    EXPECT_FALSE(reason.empty());
    EXPECT_NE(reason, "ok");
}

// ═══════════════════════════════════════════════════════════════
// Diagnostic metadata (D-9)
// ═══════════════════════════════════════════════════════════════

TEST(DiagnosticMetadata, ErrorStampsTimestampThreadAndContext) {
    auto ctx = oreo::KernelContext::create();
    ctx->diag().error(oreo::ErrorCode::OCCT_FAILURE, "stamp-me");

    ASSERT_FALSE(ctx->diag().all().empty());
    const auto& d = ctx->diag().all().back();
    EXPECT_GT(d.timestampNs, 0ull);
    EXPECT_NE(d.threadId, 0ull);
    EXPECT_EQ(d.contextId, ctx->numericId());
}
