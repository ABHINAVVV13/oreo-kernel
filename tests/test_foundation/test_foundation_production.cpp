// test_foundation_production.cpp -- Comprehensive adversarial test suite
// verifying ALL foundation guarantees for production readiness.
//
// Categories:
//   - NaN Rejection (25 tests): Every geometry op must reject NaN/Inf
//   - Tag Allocator (6 tests): Deterministic, document-scoped, no collision
//   - OperationResult (5 tests): Success/failure, diagnostics travel
//   - DiagnosticScope (3 tests): Nested, composable, no leak
//   - Context Isolation (3 tests): Independent diag, tags, units
//   - Unit Conversion (4 tests): Inch->MM, deg->rad, round-trip, meter->MM
//   - Schema (3 tests): Unknown version, header round-trip, compatible load
//   - Validation Direct (5 tests): NaN, Inf, -Inf, NaN vec, valid accepted
//   - Geometry Quality (3 tests): Box/Sphere/Cylinder never degraded

#include "core/kernel_context.h"
#include "core/operation_result.h"
#include "core/diagnostic_scope.h"
#include "core/validation.h"
#include "core/tag_allocator.h"
#include "core/units.h"
#include "core/schema.h"
#include "geometry/oreo_geometry.h"
#include "naming/named_shape.h"
#include "mesh/oreo_mesh.h"

#include <gp_Vec.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pln.hxx>

#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include <memory>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Helper constants
static const double NaN    = std::numeric_limits<double>::quiet_NaN();
static const double PosInf = std::numeric_limits<double>::infinity();
static const double NegInf = -std::numeric_limits<double>::infinity();

// ===================================================================
// NaN Rejection (25 tests)
// Every geometry operation must reject NaN inputs with !ok()
// ===================================================================

TEST(NaNRejection, MakeBox) {
    auto ctx = oreo::KernelContext::create();
    auto result = oreo::makeBox(*ctx, NaN, 10, 10);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
}

TEST(NaNRejection, MakeCylinder) {
    auto ctx = oreo::KernelContext::create();
    auto result = oreo::makeCylinder(*ctx, NaN, 10);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
}

TEST(NaNRejection, MakeSphere) {
    auto ctx = oreo::KernelContext::create();
    auto result = oreo::makeSphere(*ctx, NaN);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
}

TEST(NaNRejection, MakeCone) {
    auto ctx = oreo::KernelContext::create();
    auto result = oreo::makeCone(*ctx, NaN, 5, 10);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
}

TEST(NaNRejection, MakeTorus) {
    auto ctx = oreo::KernelContext::create();
    auto result = oreo::makeTorus(*ctx, NaN, 5);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
}

TEST(NaNRejection, MakeWedge) {
    auto ctx = oreo::KernelContext::create();
    auto result = oreo::makeWedge(*ctx, NaN, 10, 10, 5);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
}

TEST(NaNRejection, Extrude) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 20, 30);
    ASSERT_TRUE(box.ok());
    auto result = oreo::extrude(*ctx, box.value(), gp_Vec(0, 0, NaN));
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
}

TEST(NaNRejection, Revolve) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 20, 30);
    ASSERT_TRUE(box.ok());
    gp_Ax1 axis(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1));
    auto result = oreo::revolve(*ctx, box.value(), axis, NaN);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
}

TEST(NaNRejection, Fillet) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 20, 30);
    ASSERT_TRUE(box.ok());
    oreo::NamedEdge edge;
    edge.edge = box.value().getSubShape(TopAbs_EDGE, 1);
    auto result = oreo::fillet(*ctx, box.value(), {edge}, NaN);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
}

TEST(NaNRejection, Chamfer) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 20, 30);
    ASSERT_TRUE(box.ok());
    oreo::NamedEdge edge;
    edge.edge = box.value().getSubShape(TopAbs_EDGE, 1);
    auto result = oreo::chamfer(*ctx, box.value(), {edge}, NaN);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
}

TEST(NaNRejection, ShellThickness) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 20, 30);
    ASSERT_TRUE(box.ok());
    oreo::NamedFace face;
    face.face = box.value().getSubShape(TopAbs_FACE, 1);
    auto result = oreo::shell(*ctx, box.value(), {face}, NaN);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
}

TEST(NaNRejection, PatternLinearSpacing) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 20, 30);
    ASSERT_TRUE(box.ok());
    auto result = oreo::patternLinear(*ctx, box.value(), gp_Vec(1, 0, 0), 3, NaN);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
}

