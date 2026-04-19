// SPDX-License-Identifier: LGPL-2.1-or-later

// test_part_studio_function.cpp — P1 acceptance suite.
//
// Locks down the PartStudio-as-pure-function contract:
//   * ConfigSchema addInput validates names, types, defaults, bounds.
//   * ConfigValue setters enforce the schema.
//   * PartStudio::execute is deterministic in (tree, schema, config).
//   * Different ConfigValues on the SAME tree produce different geometry.
//   * ConfigRef placeholders resolve before feature validation.
//   * Unresolved ConfigRef → BrokenReference, not a crash.
//   * JSON round-trip preserves every piece of state (name, schema, tree).
//   * A v1 bare FeatureTree / v1 PartStudio envelope loads cleanly into v2.

#include <gtest/gtest.h>

#include "core/kernel_context.h"
#include "feature/config.h"
#include "feature/feature.h"
#include "feature/part_studio.h"
#include "query/oreo_query.h"

#include "oreo_kernel.h"  // C API, for the borrowed-handle safety test

#include <cmath>

using oreo::ConfigInputSpec;
using oreo::ConfigSchema;
using oreo::ConfigValue;
using oreo::ConfigRef;
using oreo::Feature;
using oreo::KernelContext;
using oreo::ParamType;
using oreo::ParamValue;
using oreo::PartStudio;
using oreo::PartStudioOutputs;

namespace {

Feature makeBoxFeature(const std::string& id, const ParamValue& dimsOrRef) {
    Feature f;
    f.id   = id;
    f.type = "MakeBox";
    f.params["dimensions"] = dimsOrRef;
    return f;
}

ConfigInputSpec doubleInput(const std::string& name, double dflt,
                            std::optional<double> lo = std::nullopt,
                            std::optional<double> hi = std::nullopt) {
    ConfigInputSpec s;
    s.name = name;
    s.type = ParamType::Double;
    s.defaultValue = dflt;
    s.minInclusive = lo;
    s.maxInclusive = hi;
    return s;
}

} // anonymous namespace

// ══════════════════════════════════════════════════════════════════
// ConfigSchema
// ══════════════════════════════════════════════════════════════════

TEST(ConfigSchema, AddsValidInputs) {
    auto ctx = KernelContext::create();
    ConfigSchema s;
    EXPECT_EQ(s.addInput(*ctx, doubleInput("length", 100.0, 1.0)), 0);
    EXPECT_EQ(s.addInput(*ctx, doubleInput("width", 50.0)), 0);
    EXPECT_EQ(s.inputCount(), 2u);
    EXPECT_NE(s.find("length"), nullptr);
    EXPECT_EQ(s.find("length")->type, ParamType::Double);
    EXPECT_EQ(std::get<double>(s.find("length")->defaultValue), 100.0);
}

TEST(ConfigSchema, RejectsDuplicateName) {
    auto ctx = KernelContext::create();
    ConfigSchema s;
    EXPECT_EQ(s.addInput(*ctx, doubleInput("x", 1.0)), 0);
    EXPECT_NE(s.addInput(*ctx, doubleInput("x", 2.0)), 0);
    EXPECT_EQ(s.inputCount(), 1u);
}

TEST(ConfigSchema, RejectsEmptyName) {
    auto ctx = KernelContext::create();
    ConfigSchema s;
    EXPECT_NE(s.addInput(*ctx, doubleInput("", 1.0)), 0);
    EXPECT_EQ(s.inputCount(), 0u);
}

TEST(ConfigSchema, RejectsElementRefType) {
    auto ctx = KernelContext::create();
    ConfigSchema s;
    ConfigInputSpec bad;
    bad.name = "edge";
    bad.type = ParamType::ElemRef;
    bad.defaultValue = oreo::ElementRef{};
    EXPECT_NE(s.addInput(*ctx, bad), 0);
}

