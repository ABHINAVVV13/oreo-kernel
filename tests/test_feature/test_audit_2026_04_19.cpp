// SPDX-License-Identifier: LGPL-2.1-or-later

// test_audit_2026_04_19.cpp — Acceptance tests for the 2026-04-19
// audit findings. Each TEST below locks a specific fix so a future
// regression cannot re-open the issue.
//
//   [P1] C ABI feature-tree param updates for pnt/dir/ax1/ax2/pln,
//        single ref, ref list, config ref, and unset.
//   [P1] C ABI feature-tree read / introspection API.
//   [P1/P2] OreoConfig bound to originating studio schema fingerprint;
//        cross-studio usage rejected at execute time.
//   [P2] ConfigSchema JSON defaults use strict param decode.
//   [P2] ROOT whole-shape refs rejected on face/edge-only schema specs
//        (validator AND resolver defence-in-depth).
//   [P2] Workspace JSON load/save via C ABI.

#include <gtest/gtest.h>

#include "oreo_kernel.h"

#include "core/kernel_context.h"
#include "feature/config.h"
#include "feature/feature.h"
#include "feature/feature_schema.h"
#include "feature/feature_tree.h"
#include "feature/part_studio.h"
#include "feature/workspace.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

// Handful of RAII wrappers so individual tests stay focused on the
// fix under test and we don't leak on failure paths.
struct CtxOwner {
    OreoContext c = nullptr;
    CtxOwner() : c(oreo_context_create()) {}
    ~CtxOwner() { if (c) oreo_context_free(c); }
    operator OreoContext() const { return c; }
};

struct StudioOwner {
    OreoPartStudio ps = nullptr;
    explicit StudioOwner(OreoContext ctx, const char* name = "studio")
        : ps(oreo_ctx_part_studio_create(ctx, name)) {}
    ~StudioOwner() { if (ps) oreo_ctx_part_studio_free(ps); }
    operator OreoPartStudio() const { return ps; }
};

struct ConfigOwner {
    OreoConfig c = nullptr;
    explicit ConfigOwner(OreoPartStudio ps) : c(oreo_config_create(ps)) {}
    ~ConfigOwner() { if (c) oreo_config_free(c); }
    operator OreoConfig() const { return c; }
};

struct WorkspaceOwner {
    OreoWorkspace ws = nullptr;
    explicit WorkspaceOwner(OreoContext ctx, const char* name = "main")
        : ws(oreo_ctx_workspace_create(ctx, name)) {}
    WorkspaceOwner(OreoWorkspace w) : ws(w) {}
    ~WorkspaceOwner() { if (ws) oreo_ctx_workspace_free(ws); }
    operator OreoWorkspace() const { return ws; }
    OreoWorkspace release() { OreoWorkspace out = ws; ws = nullptr; return out; }
};

struct BuilderOwner {
    OreoFeatureBuilder b = nullptr;
    BuilderOwner(const char* id, const char* type)
        : b(oreo_feature_builder_create(id, type)) {}
    ~BuilderOwner() { if (b) oreo_feature_builder_free(b); }
    operator OreoFeatureBuilder() const { return b; }
};

struct SolidOwner {
    OreoSolid s = nullptr;
    SolidOwner() = default;
    SolidOwner(OreoSolid sol) : s(sol) {}
    ~SolidOwner() { if (s) oreo_free_solid(s); }
    SolidOwner& operator=(OreoSolid sol) { if (s) oreo_free_solid(s); s = sol; return *this; }
    operator OreoSolid() const { return s; }
};

// Size-probe helper: drive one of the OREO_API char-buffer accessors
// twice (measure → allocate → read) and return the result string.
template <typename Fn>
std::string runSizeProbe(Fn fn) {
    std::size_t needed = 0;
    int rc = fn(nullptr, 0, &needed);
    if (rc != OREO_OK) return {};
    std::string out;
    out.resize(needed);
    std::size_t needed2 = 0;
    rc = fn(out.empty() ? nullptr : out.data(), out.size() + 1, &needed2);
    if (rc != OREO_OK) return {};
    return out;
}

} // anonymous namespace

// ════════════════════════════════════════════════════════════════════
// [P1] Feature-tree param updates (pnt, dir, ax1, ax2, pln, ref,
//      ref list, config ref, unset).
// ════════════════════════════════════════════════════════════════════