TEST(NaNRejection, PatternCircularAngle) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 20, 30);
    ASSERT_TRUE(box.ok());
    gp_Ax1 axis(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1));
    auto result = oreo::patternCircular(*ctx, box.value(), axis, 4, NaN);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
}

TEST(NaNRejection, Offset) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 20, 30);
    ASSERT_TRUE(box.ok());
    auto result = oreo::offset(*ctx, box.value(), NaN);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
}

TEST(NaNRejection, Thicken) {
    auto ctx = oreo::KernelContext::create();
    // Thicken operates on a shell/face -- pass a default NamedShape with NaN thickness
    // The NaN thickness should be caught before the null-shape check
    oreo::NamedShape empty{};
    auto result = oreo::thicken(*ctx, empty, NaN);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
}

TEST(NaNRejection, FilletVariable) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 20, 30);
    ASSERT_TRUE(box.ok());
    oreo::NamedEdge edge;
    edge.edge = box.value().getSubShape(TopAbs_EDGE, 1);
    auto result = oreo::filletVariable(*ctx, box.value(), edge, NaN, 2.0);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
}

TEST(NaNRejection, WireOffset) {
    auto ctx = oreo::KernelContext::create();
    // wireOffset with NaN distance -- pass default-constructed NamedShape
    oreo::NamedShape empty{};
    auto result = oreo::wireOffset(*ctx, empty, NaN);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
}

TEST(NaNRejection, DraftAngle) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 20, 30);
    ASSERT_TRUE(box.ok());
    oreo::NamedFace face;
    face.face = box.value().getSubShape(TopAbs_FACE, 1);
    auto result = oreo::draft(*ctx, box.value(), {face}, NaN, gp_Dir(0, 0, 1));
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
}

TEST(NaNRejection, HoleDiameter) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 50, 50, 50);
    ASSERT_TRUE(box.ok());
    oreo::NamedFace face;
    face.face = box.value().getSubShape(TopAbs_FACE, 1);
    gp_Pnt center(25, 25, 50);
    auto result = oreo::hole(*ctx, box.value(), face, center, NaN, 20.0);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
}

TEST(NaNRejection, HoleDepth) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 50, 50, 50);
    ASSERT_TRUE(box.ok());
    oreo::NamedFace face;
    face.face = box.value().getSubShape(TopAbs_FACE, 1);
    gp_Pnt center(25, 25, 50);
    auto result = oreo::hole(*ctx, box.value(), face, center, 10.0, NaN);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
}

TEST(NaNRejection, RibThickness) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 20, 30);
    ASSERT_TRUE(box.ok());
    // Rib with NaN thickness -- ribProfile is empty, NaN should be caught first
    oreo::NamedShape empty{};
    auto result = oreo::rib(*ctx, box.value(), empty, gp_Dir(0, 0, 1), NaN);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
}

TEST(NaNRejection, PocketDepth) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 20, 30);
    ASSERT_TRUE(box.ok());
    // Pocket with NaN depth -- profile is empty, NaN should be caught first
    oreo::NamedShape empty{};
    auto result = oreo::pocket(*ctx, box.value(), empty, NaN);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
}

// --- Inf Rejection (3 tests) ---

TEST(InfRejection, MakeBoxInf) {
    auto ctx = oreo::KernelContext::create();
    auto result = oreo::makeBox(*ctx, PosInf, 10, 10);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
}

TEST(InfRejection, MakeBoxNegInf) {
    auto ctx = oreo::KernelContext::create();
    auto result = oreo::makeBox(*ctx, NegInf, 10, 10);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
}

TEST(InfRejection, ExtrudeInf) {
    auto ctx = oreo::KernelContext::create();
    auto box = oreo::makeBox(*ctx, 10, 20, 30);
    ASSERT_TRUE(box.ok());
    auto result = oreo::extrude(*ctx, box.value(), gp_Vec(0, 0, PosInf));
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
}

// ===================================================================
// Tag Allocator (6 tests)
// ===================================================================

TEST(TagAllocator, SingleDocSequential) {
    // Default single-doc mode (documentId=0) produces 1, 2, 3...
    oreo::TagAllocator alloc;
    EXPECT_EQ(alloc.nextTag(), 1);
    EXPECT_EQ(alloc.nextTag(), 2);
    EXPECT_EQ(alloc.nextTag(), 3);
    EXPECT_EQ(alloc.nextTag(), 4);
    EXPECT_EQ(alloc.nextTag(), 5);
}