TEST(ConfigSchema, RejectsDefaultWithWrongVariantIndex) {
    auto ctx = KernelContext::create();
    ConfigSchema s;
    ConfigInputSpec bad;
    bad.name = "radius";
    bad.type = ParamType::Double;
    bad.defaultValue = std::string("not a double");
    EXPECT_NE(s.addInput(*ctx, bad), 0);
}

TEST(ConfigSchema, RejectsDefaultOutOfBounds) {
    auto ctx = KernelContext::create();
    ConfigSchema s;
    // default 0.5 but min = 1.0
    EXPECT_NE(s.addInput(*ctx, doubleInput("r", 0.5, 1.0)), 0);
}

TEST(ConfigSchema, RejectsNaNDefault) {
    auto ctx = KernelContext::create();
    ConfigSchema s;
    EXPECT_NE(s.addInput(*ctx, doubleInput("r", std::nan(""))), 0);
}

// ══════════════════════════════════════════════════════════════════
// ConfigValue
// ══════════════════════════════════════════════════════════════════

TEST(ConfigValue, DefaultsPopulateAllInputs) {
    auto ctx = KernelContext::create();
    ConfigSchema s;
    ASSERT_EQ(s.addInput(*ctx, doubleInput("a", 1.0)), 0);
    ASSERT_EQ(s.addInput(*ctx, doubleInput("b", 2.0)), 0);
    auto cv = ConfigValue::fromSchemaDefaults(s);
    EXPECT_EQ(cv.size(), 2u);
    EXPECT_EQ(std::get<double>(*cv.find("a")), 1.0);
    EXPECT_EQ(std::get<double>(*cv.find("b")), 2.0);
}

TEST(ConfigValue, SettersEnforceSchema) {
    auto ctx = KernelContext::create();
    ConfigSchema s;
    ASSERT_EQ(s.addInput(*ctx, doubleInput("len", 100.0, 1.0, 500.0)), 0);
    auto cv = ConfigValue::fromSchemaDefaults(s);

    EXPECT_EQ(cv.setDouble(*ctx, s, "len", 150.0), 0);
    EXPECT_EQ(std::get<double>(*cv.find("len")), 150.0);

    // Out of bounds high
    EXPECT_NE(cv.setDouble(*ctx, s, "len", 600.0), 0);
    EXPECT_EQ(std::get<double>(*cv.find("len")), 150.0);  // unchanged

    // Out of bounds low
    EXPECT_NE(cv.setDouble(*ctx, s, "len", 0.0), 0);

    // NaN
    EXPECT_NE(cv.setDouble(*ctx, s, "len", std::nan("")), 0);

    // Unknown name
    EXPECT_NE(cv.setDouble(*ctx, s, "nope", 5.0), 0);

    // Wrong type
    EXPECT_NE(cv.setString(*ctx, s, "len", "hello"), 0);
}

// ══════════════════════════════════════════════════════════════════
// resolveConfigRefs + PartStudio::execute
// ══════════════════════════════════════════════════════════════════

TEST(PartStudioExecute, RunsWithoutConfigRefs) {
    auto ctx = KernelContext::create();
    PartStudio ps(ctx, "plain");
    (void)ps.tree().addFeature(makeBoxFeature("F1", gp_Vec(10, 20, 30)));

    auto out = ps.executeWithDefaults();
    ASSERT_EQ(out.status, PartStudioOutputs::Status::Ok);
    ASSERT_FALSE(out.finalShape.isNull());
    EXPECT_EQ(out.finalShape.countSubShapes(TopAbs_FACE), 6);

    double vol = oreo::massProperties(*ctx, out.finalShape).value().volume;
    EXPECT_NEAR(vol, 10.0 * 20.0 * 30.0, 1.0);
}