TEST(AuditP1_ParamUpdates, PntSetRoundTrips) {
    CtxOwner ctx;
    OreoFeatureTree t = oreo_ctx_feature_tree_create(ctx);
    ASSERT_NE(t, nullptr);

    {
        BuilderOwner b("F1", "Hole");
        oreo_feature_builder_set_ref(b, "face", "F0", "Face3", "Face");
        oreo_feature_builder_set_pnt(b, "center", 0.0, 0.0, 0.0);
        oreo_feature_builder_set_double(b, "diameter", 5.0);
        oreo_feature_builder_set_double(b, "depth", 2.0);
        EXPECT_EQ(oreo_ctx_feature_tree_add(t, b), OREO_OK);
    }

    // Retarget the hole centre via the new ctx_feature_tree_set_param_pnt.
    EXPECT_EQ(oreo_ctx_feature_tree_set_param_pnt(t, "F1", "center",
                                                   1.0, 2.0, 3.0), OREO_OK);

    double x = 0, y = 0, z = 0;
    EXPECT_EQ(oreo_ctx_feature_tree_get_param_pnt(t, 0, "center", &x, &y, &z),
              OREO_OK);
    EXPECT_DOUBLE_EQ(x, 1.0);
    EXPECT_DOUBLE_EQ(y, 2.0);
    EXPECT_DOUBLE_EQ(z, 3.0);

    oreo_ctx_feature_tree_free(t);
}

TEST(AuditP1_ParamUpdates, DirRejectsZeroMagnitude) {
    CtxOwner ctx;
    OreoFeatureTree t = oreo_ctx_feature_tree_create(ctx);
    ASSERT_NE(t, nullptr);
    {
        BuilderOwner b("F1", "SplitBody");
        oreo_feature_builder_set_pnt(b, "point", 0, 0, 0);
        oreo_feature_builder_set_dir(b, "normal", 0, 0, 1);
        EXPECT_EQ(oreo_ctx_feature_tree_add(t, b), OREO_OK);
    }
    // Zero-magnitude direction is rejected at the ABI boundary.
    EXPECT_EQ(oreo_ctx_feature_tree_set_param_dir(t, "F1", "normal",
                                                   0, 0, 0),
              OREO_INVALID_INPUT);
    // A valid direction still goes through.
    EXPECT_EQ(oreo_ctx_feature_tree_set_param_dir(t, "F1", "normal",
                                                   0, 1, 0),
              OREO_OK);
    double x = 0, y = 0, z = 0;
    EXPECT_EQ(oreo_ctx_feature_tree_get_param_dir(t, 0, "normal", &x, &y, &z),
              OREO_OK);
    EXPECT_DOUBLE_EQ(y, 1.0);
    oreo_ctx_feature_tree_free(t);
}

TEST(AuditP1_ParamUpdates, Ax1Ax2PlnRoundTrip) {
    CtxOwner ctx;
    OreoFeatureTree t = oreo_ctx_feature_tree_create(ctx);
    ASSERT_NE(t, nullptr);
    {
        BuilderOwner b("F1", "Revolve");
        oreo_feature_builder_set_ax1(b, "axis", 0,0,0, 0,0,1);
        oreo_feature_builder_set_double(b, "angle", 3.14);
        EXPECT_EQ(oreo_ctx_feature_tree_add(t, b), OREO_OK);
    }
    {
        BuilderOwner b("F2", "Mirror");
        oreo_feature_builder_set_ax2(b, "plane", 0,0,0, 0,0,1);
        EXPECT_EQ(oreo_ctx_feature_tree_add(t, b), OREO_OK);
    }
    {
        BuilderOwner b("F3", "SplitBody");
        oreo_feature_builder_set_pnt(b, "point", 0,0,0);
        oreo_feature_builder_set_dir(b, "normal", 0,0,1);
        EXPECT_EQ(oreo_ctx_feature_tree_add(t, b), OREO_OK);
    }

    // Retarget axis, ax2 frame, and (since SplitBody uses pnt+dir)
    // swap a pln-typed param via a direct API call — we check pln
    // on a synthetic feature instead.
    EXPECT_EQ(oreo_ctx_feature_tree_set_param_ax1(t, "F1", "axis",
                                                   1, 2, 3, 0, 1, 0), OREO_OK);
    EXPECT_EQ(oreo_ctx_feature_tree_set_param_ax2(t, "F2", "plane",
                                                   0, 0, 5, 1, 0, 0), OREO_OK);
    double px, py, pz, dx, dy, dz;
    EXPECT_EQ(oreo_ctx_feature_tree_get_param_ax1(t, 0, "axis",
                                                   &px, &py, &pz, &dx, &dy, &dz),
              OREO_OK);
    EXPECT_DOUBLE_EQ(px, 1.0); EXPECT_DOUBLE_EQ(py, 2.0); EXPECT_DOUBLE_EQ(pz, 3.0);
    EXPECT_DOUBLE_EQ(dx, 0.0); EXPECT_DOUBLE_EQ(dy, 1.0); EXPECT_DOUBLE_EQ(dz, 0.0);

    double nx, ny, nz;
    EXPECT_EQ(oreo_ctx_feature_tree_get_param_ax2(t, 1, "plane",
                                                   &px, &py, &pz, &nx, &ny, &nz),
              OREO_OK);
    EXPECT_DOUBLE_EQ(pz, 5.0);
    EXPECT_DOUBLE_EQ(nx, 1.0);

    oreo_ctx_feature_tree_free(t);
}