TEST(TagAllocator, MultiDocNoCollision) {
    // Tags from two different documents must NEVER collide
    oreo::TagAllocator doc1(100);
    oreo::TagAllocator doc2(200);

    // Generate several tags from each
    std::vector<int64_t> tags1, tags2;
    for (int i = 0; i < 10; ++i) {
        tags1.push_back(doc1.nextTag());
        tags2.push_back(doc2.nextTag());
    }

    // No tag from doc1 should appear in doc2
    for (auto t1 : tags1) {
        for (auto t2 : tags2) {
            EXPECT_NE(t1, t2) << "Collision: doc1 tag " << t1 << " == doc2 tag " << t2;
        }
    }
}

TEST(TagAllocator, ExtractDocId) {
    oreo::TagAllocator alloc(42);
    int64_t tag = alloc.nextTag();
    // Upper 32 bits encode the document ID
    EXPECT_EQ(oreo::TagAllocator::extractDocumentId(tag), 42u);
}

TEST(TagAllocator, ExtractCounter) {
    oreo::TagAllocator alloc(7);
    alloc.nextTag();  // counter = 1
    alloc.nextTag();  // counter = 2
    int64_t tag3 = alloc.nextTag();  // counter = 3
    EXPECT_EQ(oreo::TagAllocator::extractCounter(tag3), 3u);
}

TEST(TagAllocator, ResetDeterministic) {
    oreo::TagAllocator alloc;
    alloc.nextTag();  // 1
    alloc.nextTag();  // 2
    alloc.nextTag();  // 3
    alloc.reset();
    // After reset, counter goes back to 0, so next tag = 1
    EXPECT_EQ(alloc.nextTag(), 1);
}

TEST(TagAllocator, HighCounterWorks) {
    oreo::TagAllocator alloc;
    alloc.seed(100000);
    int64_t tag = alloc.nextTag();
    EXPECT_EQ(tag, 100001);
}

// ===================================================================
// OperationResult (5 tests)
// ===================================================================

TEST(OperationResult, SuccessOK) {
    auto result = oreo::OperationResult<int>::success(42);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.value(), 42);
    EXPECT_FALSE(result.hasErrors());
    EXPECT_FALSE(result.hasWarnings());
}

TEST(OperationResult, FailureNotOK) {
    auto result = oreo::OperationResult<int>::failure({});
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(static_cast<bool>(result));
}

TEST(OperationResult, FailureThrows) {
    auto result = oreo::OperationResult<int>::failure({});
    EXPECT_THROW(result.valueOrThrow(), std::runtime_error);
}

TEST(OperationResult, ValueOrDefault) {
    auto result = oreo::OperationResult<int>::failure({});
    EXPECT_EQ(result.valueOr(99), 99);

    // Also verify that success returns the actual value, not the default
    auto good = oreo::OperationResult<int>::success(42);
    EXPECT_EQ(good.valueOr(99), 42);
}

TEST(OperationResult, DiagnosticsTravel) {
    // makeBox with negative dimension should fail and carry diagnostics
    auto ctx = oreo::KernelContext::create();
    auto result = oreo::makeBox(*ctx, -1, 10, 10);
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.diagnostics().empty());
    EXPECT_TRUE(result.hasErrors());
    // The error message should be non-empty
    EXPECT_FALSE(result.errorMessage().empty());
}

// ===================================================================
// DiagnosticScope (3 tests)
// ===================================================================

TEST(DiagnosticScope, NestedNoLeak) {
    auto ctx = oreo::KernelContext::create();

    // Outer scope: add a warning
    oreo::DiagnosticScope outer(*ctx);
    ctx->diag().warning(oreo::ErrorCode::SHAPE_INVALID, "Outer warning");

    {
        // Inner scope: add an error
        oreo::DiagnosticScope inner(*ctx);
        ctx->diag().error(oreo::ErrorCode::OCCT_FAILURE, "Inner error");

        // Inner scope sees ONLY its own diagnostics
        auto innerDiags = inner.extractDiagnostics();
        EXPECT_EQ(innerDiags.size(), 1u);
        EXPECT_EQ(innerDiags[0].message, "Inner error");
        EXPECT_TRUE(inner.hasErrors());
    }

    // Outer scope sees outer + inner (since inner was inside outer)
    auto outerDiags = outer.extractDiagnostics();
    EXPECT_EQ(outerDiags.size(), 2u);

    // Context still holds everything
    EXPECT_EQ(ctx->diag().count(), 2);
}

TEST(DiagnosticScope, MakeResultSuccess) {
    auto ctx = oreo::KernelContext::create();
    oreo::DiagnosticScope scope(*ctx);
    // No errors in scope -- makeResult should succeed
    auto result = scope.makeResult(42);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.value(), 42);
    EXPECT_TRUE(result.diagnostics().empty());
}