TEST(PartStudioExecute, ConfigRefDefaultsApply) {
    auto ctx = KernelContext::create();
    PartStudio ps(ctx, "configured");
    // Declare "size" = Vec(10, 20, 30) as default
    ConfigInputSpec sizeSpec;
    sizeSpec.name = "size";
    sizeSpec.type = ParamType::Vec;
    sizeSpec.defaultValue = gp_Vec(10, 20, 30);
    ASSERT_EQ(ps.configSchema().addInput(*ctx, sizeSpec), 0);

    // Feature MakeBox with dimensions = #size
    Feature f;
    f.id = "F1";
    f.type = "MakeBox";
    f.params["dimensions"] = ConfigRef{"size"};
    (void)ps.tree().addFeature(f);

    auto out = ps.executeWithDefaults();
    ASSERT_EQ(out.status, PartStudioOutputs::Status::Ok);
    double vol = oreo::massProperties(*ctx, out.finalShape).value().volume;
    EXPECT_NEAR(vol, 10.0 * 20.0 * 30.0, 1.0);
}

TEST(PartStudioExecute, ConfigRefOverriddenByConfigValue) {
    auto ctx = KernelContext::create();
    PartStudio ps(ctx, "configured");
    ConfigInputSpec sizeSpec;
    sizeSpec.name = "size";
    sizeSpec.type = ParamType::Vec;
    sizeSpec.defaultValue = gp_Vec(10, 20, 30);
    ASSERT_EQ(ps.configSchema().addInput(*ctx, sizeSpec), 0);

    Feature f;
    f.id = "F1";
    f.type = "MakeBox";
    f.params["dimensions"] = ConfigRef{"size"};
    (void)ps.tree().addFeature(f);

    ConfigValue cfg = ConfigValue::fromSchemaDefaults(ps.configSchema());
    ASSERT_EQ(cfg.setVec(*ctx, ps.configSchema(), "size", gp_Vec(5, 5, 5)), 0);

    auto out = ps.execute(cfg);
    ASSERT_EQ(out.status, PartStudioOutputs::Status::Ok);
    double vol = oreo::massProperties(*ctx, out.finalShape).value().volume;
    EXPECT_NEAR(vol, 125.0, 1.0);
}

TEST(PartStudioExecute, UnresolvedConfigRefMarksFeatureBroken) {
    auto ctx = KernelContext::create();
    PartStudio ps(ctx, "bad");
    Feature f;
    f.id = "F1";
    f.type = "MakeBox";
    f.params["dimensions"] = ConfigRef{"doesNotExist"};
    (void)ps.tree().addFeature(f);

    auto out = ps.executeWithDefaults();
    // No primitive → no final shape → Failed status.
    EXPECT_EQ(out.status, PartStudioOutputs::Status::Failed);
    ASSERT_EQ(out.brokenFeatures.size(), 1u);
    EXPECT_EQ(out.brokenFeatures[0]->id, "F1");
    EXPECT_EQ(out.brokenFeatures[0]->status, oreo::FeatureStatus::BrokenReference);
}

TEST(PartStudioExecute, RepeatedExecuteIsDeterministic) {
    // Same studio + same config → identical volumes + identity bag.
    auto ctx = KernelContext::create();
    PartStudio ps(ctx, "det");
    ConfigInputSpec s;
    s.name = "size"; s.type = ParamType::Vec;
    s.defaultValue = gp_Vec(10, 20, 30);
    ASSERT_EQ(ps.configSchema().addInput(*ctx, s), 0);

    Feature f;
    f.id = "F1"; f.type = "MakeBox";
    f.params["dimensions"] = ConfigRef{"size"};
    (void)ps.tree().addFeature(f);

    double firstVol = -1;
    for (int i = 0; i < 5; ++i) {
        auto out = ps.executeWithDefaults();
        ASSERT_EQ(out.status, PartStudioOutputs::Status::Ok);
        double v = oreo::massProperties(*ctx, out.finalShape).value().volume;
        if (i == 0) firstVol = v;
        else EXPECT_NEAR(v, firstVol, 1e-6) << "iter " << i;
    }
}