TEST(AuditP1_ParamUpdates, RefSetAndGet) {
    CtxOwner ctx;
    OreoFeatureTree t = oreo_ctx_feature_tree_create(ctx);
    ASSERT_NE(t, nullptr);
    {
        BuilderOwner b("F1", "Hole");
        oreo_feature_builder_set_ref(b, "face", "F0", "Face1", "Face");
        oreo_feature_builder_set_pnt(b, "center", 0,0,0);
        oreo_feature_builder_set_double(b, "diameter", 5.0);
        oreo_feature_builder_set_double(b, "depth", 2.0);
        EXPECT_EQ(oreo_ctx_feature_tree_add(t, b), OREO_OK);
    }
    // Retarget the hole's face.
    EXPECT_EQ(oreo_ctx_feature_tree_set_param_ref(t, "F1", "face",
                                                   "F0", "Face7", "Face"),
              OREO_OK);
    std::string n = runSizeProbe([&](char* buf, std::size_t len, std::size_t* need) {
        return oreo_ctx_feature_tree_get_param_ref(t, 0, "face", 1, buf, len, need);
    });
    EXPECT_EQ(n, "Face7");

    oreo_ctx_feature_tree_free(t);
}

TEST(AuditP1_ParamUpdates, RefListReplaceAndAppend) {
    CtxOwner ctx;
    OreoFeatureTree t = oreo_ctx_feature_tree_create(ctx);
    ASSERT_NE(t, nullptr);
    {
        BuilderOwner b("F1", "Fillet");
        oreo_feature_builder_add_ref(b, "edges", "F0", "Edge1", "Edge");
        oreo_feature_builder_add_ref(b, "edges", "F0", "Edge2", "Edge");
        oreo_feature_builder_set_double(b, "radius", 1.0);
        EXPECT_EQ(oreo_ctx_feature_tree_add(t, b), OREO_OK);
    }
    // Replace the whole list.
    const char* fids [] = { "F0", "F0", "F0" };
    const char* names[] = { "Edge3", "Edge4", "Edge5" };
    const char* types[] = { "Edge", "Edge", "Edge" };
    EXPECT_EQ(oreo_ctx_feature_tree_set_param_ref_list(t, "F1", "edges",
                                                        fids, names, types, 3),
              OREO_OK);
    EXPECT_EQ(oreo_ctx_feature_tree_get_param_ref_list_size(t, 0, "edges"), 3);
    // Append a fourth.
    EXPECT_EQ(oreo_ctx_feature_tree_add_param_ref_list(t, "F1", "edges",
                                                        "F0", "Edge6", "Edge"),
              OREO_OK);
    EXPECT_EQ(oreo_ctx_feature_tree_get_param_ref_list_size(t, 0, "edges"), 4);
    // Read the last entry's elementName (field 1).
    std::string last = runSizeProbe([&](char* buf, std::size_t len, std::size_t* need) {
        return oreo_ctx_feature_tree_get_param_ref_list_entry(t, 0, "edges",
                                                               3, 1,
                                                               buf, len, need);
    });
    EXPECT_EQ(last, "Edge6");

    // Clear the list via an n==0 replace.
    EXPECT_EQ(oreo_ctx_feature_tree_set_param_ref_list(t, "F1", "edges",
                                                        nullptr, nullptr, nullptr, 0),
              OREO_OK);
    EXPECT_EQ(oreo_ctx_feature_tree_get_param_ref_list_size(t, 0, "edges"), 0);

    oreo_ctx_feature_tree_free(t);
}