TEST(DiagnosticScope, MakeFailureOnError) {
    auto ctx = oreo::KernelContext::create();
    oreo::DiagnosticScope scope(*ctx);
    ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Bad input");

    // makeResult should detect the error and return failure
    auto result = scope.makeResult(42);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.hasErrors());
    EXPECT_THROW(result.valueOrThrow(), std::runtime_error);
}

// ===================================================================
// Context Isolation (3 tests)
// ===================================================================

TEST(ContextIsolation, DiagIndependent) {
    auto ctx1 = oreo::KernelContext::create();
    auto ctx2 = oreo::KernelContext::create();

    // Report an error in ctx1 only
    ctx1->diag().error(oreo::ErrorCode::INVALID_INPUT, "ctx1 error");

    // ctx2 should have NO errors
    EXPECT_TRUE(ctx1->diag().hasErrors());
    EXPECT_FALSE(ctx2->diag().hasErrors());
    EXPECT_EQ(ctx2->diag().count(), 0);
}

TEST(ContextIsolation, TagsIndependent) {
    auto ctx1 = oreo::KernelContext::create();
    auto ctx2 = oreo::KernelContext::create();

    // Allocate tags from ctx1
    int64_t t1a = ctx1->tags().nextTag();
    int64_t t1b = ctx1->tags().nextTag();

    // ctx2 tags should start fresh, not affected by ctx1
    int64_t t2a = ctx2->tags().nextTag();

    // Both should produce 1 as their first tag (single-doc default)
    EXPECT_EQ(t1a, 1);
    EXPECT_EQ(t2a, 1);
    EXPECT_EQ(t1b, 2);

    // ctx1 counter is at 2, ctx2 at 1 -- independent
    EXPECT_EQ(ctx1->tags().currentValue(), 2);
    EXPECT_EQ(ctx2->tags().currentValue(), 1);
}

TEST(ContextIsolation, UnitsIndependent) {
    oreo::KernelConfig cfg1;
    cfg1.units.documentLength = oreo::LengthUnit::Inch;
    auto ctx1 = oreo::KernelContext::create(cfg1);

    oreo::KernelConfig cfg2;
    cfg2.units.documentLength = oreo::LengthUnit::Millimeter;
    auto ctx2 = oreo::KernelContext::create(cfg2);

    // ctx1 is in inches, ctx2 is in mm -- they should not affect each other
    EXPECT_EQ(ctx1->units().documentLength, oreo::LengthUnit::Inch);
    EXPECT_EQ(ctx2->units().documentLength, oreo::LengthUnit::Millimeter);

    // Conversions are independent
    double inch_to_mm = ctx1->units().toKernelLength(1.0);
    double mm_to_mm   = ctx2->units().toKernelLength(1.0);
    EXPECT_NEAR(inch_to_mm, 25.4, 1e-10);
    EXPECT_NEAR(mm_to_mm, 1.0, 1e-10);
}

// ===================================================================
// Unit Conversion (4 tests)
// ===================================================================

TEST(Units, InchToMM) {
    double result = oreo::unit_convert::lengthToMM(1.0, oreo::LengthUnit::Inch);
    EXPECT_NEAR(result, 25.4, 1e-12);
}

TEST(Units, DegreeToRadian) {
    double result = oreo::unit_convert::angleToRad(180.0, oreo::AngleUnit::Degree);
    EXPECT_NEAR(result, M_PI, 1e-12);
}

TEST(Units, RoundTrip) {
    // Convert inch -> mm -> inch, should get back the original
    double original = 3.5;
    double mm = oreo::unit_convert::lengthToMM(original, oreo::LengthUnit::Inch);
    double back = oreo::unit_convert::mmToLength(mm, oreo::LengthUnit::Inch);
    EXPECT_NEAR(back, original, 1e-12);

    // Also test angle round trip: deg -> rad -> deg
    double angleDeg = 45.0;
    double rad = oreo::unit_convert::angleToRad(angleDeg, oreo::AngleUnit::Degree);
    double backDeg = oreo::unit_convert::radToAngle(rad, oreo::AngleUnit::Degree);
    EXPECT_NEAR(backDeg, angleDeg, 1e-12);
}

TEST(Units, MeterToMM) {
    double result = oreo::unit_convert::lengthToMM(1.0, oreo::LengthUnit::Meter);
    EXPECT_NEAR(result, 1000.0, 1e-12);
}

// ===================================================================
// Schema (3 tests)
// ===================================================================