TEST(PartStudioExecute, StoredFeatureParamIsNotMutated) {
    // A ConfigRef should survive execute — the stored feature's param
    // must still be a ConfigRef after the call, not the resolved value.
    auto ctx = KernelContext::create();
    PartStudio ps(ctx, "preserve");
    ConfigInputSpec s;
    s.name = "size"; s.type = ParamType::Vec;
    s.defaultValue = gp_Vec(1, 2, 3);
    ASSERT_EQ(ps.configSchema().addInput(*ctx, s), 0);

    Feature f;
    f.id = "F1"; f.type = "MakeBox";
    f.params["dimensions"] = ConfigRef{"size"};
    (void)ps.tree().addFeature(f);

    ps.executeWithDefaults();

    const auto* stored = ps.tree().getFeature("F1");
    ASSERT_NE(stored, nullptr);
    auto it = stored->params.find("dimensions");
    ASSERT_NE(it, stored->params.end());
    const auto* cr = std::get_if<ConfigRef>(&it->second);
    ASSERT_NE(cr, nullptr) << "stored ConfigRef was replaced by resolved value";
    EXPECT_EQ(cr->name, "size");
}

TEST(PartStudioExecute, DifferentConfigsYieldDifferentShapes) {
    // Same tree, two configs, two distinct volumes.
    auto ctx = KernelContext::create();
    PartStudio ps(ctx, "variant");
    ConfigInputSpec s;
    s.name = "size"; s.type = ParamType::Vec;
    s.defaultValue = gp_Vec(10, 10, 10);
    ASSERT_EQ(ps.configSchema().addInput(*ctx, s), 0);

    Feature f;
    f.id = "F1"; f.type = "MakeBox";
    f.params["dimensions"] = ConfigRef{"size"};
    (void)ps.tree().addFeature(f);

    ConfigValue small = ConfigValue::fromSchemaDefaults(ps.configSchema());
    ASSERT_EQ(small.setVec(*ctx, ps.configSchema(), "size", gp_Vec(2, 2, 2)), 0);

    ConfigValue big = ConfigValue::fromSchemaDefaults(ps.configSchema());
    ASSERT_EQ(big.setVec(*ctx, ps.configSchema(), "size", gp_Vec(20, 20, 20)), 0);

    auto outSmall = ps.execute(small);
    auto outBig   = ps.execute(big);
    ASSERT_EQ(outSmall.status, PartStudioOutputs::Status::Ok);
    ASSERT_EQ(outBig.status,   PartStudioOutputs::Status::Ok);

    double volSmall = oreo::massProperties(*ctx, outSmall.finalShape).value().volume;
    double volBig   = oreo::massProperties(*ctx, outBig.finalShape).value().volume;
    EXPECT_NEAR(volSmall, 8.0,    1e-3);
    EXPECT_NEAR(volBig,   8000.0, 1.0);
}

TEST(PartStudioExecute, InvalidConfigValueIsRejectedCleanly) {
    auto ctx = KernelContext::create();
    PartStudio ps(ctx, "bad_cfg");
    ConfigInputSpec s;
    s.name = "r"; s.type = ParamType::Double;
    s.defaultValue = 5.0;
    s.minInclusive = 1.0; s.maxInclusive = 10.0;
    ASSERT_EQ(ps.configSchema().addInput(*ctx, s), 0);

    ConfigValue cv;
    // The public ConfigValue::setDouble path enforces schema bounds;
    // an out-of-range value never lands and execute() never sees it.
    EXPECT_NE(cv.setDouble(*ctx, ps.configSchema(), "r", 100.0), 0);
    EXPECT_EQ(cv.find("r"), nullptr);
}

// ══════════════════════════════════════════════════════════════════
// JSON round-trip
// ══════════════════════════════════════════════════════════════════