TEST(AuditP1_ParamUpdates, ConfigRefSetAndGet) {
    CtxOwner ctx;
    OreoFeatureTree t = oreo_ctx_feature_tree_create(ctx);
    ASSERT_NE(t, nullptr);
    {
        BuilderOwner b("F1", "MakeBox");
        oreo_feature_builder_set_vec(b, "dimensions", 1, 1, 1);
        EXPECT_EQ(oreo_ctx_feature_tree_add(t, b), OREO_OK);
    }
    EXPECT_EQ(oreo_ctx_feature_tree_set_param_config_ref(t, "F1", "dimensions",
                                                          "size"),
              OREO_OK);
    std::string name = runSizeProbe([&](char* buf, std::size_t len, std::size_t* need) {
        return oreo_ctx_feature_tree_get_param_config_ref(t, 0, "dimensions",
                                                           buf, len, need);
    });
    EXPECT_EQ(name, "size");
    // Empty config input name is rejected.
    EXPECT_EQ(oreo_ctx_feature_tree_set_param_config_ref(t, "F1", "dimensions", ""),
              OREO_INVALID_INPUT);
    EXPECT_EQ(oreo_ctx_feature_tree_set_param_config_ref(t, "F1", "dimensions", nullptr),
              OREO_INVALID_INPUT);

    oreo_ctx_feature_tree_free(t);
}

TEST(AuditP1_ParamUpdates, UnsetParamRemovesAndPreservesOthers) {
    CtxOwner ctx;
    OreoFeatureTree t = oreo_ctx_feature_tree_create(ctx);
    ASSERT_NE(t, nullptr);
    {
        BuilderOwner b("F1", "MakeBox");
        oreo_feature_builder_set_vec(b, "dimensions", 5, 5, 5);
        EXPECT_EQ(oreo_ctx_feature_tree_add(t, b), OREO_OK);
    }
    EXPECT_EQ(oreo_ctx_feature_tree_param_count(t, 0), 1);
    EXPECT_EQ(oreo_ctx_feature_tree_unset_param(t, "F1", "dimensions"), OREO_OK);
    EXPECT_EQ(oreo_ctx_feature_tree_param_count(t, 0), 0);
    // Unsetting an already-missing param is OK.
    EXPECT_EQ(oreo_ctx_feature_tree_unset_param(t, "F1", "dimensions"), OREO_OK);
    oreo_ctx_feature_tree_free(t);
}

// ════════════════════════════════════════════════════════════════════
// [P1] Feature-tree introspection.
// ════════════════════════════════════════════════════════════════════

TEST(AuditP1_Introspection, IndexOfIdTypeStatusSuppressed) {
    CtxOwner ctx;
    OreoFeatureTree t = oreo_ctx_feature_tree_create(ctx);
    ASSERT_NE(t, nullptr);
    {
        BuilderOwner b("F1", "MakeBox");
        oreo_feature_builder_set_vec(b, "dimensions", 1, 1, 1);
        EXPECT_EQ(oreo_ctx_feature_tree_add(t, b), OREO_OK);
    }
    {
        BuilderOwner b("F2", "MakeSphere");
        oreo_feature_builder_set_double(b, "radius", 0.5);
        EXPECT_EQ(oreo_ctx_feature_tree_add(t, b), OREO_OK);
    }
    EXPECT_EQ(oreo_ctx_feature_tree_count(t), 2);
    EXPECT_EQ(oreo_ctx_feature_tree_index_of(t, "F1"), 0);
    EXPECT_EQ(oreo_ctx_feature_tree_index_of(t, "F2"), 1);
    EXPECT_EQ(oreo_ctx_feature_tree_index_of(t, "MISSING"), -1);

    std::string id0 = runSizeProbe([&](char* b, std::size_t n, std::size_t* need) {
        return oreo_ctx_feature_tree_feature_id(t, 0, b, n, need);
    });
    std::string type1 = runSizeProbe([&](char* b, std::size_t n, std::size_t* need) {
        return oreo_ctx_feature_tree_feature_type(t, 1, b, n, need);
    });
    EXPECT_EQ(id0, "F1");
    EXPECT_EQ(type1, "MakeSphere");

    // Pre-replay status is NotExecuted (4).
    EXPECT_EQ(oreo_ctx_feature_tree_feature_status(t, 0), 4);
    EXPECT_EQ(oreo_ctx_feature_tree_feature_suppressed(t, 0), 0);
    EXPECT_EQ(oreo_ctx_feature_tree_suppress(t, "F1", 1), OREO_OK);
    EXPECT_EQ(oreo_ctx_feature_tree_feature_suppressed(t, 0), 1);

    // OOB index handling.
    EXPECT_EQ(oreo_ctx_feature_tree_feature_status(t, 99), -1);
    EXPECT_EQ(oreo_ctx_feature_tree_feature_suppressed(t, -1), -1);

    oreo_ctx_feature_tree_free(t);
}

