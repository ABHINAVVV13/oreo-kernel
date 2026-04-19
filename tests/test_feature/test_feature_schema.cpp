// SPDX-License-Identifier: LGPL-2.1-or-later

// test_feature_schema.cpp — Validates the FeatureSchemaRegistry catalog
// and the validateFeature() pre-execution gate that runs from
// executeFeature() in src/feature/feature.cpp.

#include <gtest/gtest.h>

#include "core/diagnostic.h"
#include "core/kernel_context.h"
#include "feature/feature.h"
#include "feature/feature_schema.h"

#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <limits>

using namespace oreo;

// ─── Registry catalog ─────────────────────────────────────────

TEST(FeatureSchema, RegistryListsAllExpectedTypes) {
    auto types = FeatureSchemaRegistry::instance().registeredTypes();
    // Snapshot of the full feature taxonomy. If a feature type is added
    // or removed, update this list AND the dispatch in feature.cpp.
    const std::vector<std::string> expected = {
        "BooleanIntersect", "BooleanSubtract", "BooleanUnion",
        "Chamfer", "CircularPattern", "Combine",
        "Draft", "Extrude", "Fillet", "Hole", "LinearPattern", "Loft",
        "MakeBox", "MakeCone", "MakeCylinder", "MakeFace", "MakeSphere",
        "MakeTorus", "MakeWedge", "Mirror",
        "Offset", "Pocket", "Revolve", "Rib", "Shell", "SplitBody",
        "Sweep", "Thicken", "VariableFillet"
    };
    EXPECT_EQ(types, expected);
}

TEST(FeatureSchema, FindReturnsNullForUnknownType) {
    EXPECT_EQ(FeatureSchemaRegistry::instance().find("FlibbleWidget"), nullptr);
}

TEST(FeatureSchema, ExtrudeSchemaShape) {
    auto* s = FeatureSchemaRegistry::instance().find("Extrude");
    ASSERT_NE(s, nullptr);
    EXPECT_TRUE(s->requiresBaseShape);
    ASSERT_EQ(s->params.size(), 1u);
    EXPECT_EQ(s->params[0].name, "direction");
    EXPECT_EQ(s->params[0].type, ParamType::Vec);
}

// ─── validateFeature: type-level errors ───────────────────────

TEST(FeatureSchema, UnknownFeatureTypeFails) {
    auto ctx = KernelContext::create();
    Feature f; f.id = "F1"; f.type = "DefinitelyNotAFeature";
    EXPECT_FALSE(validateFeature(*ctx, f));
    EXPECT_EQ(f.status, FeatureStatus::ExecutionFailed);
    EXPECT_TRUE(ctx->diag().hasErrors());
}

TEST(FeatureSchema, MissingRequiredParamFails) {
    auto ctx = KernelContext::create();
    Feature f; f.id = "F1"; f.type = "Extrude";
    EXPECT_FALSE(validateFeature(*ctx, f));
    EXPECT_EQ(f.status, FeatureStatus::ExecutionFailed);
    EXPECT_NE(f.errorMessage.find("direction"), std::string::npos);
}

TEST(FeatureSchema, WrongVariantTypeFails) {
    auto ctx = KernelContext::create();
    Feature f; f.id = "F1"; f.type = "Extrude";
    f.params["direction"] = std::string("not-a-vec");
    EXPECT_FALSE(validateFeature(*ctx, f));
    EXPECT_EQ(f.status, FeatureStatus::ExecutionFailed);
    EXPECT_NE(f.errorMessage.find("expected Vec"), std::string::npos);
}

// ─── Numeric range / NaN handling ─────────────────────────────

TEST(FeatureSchema, MakeSpherePositiveRadiusOK) {
    auto ctx = KernelContext::create();
    Feature f; f.type = "MakeSphere";
    f.params["radius"] = 5.0;
    EXPECT_TRUE(validateFeature(*ctx, f));
    EXPECT_FALSE(ctx->diag().hasErrors());
}

TEST(FeatureSchema, MakeSphereNegativeRadiusFails) {
    auto ctx = KernelContext::create();
    Feature f; f.type = "MakeSphere";
    f.params["radius"] = -1.0;
    EXPECT_FALSE(validateFeature(*ctx, f));
    EXPECT_EQ(f.status, FeatureStatus::ExecutionFailed);
}

TEST(FeatureSchema, MakeSphereZeroRadiusFails) {
    auto ctx = KernelContext::create();
    Feature f; f.type = "MakeSphere";
    f.params["radius"] = 0.0;  // dblPositive forbids zero too
    EXPECT_FALSE(validateFeature(*ctx, f));
}

TEST(FeatureSchema, NaNDoubleFails) {
    auto ctx = KernelContext::create();
    Feature f; f.type = "MakeSphere";
    f.params["radius"] = std::nan("");
    EXPECT_FALSE(validateFeature(*ctx, f));
}

TEST(FeatureSchema, InfDoubleFails) {
    auto ctx = KernelContext::create();
    Feature f; f.type = "MakeSphere";
    f.params["radius"] = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(validateFeature(*ctx, f));
}

TEST(FeatureSchema, RevolveAngleOutOfRangeFails) {
    auto ctx = KernelContext::create();
    Feature f; f.type = "Revolve";
    f.params["axis"]  = gp_Ax1(gp_Pnt(0,0,0), gp_Dir(0,0,1));
    f.params["angle"] = 100.0;  // ~16π — surely meant degrees
    EXPECT_FALSE(validateFeature(*ctx, f));
}