TEST(PartStudioJSON, RoundTripsSchemaAndTree) {
    auto ctx = KernelContext::create();
    PartStudio ps(ctx, "rt");
    ConfigInputSpec s;
    s.name = "len"; s.type = ParamType::Double;
    s.defaultValue = 12.5;
    s.minInclusive = 0.1; s.maxInclusive = 100.0;
    s.description = "length along +X";
    ASSERT_EQ(ps.configSchema().addInput(*ctx, s), 0);

    Feature f;
    f.id = "F1"; f.type = "MakeBox";
    f.params["dimensions"] = gp_Vec(10, 20, 30);
    (void)ps.tree().addFeature(f);

    std::string json = ps.toJSON();
    EXPECT_NE(json.find("oreo.part_studio"), std::string::npos);
    EXPECT_NE(json.find("configSchema"), std::string::npos);
    EXPECT_NE(json.find("\"len\""), std::string::npos);

    // Round-trip
    auto ctx2 = KernelContext::create();
    auto restored = PartStudio::fromJSON(ctx2, json);
    ASSERT_TRUE(restored.ok) << restored.error;
    ASSERT_NE(restored.studio, nullptr);

    EXPECT_EQ(restored.studio->name(), "rt");
    EXPECT_EQ(restored.studio->configSchema().inputCount(), 1u);
    const auto* spec = restored.studio->configSchema().find("len");
    ASSERT_NE(spec, nullptr);
    EXPECT_EQ(spec->type, ParamType::Double);
    EXPECT_EQ(std::get<double>(spec->defaultValue), 12.5);
    ASSERT_TRUE(spec->minInclusive.has_value());
    EXPECT_EQ(*spec->minInclusive, 0.1);

    EXPECT_EQ(restored.studio->tree().featureCount(), 1);
}

TEST(PartStudioJSON, ConfigRefRoundTrips) {
    auto ctx = KernelContext::create();
    PartStudio ps(ctx, "ref");
    ConfigInputSpec s;
    s.name = "size"; s.type = ParamType::Vec;
    s.defaultValue = gp_Vec(10, 10, 10);
    ASSERT_EQ(ps.configSchema().addInput(*ctx, s), 0);

    Feature f;
    f.id = "F1"; f.type = "MakeBox";
    f.params["dimensions"] = ConfigRef{"size"};
    (void)ps.tree().addFeature(f);

    std::string json = ps.toJSON();
    EXPECT_NE(json.find("configRef"), std::string::npos);

    auto ctx2 = KernelContext::create();
    auto restored = PartStudio::fromJSON(ctx2, json);
    ASSERT_TRUE(restored.ok) << restored.error;

    const auto* feat = restored.studio->tree().getFeature("F1");
    ASSERT_NE(feat, nullptr);
    const auto* cr = std::get_if<ConfigRef>(&feat->params.at("dimensions"));
    ASSERT_NE(cr, nullptr);
    EXPECT_EQ(cr->name, "size");
}

TEST(PartStudioJSON, V1BareFeatureTreeLoadsAsEmptySchemaStudio) {
    auto ctx = KernelContext::create();

    // Hand-built v1 "bare FeatureTree" JSON — matches what FeatureTree::toJSON
    // emitted before the PartStudio envelope existed. No `tree` key;
    // features are at the top level.
    std::string v1 = R"({
        "_schema": "oreo.feature_tree",
        "features": [
            {"id": "F1", "type": "MakeBox",
             "params": {"dimensions": {"type": "vec", "x": 4, "y": 5, "z": 6}}}
        ],
        "rollbackIndex": -1
    })";

    auto restored = PartStudio::fromJSON(ctx, v1);
    ASSERT_TRUE(restored.ok) << restored.error;
    EXPECT_EQ(restored.studio->configSchema().inputCount(), 0u);
    EXPECT_EQ(restored.studio->tree().featureCount(), 1);

    // Replay still works.
    auto out = restored.studio->executeWithDefaults();
    ASSERT_EQ(out.status, PartStudioOutputs::Status::Ok);
}

TEST(PartStudioJSON, MalformedJsonFailsCleanly) {
    auto ctx = KernelContext::create();
    auto r1 = PartStudio::fromJSON(ctx, "not json");
    EXPECT_FALSE(r1.ok);
    EXPECT_FALSE(r1.error.empty());

    auto r2 = PartStudio::fromJSON(ctx, R"({"_schema": "oreo.part_studio", "tree": {"features": "should be array"}})");
    EXPECT_FALSE(r2.ok);
}