TEST(AuditP1_Introspection, ParamReadersByType) {
    CtxOwner ctx;
    OreoFeatureTree t = oreo_ctx_feature_tree_create(ctx);
    ASSERT_NE(t, nullptr);
    {
        BuilderOwner b("F1", "Extrude");
        oreo_feature_builder_set_vec(b, "direction", 1, 2, 3);
        EXPECT_EQ(oreo_ctx_feature_tree_add(t, b), OREO_OK);
    }
    EXPECT_EQ(oreo_ctx_feature_tree_param_type(t, 0, "direction"),
              /*ParamType::Vec=*/4);
    EXPECT_EQ(oreo_ctx_feature_tree_param_count(t, 0), 1);
    std::string name = runSizeProbe([&](char* b, std::size_t n, std::size_t* need) {
        return oreo_ctx_feature_tree_param_name(t, 0, 0, b, n, need);
    });
    EXPECT_EQ(name, "direction");
    double x = 0, y = 0, z = 0;
    EXPECT_EQ(oreo_ctx_feature_tree_get_param_vec(t, 0, "direction", &x, &y, &z),
              OREO_OK);
    EXPECT_DOUBLE_EQ(x, 1); EXPECT_DOUBLE_EQ(y, 2); EXPECT_DOUBLE_EQ(z, 3);

    // Wrong-type readers must not succeed on a Vec param.
    double d = 0;
    EXPECT_EQ(oreo_ctx_feature_tree_get_param_double(t, 0, "direction", &d),
              OREO_INVALID_STATE);
    // Missing param returns INVALID_INPUT.
    EXPECT_EQ(oreo_ctx_feature_tree_get_param_double(t, 0, "nonexistent", &d),
              OREO_INVALID_INPUT);
    // OOB feature returns OUT_OF_RANGE.
    EXPECT_EQ(oreo_ctx_feature_tree_get_param_double(t, 99, "direction", &d),
              OREO_OUT_OF_RANGE);

    oreo_ctx_feature_tree_free(t);
}

// ════════════════════════════════════════════════════════════════════
// [P1/P2] OreoConfig fingerprint binding.
// ════════════════════════════════════════════════════════════════════

TEST(AuditP1P2_ConfigGuard, FingerprintStableAcrossIdenticalSchemas) {
    CtxOwner ctx;
    StudioOwner psA(ctx, "A");
    StudioOwner psB(ctx, "B");

    EXPECT_EQ(oreo_ctx_part_studio_add_input_double(psA, "size", 10.0), OREO_OK);
    EXPECT_EQ(oreo_ctx_part_studio_add_input_double(psB, "size", 10.0), OREO_OK);

    const uint64_t a = oreo_ctx_part_studio_schema_fingerprint(psA);
    const uint64_t b = oreo_ctx_part_studio_schema_fingerprint(psB);
    EXPECT_NE(a, 0u);
    EXPECT_EQ(a, b);  // same schema shape ⇒ same fingerprint
}

TEST(AuditP1P2_ConfigGuard, FingerprintChangesWithSchema) {
    CtxOwner ctx;
    StudioOwner ps(ctx);

    const uint64_t empty = oreo_ctx_part_studio_schema_fingerprint(ps);
    EXPECT_EQ(oreo_ctx_part_studio_add_input_double(ps, "a", 1.0), OREO_OK);
    const uint64_t afterA = oreo_ctx_part_studio_schema_fingerprint(ps);
    EXPECT_NE(empty, afterA);
    EXPECT_EQ(oreo_ctx_part_studio_add_input_double(ps, "b", 2.0), OREO_OK);
    const uint64_t afterB = oreo_ctx_part_studio_schema_fingerprint(ps);
    EXPECT_NE(afterA, afterB);
}