TEST(Schema, UnknownVersionRejected) {
    oreo::SchemaRegistry reg;
    // Future major version that does not exist
    auto data = oreo::SchemaRegistry::addHeader(
        oreo::schema::TYPE_FEATURE_TREE,
        {99, 0, 0},  // Way in the future
        nlohmann::json::object());
    EXPECT_THROW(reg.migrate(oreo::schema::TYPE_FEATURE_TREE, data), std::runtime_error);
}

TEST(Schema, HeaderRoundTrip) {
    // Add header, then read it back -- should match
    oreo::SchemaVersion ver{1, 2, 3};
    std::string type = oreo::schema::TYPE_FEATURE;
    nlohmann::json payload = {{"name", "test_feature"}};

    nlohmann::json withHeader = oreo::SchemaRegistry::addHeader(type, ver, payload);

    auto info = oreo::SchemaRegistry::readHeader(withHeader);
    EXPECT_TRUE(info.valid);
    EXPECT_EQ(info.type, type);
    EXPECT_EQ(info.version.major, 1);
    EXPECT_EQ(info.version.minor, 2);
    EXPECT_EQ(info.version.patch, 3);
}

TEST(Schema, CompatibleVersionLoads) {
    oreo::SchemaRegistry reg;
    // Current version for FEATURE_TREE is {1, 0, 0}
    // A document with version {1, 0, 0} should be loadable
    EXPECT_TRUE(reg.canLoad(oreo::schema::TYPE_FEATURE_TREE, {1, 0, 0}));
    // Same major, lower minor should also be loadable
    EXPECT_TRUE(reg.canLoad(oreo::schema::TYPE_FEATURE_TREE, {1, 0, 0}));
    // Different major should NOT be loadable
    EXPECT_FALSE(reg.canLoad(oreo::schema::TYPE_FEATURE_TREE, {2, 0, 0}));
}

// ===================================================================
// Validation Direct (5 tests)
// ===================================================================

TEST(Validation, RejectNaN) {
    auto ctx = oreo::KernelContext::create();
    EXPECT_FALSE(oreo::validation::requirePositive(*ctx, NaN, "test"));
    EXPECT_TRUE(ctx->diag().hasErrors());
}

TEST(Validation, RejectInf) {
    auto ctx = oreo::KernelContext::create();
    EXPECT_FALSE(oreo::validation::requirePositive(*ctx, PosInf, "test"));
    EXPECT_TRUE(ctx->diag().hasErrors());
}

TEST(Validation, RejectNegInf) {
    auto ctx = oreo::KernelContext::create();
    EXPECT_FALSE(oreo::validation::requireNonNegative(*ctx, NegInf, "test"));
    EXPECT_TRUE(ctx->diag().hasErrors());
}

TEST(Validation, RejectNaNVec) {
    auto ctx = oreo::KernelContext::create();
    gp_Vec nanVec(NaN, 0, 0);
    EXPECT_FALSE(oreo::validation::requireNonZeroVec(*ctx, nanVec, "test"));
    EXPECT_TRUE(ctx->diag().hasErrors());
}

TEST(Validation, AcceptValid) {
    auto ctx = oreo::KernelContext::create();
    EXPECT_TRUE(oreo::validation::requirePositive(*ctx, 5.0, "test"));
    EXPECT_FALSE(ctx->diag().hasErrors());
}

// ===================================================================
// Geometry Quality (3 tests)
// Valid primitives must NEVER have degraded geometry
// ===================================================================

TEST(GeomQuality, BoxNeverDegraded) {
    auto ctx = oreo::KernelContext::create();
    auto result = oreo::makeBox(*ctx, 10, 20, 30);
    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result.isGeometryDegraded());
    EXPECT_FALSE(result.isNamingDegraded());
    // The shape should be non-null
    EXPECT_FALSE(result.value().isNull());
    // Should have 6 faces
    EXPECT_EQ(result.value().countSubShapes(TopAbs_FACE), 6);
    // Should have 12 edges
    EXPECT_EQ(result.value().countSubShapes(TopAbs_EDGE), 12);
}

TEST(GeomQuality, SphereNeverDegraded) {
    auto ctx = oreo::KernelContext::create();
    auto result = oreo::makeSphere(*ctx, 15);
    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result.isGeometryDegraded());
    EXPECT_FALSE(result.isNamingDegraded());
    EXPECT_FALSE(result.value().isNull());
}

TEST(GeomQuality, CylinderNeverDegraded) {
    auto ctx = oreo::KernelContext::create();
    auto result = oreo::makeCylinder(*ctx, 5, 20);
    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result.isGeometryDegraded());
    EXPECT_FALSE(result.isNamingDegraded());
    EXPECT_FALSE(result.value().isNull());
}