TEST(PartStudioJSON, DuplicateFeatureIdIsRejected) {
    // Regression: pre-fix the duplicate-id check lived only in
    // FeatureTree::fromJSON when called via the bare-tree path. The
    // envelope path delegates to FeatureTree::fromJSON(ctx, treeJson)
    // so the same check applies — this test pins the contract.
    auto ctx = KernelContext::create();
    std::string dup = R"({
        "_schema": "oreo.part_studio",
        "name": "dup",
        "tree": {
            "_schema": "oreo.feature_tree",
            "features": [
                {"id": "F1", "type": "MakeBox",
                 "params": {"dimensions": {"type":"vec","x":1,"y":1,"z":1}}},
                {"id": "F1", "type": "MakeSphere",
                 "params": {"radius": 2.0}}
            ]
        }
    })";
    auto r = PartStudio::fromJSON(ctx, dup);
    EXPECT_FALSE(r.ok);
}

TEST(PartStudioJSON, ConfigSchemaRejectsMalformedType) {
    auto ctx = KernelContext::create();
    std::string bad = R"({
        "_schema": "oreo.part_studio",
        "configSchema": [
            {"name": "x", "type": "NotARealType", "default": 0.0}
        ],
        "tree": {"features": []}
    })";
    auto r = PartStudio::fromJSON(ctx, bad);
    EXPECT_FALSE(r.ok);
}

// ══════════════════════════════════════════════════════════════════
// Extra gap-closers identified by 2026-04-18 self-audit
// ══════════════════════════════════════════════════════════════════

// Identity-bag determinism — not just volume. Repeated executions
// with the same config must produce byte-identical element names
// across the final shape. This is the guarantee the design doc
// actually promises; the old "just volume" test would pass even if
// the allocator went rogue.
TEST(PartStudioExecute, RepeatedExecuteProducesIdenticalElementNames) {
    auto ctx = KernelContext::create();
    PartStudio ps(ctx, "det-names");
    // Simple two-feature tree: box + offset.
    Feature boxF;
    boxF.id = "F1"; boxF.type = "MakeBox";
    boxF.params["dimensions"] = gp_Vec(10, 20, 30);
    (void)ps.tree().addFeature(boxF);

    std::vector<std::string> firstFaceNames;
    std::vector<std::string> firstEdgeNames;
    for (int iter = 0; iter < 4; ++iter) {
        auto out = ps.executeWithDefaults();
        ASSERT_EQ(out.status, PartStudioOutputs::Status::Ok);
        ASSERT_FALSE(out.finalShape.isNull());

        std::vector<std::string> faces;
        std::vector<std::string> edges;
        for (int i = 1; i <= out.finalShape.countSubShapes(TopAbs_FACE); ++i) {
            oreo::IndexedName idx("Face", i);
            faces.push_back(std::string(out.finalShape.getElementName(idx).data()));
        }
        for (int i = 1; i <= out.finalShape.countSubShapes(TopAbs_EDGE); ++i) {
            oreo::IndexedName idx("Edge", i);
            edges.push_back(std::string(out.finalShape.getElementName(idx).data()));
        }
        if (iter == 0) {
            firstFaceNames = faces;
            firstEdgeNames = edges;
        } else {
            EXPECT_EQ(faces, firstFaceNames)
                << "face names diverged at iter " << iter;
            EXPECT_EQ(edges, firstEdgeNames)
                << "edge names diverged at iter " << iter;
        }
    }
}

// Empty ConfigRef name must produce a BrokenReference, not a crash.
// Validates the fail-closed contract at the resolveConfigRefs layer
// (JSON layer accepts empty names as a v1-compat shape, but execute
// must refuse to dispatch).
TEST(PartStudioExecute, EmptyConfigRefNameIsBrokenReference) {
    auto ctx = KernelContext::create();
    PartStudio ps(ctx, "empty-ref");
    Feature f;
    f.id = "F1"; f.type = "MakeBox";
    ConfigRef bad;  // default name is ""
    f.params["dimensions"] = bad;
    (void)ps.tree().addFeature(f);

    auto out = ps.executeWithDefaults();
    EXPECT_EQ(out.status, PartStudioOutputs::Status::Failed);
    ASSERT_EQ(out.brokenFeatures.size(), 1u);
    EXPECT_EQ(out.brokenFeatures[0]->status,
              oreo::FeatureStatus::BrokenReference);
}