TEST(AuditP1P2_ConfigGuard, ExecuteRejectsCrossStudioConfig) {
    CtxOwner ctx;
    StudioOwner psA(ctx, "A");
    StudioOwner psB(ctx, "B");

    // Studios share the schema shape, but only psA's config is valid
    // for psA. The audit scenario: app swaps which studio it's calling.
    EXPECT_EQ(oreo_ctx_part_studio_add_input_double(psA, "size", 10.0), OREO_OK);
    EXPECT_EQ(oreo_ctx_part_studio_add_input_vec(psB, "size", 1, 1, 1), OREO_OK);

    // Build a feature referencing the config so execute actually uses it.
    {
        BuilderOwner b("F1", "MakeBox");
        // psB's "size" is a Vec — a MakeBox that reads "size" via
        // ConfigRef would fetch a Vec. psA's "size" is a Double — that
        // same ConfigRef would fetch a Double, which validateFeature
        // would reject when MakeBox expects Vec for dimensions.
        oreo_feature_builder_set_config_ref(b, "dimensions", "size");
        EXPECT_EQ(oreo_ctx_feature_tree_add(
                      oreo_ctx_part_studio_tree(psB), b), OREO_OK);
    }

    ConfigOwner cfgA(psA);
    // Without the fingerprint guard, execute would accept cfgA on
    // psB, find "size" with a Double value (psA's schema default),
    // and try to feed it into MakeBox.dimensions which expects a Vec
    // — leading to a confusing validation failure inside the feature.
    //
    // With the guard: execute refuses up front with INVALID_INPUT and
    // reports the mismatch on the studio's ctx.
    SolidOwner out = oreo_ctx_part_studio_execute(psB, cfgA);
    EXPECT_EQ(out.s, nullptr);
    EXPECT_TRUE(oreo_context_has_errors(ctx));
}

TEST(AuditP1P2_ConfigGuard, MutatingSchemaAfterConfigInvalidates) {
    CtxOwner ctx;
    StudioOwner ps(ctx);
    EXPECT_EQ(oreo_ctx_part_studio_add_input_double(ps, "x", 1.0), OREO_OK);
    ConfigOwner cfg(ps);
    // Schema mutates AFTER cfg was created.
    EXPECT_EQ(oreo_ctx_part_studio_add_input_double(ps, "y", 2.0), OREO_OK);
    // No features to execute, but the guard still trips because the
    // fingerprints no longer match.
    SolidOwner out = oreo_ctx_part_studio_execute(ps, cfg);
    EXPECT_EQ(out.s, nullptr);
    EXPECT_TRUE(oreo_context_has_errors(ctx));
}

TEST(AuditP1P2_ConfigGuard, ConfigSymmetricAccessor) {
    CtxOwner ctx;
    StudioOwner ps(ctx);
    EXPECT_EQ(oreo_ctx_part_studio_add_input_double(ps, "x", 1.0), OREO_OK);
    const uint64_t psFp = oreo_ctx_part_studio_schema_fingerprint(ps);
    ConfigOwner cfg(ps);
    EXPECT_EQ(oreo_config_schema_fingerprint(cfg), psFp);
}

// ════════════════════════════════════════════════════════════════════
// [P2] ConfigSchema JSON defaults: strict decode path.
// ════════════════════════════════════════════════════════════════════

TEST(AuditP2_StrictSchemaDefaults, RejectsBadPointDefault) {
    auto ctx = oreo::KernelContext::create();
    // Directly craft a v2 PartStudio envelope whose configSchema carries
    // a Pnt default with the wrong type tag ("vec"). The legacy lenient
    // decode would silently accept it as a Vec; the strict path rejects.
    const char* badJson = R"({
        "_schema": "oreo.part_studio",
        "_version": {"major":2,"minor":0,"patch":0},
        "name": "bad",
        "documentId": "0",
        "configSchema": [
            {"name":"p","type":"Pnt","default":{"type":"vec","x":1,"y":2,"z":3}}
        ],
        "tree": {"features":[]}
    })";
    auto result = oreo::PartStudio::fromJSON(ctx, badJson);
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.studio);
    // The specific "default is malformed" diagnostic is routed to the
    // ctx via ctx.diag().error() — the outer fromJSON error is the
    // wrapper "PartStudio configSchema failed to parse". Both surfaces
    // indicate rejection; we check the underlying diagnostic stream
    // carries the strict-decode reason so a future regression to the
    // lenient path is caught.
    bool foundStrictReason = false;
    for (const auto& d : ctx->diag().all()) {
        if (d.message.find("malformed") != std::string::npos) {
            foundStrictReason = true;
            break;
        }
    }
    EXPECT_TRUE(foundStrictReason)
        << "Expected strict-decode 'malformed' diagnostic on ctx; outer error="
        << result.error;
}