TEST(FeatureSchema, IntCountTooSmallFails) {
    auto ctx = KernelContext::create();
    Feature f; f.type = "LinearPattern";
    f.params["direction"] = gp_Vec(1,0,0);
    f.params["count"]     = 0;  // intMin=1
    f.params["spacing"]   = 5.0;
    EXPECT_FALSE(validateFeature(*ctx, f));
}

// ─── Vec validation ────────────────────────────────────────────

TEST(FeatureSchema, ExtrudeZeroVectorFails) {
    auto ctx = KernelContext::create();
    Feature f; f.type = "Extrude";
    f.params["direction"] = gp_Vec(0, 0, 0);
    EXPECT_FALSE(validateFeature(*ctx, f));
}

TEST(FeatureSchema, ExtrudeNaNVectorFails) {
    auto ctx = KernelContext::create();
    Feature f; f.type = "Extrude";
    f.params["direction"] = gp_Vec(std::nan(""), 0, 0);
    EXPECT_FALSE(validateFeature(*ctx, f));
}

TEST(FeatureSchema, MakeBoxNegativeDimensionFails) {
    auto ctx = KernelContext::create();
    Feature f; f.type = "MakeBox";
    f.params["dimensions"] = gp_Vec(10, -1, 5);  // dy<0 rejected
    EXPECT_FALSE(validateFeature(*ctx, f));
}

TEST(FeatureSchema, MakeBoxAllPositiveOK) {
    auto ctx = KernelContext::create();
    Feature f; f.type = "MakeBox";
    f.params["dimensions"] = gp_Vec(10, 20, 30);
    EXPECT_TRUE(validateFeature(*ctx, f));
}

// ─── ElementRef validation ────────────────────────────────────

TEST(FeatureSchema, FilletEmptyEdgeListFails) {
    auto ctx = KernelContext::create();
    Feature f; f.type = "Fillet";
    f.params["edges"]  = std::vector<ElementRef>{};
    f.params["radius"] = 1.5;
    EXPECT_FALSE(validateFeature(*ctx, f));
}

TEST(FeatureSchema, FilletElementRefMissingFeatureIdFails) {
    auto ctx = KernelContext::create();
    Feature f; f.type = "Fillet";
    ElementRef bad;
    bad.elementName = "Edge1";
    bad.elementType = "Edge";
    f.params["edges"]  = std::vector<ElementRef>{bad};
    f.params["radius"] = 1.5;
    EXPECT_FALSE(validateFeature(*ctx, f));
}

TEST(FeatureSchema, FilletWrongElementKindFails) {
    auto ctx = KernelContext::create();
    Feature f; f.type = "Fillet";
    ElementRef ref;
    ref.featureId = "F1";
    ref.elementName = "Vert1";
    ref.elementType = "Vertex";  // schema requires "Edge"
    f.params["edges"]  = std::vector<ElementRef>{ref};
    f.params["radius"] = 1.5;
    EXPECT_FALSE(validateFeature(*ctx, f));
}

TEST(FeatureSchema, FilletHappyPath) {
    auto ctx = KernelContext::create();
    Feature f; f.type = "Fillet";
    ElementRef ref;
    ref.featureId = "F1";
    ref.elementName = "Edge1";
    ref.elementType = "Edge";
    f.params["edges"]  = std::vector<ElementRef>{ref};
    f.params["radius"] = 1.5;
    EXPECT_TRUE(validateFeature(*ctx, f));
}

TEST(FeatureSchema, BooleanUnionRefAnyKindOK) {
    auto ctx = KernelContext::create();
    Feature f; f.type = "BooleanUnion";
    ElementRef ref; ref.featureId = "F2"; ref.elementName = "Solid";
    ref.elementType = "";  // unspecified — schema "" allows any
    f.params["tool"] = ref;
    EXPECT_TRUE(validateFeature(*ctx, f));
}

// ─── Forward-compat warning behaviour ─────────────────────────

TEST(FeatureSchema, UnknownExtraParamWarnsButPasses) {
    auto ctx = KernelContext::create();
    Feature f; f.type = "MakeSphere";
    f.params["radius"]  = 5.0;
    f.params["future_v2_field"] = std::string("something");
    EXPECT_TRUE(validateFeature(*ctx, f));
    EXPECT_FALSE(ctx->diag().hasErrors());
    EXPECT_TRUE(ctx->diag().hasWarnings());
}

// ─── Integration: executeFeature gate ─────────────────────────

TEST(FeatureSchema, ExecuteFeatureRejectsBadInput) {
    auto ctx = KernelContext::create();
    Feature f; f.id = "F1"; f.type = "Extrude";
    // No direction param — will fail validation.
    NamedShape empty;
    auto result = executeFeature(*ctx, f, empty,
        [](const ElementRef&) { return ResolvedRef{}; });
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(f.status, FeatureStatus::ExecutionFailed);
}

TEST(FeatureSchema, ExecuteFeatureSuppressedSkipsValidation) {
    auto ctx = KernelContext::create();
    Feature f; f.id = "F1"; f.type = "Extrude";
    f.suppressed = true;
    // Missing direction would normally fail validation, but suppressed
    // features short-circuit before validateFeature runs.
    NamedShape empty;
    auto result = executeFeature(*ctx, f, empty,
        [](const ElementRef&) { return ResolvedRef{}; });
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(f.status, FeatureStatus::Suppressed);
}