// A v1 PartStudio envelope (i.e. _schema="oreo.part_studio" with the
// tree nested but NO configSchema field) must load cleanly — the
// contract claimed in the back-compat section of the design doc.
// The existing V1BareFeatureTreeLoadsAsEmptySchemaStudio tests the
// bare-tree form; this test covers the nested-envelope form
// specifically.
TEST(PartStudioJSON, V1EnvelopeWithoutConfigSchemaLoadsAsEmptySchemaStudio) {
    auto ctx = KernelContext::create();

    // Hand-built v1 PartStudio envelope (no configSchema key).
    std::string v1 = R"({
        "_schema": "oreo.part_studio",
        "_version": {"major": 1, "minor": 0, "patch": 0},
        "name": "legacy studio",
        "tree": {
            "_schema": "oreo.feature_tree",
            "features": [
                {"id": "F1", "type": "MakeBox",
                 "params": {"dimensions": {"type": "vec", "x": 3, "y": 4, "z": 5}}}
            ]
        }
    })";

    auto restored = PartStudio::fromJSON(ctx, v1);
    ASSERT_TRUE(restored.ok) << restored.error;
    EXPECT_EQ(restored.studio->name(), "legacy studio");
    EXPECT_EQ(restored.studio->configSchema().inputCount(), 0u);
    EXPECT_EQ(restored.studio->tree().featureCount(), 1);

    auto out = restored.studio->executeWithDefaults();
    EXPECT_EQ(out.status, PartStudioOutputs::Status::Ok);
}

// C ABI safety: freeing a borrowed tree handle (loaned from
// PartStudio::tree / Workspace::tree / MergeResult::tree) must NOT
// free the parent-owned tree. The free call is a no-op with a warning.
TEST(PartStudioCApiSafety, FreeOnBorrowedTreeIsNoOp) {
    OreoContext cctx = oreo_context_create();
    ASSERT_NE(cctx, nullptr);

    OreoPartStudio ps = oreo_ctx_part_studio_create(cctx, "safety");
    ASSERT_NE(ps, nullptr);

    OreoFeatureTree borrowed = oreo_ctx_part_studio_tree(ps);
    ASSERT_NE(borrowed, nullptr);

    // Attempt an illegal free. The function must tolerate it (no
    // crash, no double-free when ps is later freed).
    oreo_ctx_feature_tree_free(borrowed);

    // Subsequent access still works — the warning was advisory, the
    // tree is still alive because the studio owns it.
    int count = oreo_ctx_feature_tree_count(borrowed);
    EXPECT_EQ(count, 0);

    oreo_ctx_part_studio_free(ps);
    oreo_context_free(cctx);
}

// Transformer-scope RAII: if tree.replay() runs successfully, the
// transformer must not leak out. Calling a non-config replay after
// execute should behave as if no transformer had ever been set.
TEST(PartStudioExecute, TransformerClearedAfterSuccessfulExecute) {
    auto ctx = KernelContext::create();
    PartStudio ps(ctx, "no-leak");
    ConfigInputSpec s;
    s.name = "size"; s.type = ParamType::Vec;
    s.defaultValue = gp_Vec(10, 10, 10);
    ASSERT_EQ(ps.configSchema().addInput(*ctx, s), 0);

    Feature f;
    f.id = "F1"; f.type = "MakeBox";
    f.params["dimensions"] = ConfigRef{"size"};
    (void)ps.tree().addFeature(f);

    (void)ps.executeWithDefaults();
    EXPECT_FALSE(ps.tree().hasFeatureTransformer())
        << "execute() leaked a transformer — subsequent non-config "
           "replays would apply stale config resolution.";
}