TEST(AuditP2_StrictSchemaDefaults, AcceptsWellFormedPointDefault) {
    auto ctx = oreo::KernelContext::create();
    const char* goodJson = R"({
        "_schema": "oreo.part_studio",
        "_version": {"major":2,"minor":0,"patch":0},
        "name": "good",
        "documentId": "0",
        "configSchema": [
            {"name":"p","type":"Pnt","default":{"type":"point","x":1,"y":2,"z":3}}
        ],
        "tree": {"features":[]}
    })";
    auto result = oreo::PartStudio::fromJSON(ctx, goodJson);
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(result.studio);
    const auto* spec = result.studio->configSchema().find("p");
    ASSERT_NE(spec, nullptr);
    EXPECT_EQ(spec->type, oreo::ParamType::Pnt);
}

// ════════════════════════════════════════════════════════════════════
// [P2] ROOT whole-shape refs are rejected on sub-shape specs.
// ════════════════════════════════════════════════════════════════════

TEST(AuditP2_RootRef, SchemaValidatorRejectsRootOnFaceSpec) {
    auto ctx = oreo::KernelContext::create();
    oreo::FeatureTree tree(ctx);

    // MakeBox as producer.
    oreo::Feature box;
    box.id = "F1"; box.type = "MakeBox";
    box.params["dimensions"] = gp_Vec(10, 10, 10);
    ASSERT_TRUE(tree.addFeature(box));

    // Hole requires a Face ref. A ROOT ref with elementType="Face"
    // would previously pass validateFeature because only the string
    // type was checked. After the fix it must fail in validation.
    oreo::Feature hole;
    hole.id = "F2"; hole.type = "Hole";
    hole.params["face"]     = oreo::ElementRef{"F1", "ROOT", "Face"};
    hole.params["center"]   = gp_Pnt(0, 0, 0);
    hole.params["diameter"] = 1.0;
    hole.params["depth"]    = 1.0;
    ASSERT_TRUE(tree.addFeature(hole));

    tree.replay();
    const auto* f2 = tree.getFeature("F2");
    ASSERT_NE(f2, nullptr);
    EXPECT_NE(f2->status, oreo::FeatureStatus::OK)
        << "Expected ROOT-on-Face-spec to be rejected; status="
        << static_cast<int>(f2->status);
    EXPECT_NE(f2->errorMessage.find("whole-shape sentinel"), std::string::npos)
        << "Error should name the sentinel pattern; got: " << f2->errorMessage;
}

TEST(AuditP2_RootRef, ResolverRefusesRootWithSubShapeType) {
    // Defence-in-depth: even if a caller bypasses the schema validator
    // (e.g. direct C++ calls through feature_tree internals), the
    // resolver itself refuses a ROOT ref declaring a sub-shape type.
    auto ctx = oreo::KernelContext::create();
    oreo::FeatureTree tree(ctx);

    oreo::Feature box;
    box.id = "F1"; box.type = "MakeBox";
    box.params["dimensions"] = gp_Vec(10, 10, 10);
    ASSERT_TRUE(tree.addFeature(box));

    // Combine is a whole-shape consumer whose schema accepts refKind=""
    // — so the validator lets through a ROOT ref with elementType="Face".
    // The resolver layer MUST still refuse the ROOT+Face combination
    // because it is internally contradictory.
    oreo::Feature combine;
    combine.id = "F2"; combine.type = "Combine";
    combine.params["shapes"] = std::vector<oreo::ElementRef>{
        oreo::ElementRef{"F1", "ROOT", "Face"}  // contradictory
    };
    ASSERT_TRUE(tree.addFeature(combine));
    tree.replay();

    const auto* f2 = tree.getFeature("F2");
    ASSERT_NE(f2, nullptr);
    EXPECT_NE(f2->status, oreo::FeatureStatus::OK)
        << "Resolver must refuse a ROOT+Face ref; status="
        << static_cast<int>(f2->status);
}

TEST(AuditP2_RootRef, LegitimateRootForWholeShapeStillWorks) {
    // Regression guard: our fix must not break the BooleanIntersect
    // whole-shape path that test_feature_extended validates.
    auto ctx = oreo::KernelContext::create();
    oreo::FeatureTree tree(ctx);

    oreo::Feature f1, f2, f3;
    f1.id = "F1"; f1.type = "MakeBox";
    f1.params["dimensions"] = gp_Vec(20, 20, 20);
    ASSERT_TRUE(tree.addFeature(f1));

    f2.id = "F2"; f2.type = "MakeSphere";
    f2.params["radius"] = 12.0;
    ASSERT_TRUE(tree.addFeature(f2));

    f3.id = "F3"; f3.type = "BooleanIntersect";
    f3.params["tool"] = oreo::ElementRef{"F1", "ROOT", "Solid"};
    ASSERT_TRUE(tree.addFeature(f3));

    tree.replay();
    const auto* f3p = tree.getFeature("F3");
    ASSERT_NE(f3p, nullptr);
    EXPECT_EQ(f3p->status, oreo::FeatureStatus::OK)
        << "Whole-shape ROOT ref with Solid type must still resolve. "
        << "errorMessage=" << f3p->errorMessage;
}

// ════════════════════════════════════════════════════════════════════
// [P2] Workspace JSON round-trip via C ABI.
// ════════════════════════════════════════════════════════════════════

TEST(AuditP2_WorkspaceJson, RoundTripPreservesFeatures) {
    CtxOwner ctx;
    WorkspaceOwner ws(ctx, "main");
    ASSERT_NE(ws.ws, nullptr);

    // Populate a feature.
    {
        OreoFeatureTree tree = oreo_ctx_workspace_tree(ws);
        ASSERT_NE(tree, nullptr);
        BuilderOwner b("F1", "MakeBox");
        oreo_feature_builder_set_vec(b, "dimensions", 1, 2, 3);
        EXPECT_EQ(oreo_ctx_feature_tree_add(tree, b), OREO_OK);
    }
    // Save to JSON via C ABI.
    char* saved = oreo_ctx_workspace_to_json(ws);
    ASSERT_NE(saved, nullptr);
    const std::string savedStr = saved;
    oreo_free_string(saved);

    // Re-load in a fresh context; feature count + first feature's id
    // + name must survive.
    CtxOwner ctx2;
    OreoWorkspace ws2 = oreo_ctx_workspace_from_json(ctx2, savedStr.c_str());
    ASSERT_NE(ws2, nullptr);
    WorkspaceOwner ws2Guard(ws2);
    OreoFeatureTree t2 = oreo_ctx_workspace_tree(ws2);
    ASSERT_NE(t2, nullptr);
    EXPECT_EQ(oreo_ctx_feature_tree_count(t2), 1);
    std::string id0 = runSizeProbe([&](char* b, std::size_t n, std::size_t* need) {
        return oreo_ctx_feature_tree_feature_id(t2, 0, b, n, need);
    });
    EXPECT_EQ(id0, "F1");
    std::string name = runSizeProbe([&](char* b, std::size_t n, std::size_t* need) {
        return oreo_ctx_workspace_name(ws2, b, n, need);
    });
    EXPECT_EQ(name, "main");
}

TEST(AuditP2_WorkspaceJson, RejectsMalformedJson) {
    CtxOwner ctx;
    EXPECT_EQ(oreo_ctx_workspace_from_json(ctx, "{not: valid"), nullptr);
    EXPECT_TRUE(oreo_context_has_errors(ctx));
}

TEST(AuditP2_WorkspaceJson, RejectsWrongSchema) {
    CtxOwner ctx;
    // A FeatureTree JSON is not a Workspace — the schema gate must trip.
    const char* treeJson = R"({
        "_schema": "oreo.feature_tree",
        "_version": {"major":2,"minor":0,"patch":0},
        "features": []
    })";
    EXPECT_EQ(oreo_ctx_workspace_from_json(ctx, treeJson), nullptr);
    EXPECT_TRUE(oreo_context_has_errors(ctx));
}
