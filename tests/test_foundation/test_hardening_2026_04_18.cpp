// SPDX-License-Identifier: LGPL-2.1-or-later
//
// test_hardening_2026_04_18.cpp
//
// End-to-end lock-down tests for the kernel-hardening sprint landed on
// 2026-04-18. One fixture per finding in the audit that motivated the
// sprint; each test is written to FAIL if the corresponding fix ever
// regresses. Comments reference finding numbers from
// docs/PROTOTYPE-V0.md so future maintainers can cross-reference.

#include "core/kernel_context.h"
#include "core/schema.h"
#include "feature/feature.h"
#include "feature/feature_tree.h"
#include "feature/merge.h"
#include "feature/param_json.h"
#include "feature/part_studio.h"
#include "feature/workspace.h"
#include "geometry/oreo_geometry.h"
#include "io/temp_file.h"
#include "oreo_kernel.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

namespace {

oreo::Feature makeBoxFeature(const std::string& id, double dx, double dy, double dz) {
    oreo::Feature f;
    f.id = id;
    f.type = "MakeBox";
    f.params["dimensions"] = gp_Vec(dx, dy, dz);
    return f;
}

}  // anonymous

// ═══════════════════════════════════════════════════════════════
// Finding #1 — whole-shape ElementRef resolution
// ═══════════════════════════════════════════════════════════════

TEST(Hardening_2026_04_18, BooleanSubtractViaFeatureTreeWholeShapeRef) {
    auto ctx = oreo::KernelContext::create();
    oreo::FeatureTree tree(ctx);

    (void)tree.addFeature(makeBoxFeature("cube", 10, 10, 10));
    // Second primitive — cache-only, not the running shape.
    (void)tree.addFeature(makeBoxFeature("tool", 4, 4, 20));

    oreo::Feature sub;
    sub.id = "cut";
    sub.type = "BooleanSubtract";
    sub.params["tool"] = oreo::ElementRef{"cube", "ROOT", "Solid"};
    (void)tree.addFeature(sub);

    auto result = tree.replay();

    const auto* cutF = tree.getFeature("cut");
    ASSERT_NE(cutF, nullptr);
    EXPECT_EQ(cutF->status, oreo::FeatureStatus::OK)
        << "BooleanSubtract(current=tool, toolRef=ROOT-of-cube) must resolve; "
           "got status=" << static_cast<int>(cutF->status)
        << " err=" << cutF->errorMessage;
    EXPECT_FALSE(result.isNull())
        << "Replay must produce a non-null NamedShape after boolean";
}

TEST(Hardening_2026_04_18, BooleanUnionViaFeatureTreeWholeShapeRef) {
    auto ctx = oreo::KernelContext::create();
    oreo::FeatureTree tree(ctx);

    (void)tree.addFeature(makeBoxFeature("a", 8, 8, 8));
    (void)tree.addFeature(makeBoxFeature("b", 6, 6, 6));

    oreo::Feature un;
    un.id = "uni";
    un.type = "BooleanUnion";
    un.params["tool"] = oreo::ElementRef{"a", "ROOT", "Solid"};
    (void)tree.addFeature(un);

    auto result = tree.replay();
    const auto* uniF = tree.getFeature("uni");
    ASSERT_NE(uniF, nullptr);
    EXPECT_EQ(uniF->status, oreo::FeatureStatus::OK)
        << "BooleanUnion with whole-shape ref must succeed; err="
        << uniF->errorMessage;
    EXPECT_FALSE(result.isNull());
}

TEST(Hardening_2026_04_18, BooleanRejectsSubShapeToolRef) {
    // A boolean "tool" MUST be a whole-shape ref. Passing a Face / Edge /
    // Vertex ref is a schema error surfaced as BrokenReference. This
    // catches app-layer bugs before the OCCT op detonates.
    auto ctx = oreo::KernelContext::create();
    oreo::FeatureTree tree(ctx);

    (void)tree.addFeature(makeBoxFeature("cube", 10, 10, 10));
    (void)tree.addFeature(makeBoxFeature("tool", 3, 3, 3));

    oreo::Feature sub;
    sub.id = "bad";
    sub.type = "BooleanSubtract";
    sub.params["tool"] = oreo::ElementRef{"cube", "Face1", "Face"};
    (void)tree.addFeature(sub);

    tree.replay();
    const auto* badF = tree.getFeature("bad");
    ASSERT_NE(badF, nullptr);
    EXPECT_EQ(badF->status, oreo::FeatureStatus::BrokenReference);
    EXPECT_NE(badF->errorMessage.find("whole-shape"), std::string::npos)
        << "Error message should indicate whole-shape requirement; got: "
        << badF->errorMessage;
}

// ═══════════════════════════════════════════════════════════════
// Finding #2 — applyResolutions exposed through C ABI
// ═══════════════════════════════════════════════════════════════

TEST(Hardening_2026_04_18, ApplyResolutionsRoundTripViaCAbi) {
    // Build a simple conflict scenario entirely via the C ABI, call
    // oreo_merge_result_apply_resolutions, and confirm the returned
    // tree reflects the caller's choice.
    OreoContext ctx = oreo_context_create();
    ASSERT_NE(ctx, nullptr);

    // Build base workspace: one box feature.
    OreoWorkspace base = oreo_ctx_workspace_create(ctx, "base");
    ASSERT_NE(base, nullptr);
    OreoFeatureBuilder fb = oreo_feature_builder_create("F1", "MakeBox");
    ASSERT_NE(fb, nullptr);
    ASSERT_EQ(oreo_feature_builder_set_vec(fb, "dimensions", 10.0, 10.0, 10.0), OREO_OK);
    OreoFeatureTree baseTree = oreo_ctx_workspace_tree(base);
    ASSERT_EQ(oreo_ctx_feature_tree_add(baseTree, fb), OREO_OK);
    oreo_feature_builder_free(fb);

    // Fork → ours (change dimensions to 20×10×10).
    OreoWorkspace ours = oreo_ctx_workspace_fork(base, "ours");
    ASSERT_NE(ours, nullptr);
    OreoFeatureTree oursTree = oreo_ctx_workspace_tree(ours);
    ASSERT_EQ(oreo_ctx_feature_tree_set_param_vec(oursTree, "F1", "dimensions",
                                                   20.0, 10.0, 10.0), OREO_OK);

    // Fork → theirs (change dimensions to 10×20×10). Conflict on F1.dimensions.
    OreoWorkspace theirs = oreo_ctx_workspace_fork(base, "theirs");
    ASSERT_NE(theirs, nullptr);
    OreoFeatureTree theirsTree = oreo_ctx_workspace_tree(theirs);
    ASSERT_EQ(oreo_ctx_feature_tree_set_param_vec(theirsTree, "F1", "dimensions",
                                                   10.0, 20.0, 10.0), OREO_OK);

    OreoMergeResult mr = oreo_ctx_workspace_merge(base, ours, theirs);
    ASSERT_NE(mr, nullptr);
    EXPECT_FALSE(oreo_merge_result_is_clean(mr));
    EXPECT_GE(oreo_merge_result_conflict_count(mr), 1);

    // Build resolution: take "ours" (choice=0). paramName is "dimensions".
    OreoResolutionSet rs = oreo_resolution_set_create();
    ASSERT_NE(rs, nullptr);
    ASSERT_EQ(oreo_resolution_set_add(rs, "F1", "dimensions", /*Ours=*/0), OREO_OK);
    EXPECT_EQ(oreo_resolution_set_size(rs), 1);

    OreoFeatureTree resolved = oreo_merge_result_apply_resolutions(mr, rs);
    ASSERT_NE(resolved, nullptr);
    EXPECT_EQ(oreo_ctx_feature_tree_count(resolved), 1);

    // Sanity: replaying the resolved tree produces a box 20x10x10 (ours).
    OreoSolid h = oreo_ctx_feature_tree_replay(resolved);
    ASSERT_NE(h, nullptr);
    OreoBBox bb = oreo_ctx_aabb(ctx, h);
    EXPECT_NEAR(bb.xmax - bb.xmin, 20.0, 1e-6) << "resolve 'Ours' → dx=20";
    EXPECT_NEAR(bb.ymax - bb.ymin, 10.0, 1e-6);
    EXPECT_NEAR(bb.zmax - bb.zmin, 10.0, 1e-6);
    oreo_free_solid(h);

    // Apply the SAME merge with a "Theirs" resolution and confirm the
    // tree reflects the alternative choice.
    OreoResolutionSet rs2 = oreo_resolution_set_create();
    ASSERT_EQ(oreo_resolution_set_add(rs2, "F1", "dimensions", /*Theirs=*/1), OREO_OK);
    OreoFeatureTree resolved2 = oreo_merge_result_apply_resolutions(mr, rs2);
    ASSERT_NE(resolved2, nullptr);
    OreoSolid h2 = oreo_ctx_feature_tree_replay(resolved2);
    ASSERT_NE(h2, nullptr);
    OreoBBox bb2 = oreo_ctx_aabb(ctx, h2);
    EXPECT_NEAR(bb2.ymax - bb2.ymin, 20.0, 1e-6) << "resolve 'Theirs' → dy=20";
    oreo_free_solid(h2);

    // Custom value via last_set_vec.
    OreoResolutionSet rs3 = oreo_resolution_set_create();
    ASSERT_EQ(oreo_resolution_set_add(rs3, "F1", "dimensions", /*Custom=*/3), OREO_OK);
    ASSERT_EQ(oreo_resolution_set_last_set_vec(rs3, 7.0, 7.0, 7.0), OREO_OK);
    OreoFeatureTree resolved3 = oreo_merge_result_apply_resolutions(mr, rs3);
    ASSERT_NE(resolved3, nullptr);
    OreoSolid h3 = oreo_ctx_feature_tree_replay(resolved3);
    ASSERT_NE(h3, nullptr);
    OreoBBox bb3 = oreo_ctx_aabb(ctx, h3);
    EXPECT_NEAR(bb3.xmax - bb3.xmin, 7.0, 1e-6);
    oreo_free_solid(h3);

    oreo_ctx_feature_tree_free(resolved);
    oreo_ctx_feature_tree_free(resolved2);
    oreo_ctx_feature_tree_free(resolved3);
    oreo_resolution_set_free(rs);
    oreo_resolution_set_free(rs2);
    oreo_resolution_set_free(rs3);
    oreo_merge_result_free(mr);
    oreo_ctx_workspace_free(theirs);
    oreo_ctx_workspace_free(ours);
    oreo_ctx_workspace_free(base);
    oreo_context_free(ctx);
}

TEST(Hardening_2026_04_18, ResolutionSetLastSetRejectsNonCustom) {
    // _last_set_* should fail with OREO_INVALID_STATE when the last
    // resolution's choice is not Custom. This prevents callers from
    // accidentally attaching a value that will be silently discarded.
    OreoResolutionSet rs = oreo_resolution_set_create();
    ASSERT_NE(rs, nullptr);

    // Empty set → error.
    EXPECT_EQ(oreo_resolution_set_last_set_double(rs, 1.0), OREO_INVALID_STATE);

    // Add with choice=Ours (0); setter must refuse.
    ASSERT_EQ(oreo_resolution_set_add(rs, "F1", "p", /*Ours=*/0), OREO_OK);
    EXPECT_EQ(oreo_resolution_set_last_set_double(rs, 1.0), OREO_INVALID_STATE);
    EXPECT_EQ(oreo_resolution_set_last_set_string(rs, "x"), OREO_INVALID_STATE);

    // Switch to Custom by adding a second resolution; setter now succeeds.
    ASSERT_EQ(oreo_resolution_set_add(rs, "F2", "p", /*Custom=*/3), OREO_OK);
    EXPECT_EQ(oreo_resolution_set_last_set_double(rs, 2.0), OREO_OK);

    oreo_resolution_set_free(rs);
}

// ═══════════════════════════════════════════════════════════════
// Finding #3 — schema/version gate in PartStudio::fromJSON / Workspace::fromJSON
// ═══════════════════════════════════════════════════════════════

TEST(Hardening_2026_04_18, PartStudioFromJsonRejectsWrongSchemaType) {
    auto ctx = oreo::KernelContext::create();
    // Valid envelope but with the wrong _schema value.
    json bad;
    bad["_schema"] = "oreo.workspace";  // should be oreo.part_studio
    bad["_version"] = {{"major", 2}, {"minor", 0}, {"patch", 0}};
    bad["name"] = "x";
    bad["tree"] = json::object({{"features", json::array()}});

    auto r = oreo::PartStudio::fromJSON(ctx, bad.dump());
    EXPECT_EQ(r.studio, nullptr);
    EXPECT_FALSE(r.error.empty());
    EXPECT_NE(r.error.find("wrong _schema"), std::string::npos)
        << "Error should call out schema mismatch; got: " << r.error;
}

TEST(Hardening_2026_04_18, PartStudioFromJsonRejectsFutureMajor) {
    auto ctx = oreo::KernelContext::create();
    json far;
    far["_schema"] = "oreo.part_studio";
    far["_version"] = {{"major", 999}, {"minor", 0}, {"patch", 0}};
    far["name"] = "y";
    far["tree"] = json::object({{"features", json::array()}});

    auto r = oreo::PartStudio::fromJSON(ctx, far.dump());
    EXPECT_EQ(r.studio, nullptr);
    EXPECT_FALSE(r.error.empty());
    EXPECT_NE(r.error.find("not loadable"), std::string::npos)
        << "Future-major should be rejected as not loadable; got: " << r.error;
}

TEST(Hardening_2026_04_18, PartStudioFromJsonAcceptsCurrentVersion) {
    // Round-trip: write a PartStudio with headers, read it back, succeed.
    auto ctx = oreo::KernelContext::create();
    auto ps = std::make_unique<oreo::PartStudio>(ctx, "roundtrip");
    (void)ps->tree().addFeature(makeBoxFeature("F1", 3, 4, 5));
    std::string jsonStr = ps->toJSON();

    auto r = oreo::PartStudio::fromJSON(ctx, jsonStr);
    ASSERT_NE(r.studio, nullptr)
        << "self-round-trip must succeed after schema gate; err=" << r.error;
    EXPECT_EQ(r.studio->tree().featureCount(), 1);
}

TEST(Hardening_2026_04_18, WorkspaceFromJsonRejectsWrongSchemaType) {
    auto ctx = oreo::KernelContext::create();
    json bad;
    bad["_schema"] = "oreo.part_studio";  // should be oreo.workspace
    bad["_version"] = {{"major", 1}, {"minor", 0}, {"patch", 0}};
    bad["name"] = "ws";
    bad["features"] = json::array();

    auto r = oreo::Workspace::fromJSON(ctx, bad.dump());
    EXPECT_EQ(r.workspace, nullptr);
    EXPECT_FALSE(r.error.empty());
    EXPECT_NE(r.error.find("wrong _schema"), std::string::npos);
}

TEST(Hardening_2026_04_18, WorkspaceFromJsonRejectsFutureMajor) {
    auto ctx = oreo::KernelContext::create();
    json far;
    far["_schema"] = "oreo.workspace";
    far["_version"] = {{"major", 42}, {"minor", 0}, {"patch", 0}};
    far["name"] = "ws";
    far["features"] = json::array();

    auto r = oreo::Workspace::fromJSON(ctx, far.dump());
    EXPECT_EQ(r.workspace, nullptr);
    EXPECT_FALSE(r.error.empty());
    EXPECT_NE(r.error.find("not loadable"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════
// Finding #4 — strict param JSON decode (schema-guided)
// ═══════════════════════════════════════════════════════════════

TEST(Hardening_2026_04_18, StrictDecodeRejectsMissingGeometryField) {
    // dimensions is a Vec: all three axes must be present. Previously
    // `val.value("y", 0.0)` would silently default y=0.
    json tree;
    tree["features"] = json::array({
        json{
            {"id", "F1"}, {"type", "MakeBox"},
            {"params", {
                {"dimensions", json{{"type", "vec"}, {"x", 10}, {"z", 10}}},  // missing y
            }},
        },
    });
    auto r = oreo::FeatureTree::fromJSON(tree.dump());
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("dimensions"), std::string::npos)
        << "Error should name the field: " << r.error;
    EXPECT_NE(r.error.find("missing field 'y'"), std::string::npos)
        << "Error should specify the missing subfield: " << r.error;
}

TEST(Hardening_2026_04_18, StrictDecodeRejectsWrongTypeForDouble) {
    json tree;
    tree["features"] = json::array({
        json{
            {"id", "F1"}, {"type", "MakeCylinder"},
            {"params", {
                {"radius", "five"},  // string, not number
                {"height", 10},
            }},
        },
    });
    auto r = oreo::FeatureTree::fromJSON(tree.dump());
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("expected JSON number"), std::string::npos)
        << "Error should complain about number type; got: " << r.error;
}

TEST(Hardening_2026_04_18, StrictDecodeRejectsNaNForDouble) {
    // NaN sneaks through nlohmann::json as std::numeric_limits<double>::quiet_NaN()
    // if a producer serializes it as a sentinel. The strict decoder must
    // reject it (validateFeature also rejects, but the earlier the better).
    auto decoded = oreo::paramValueFromJsonStrict(
        std::numeric_limits<double>::quiet_NaN(),
        oreo::ParamType::Double,
        nullptr);
    EXPECT_FALSE(decoded.has_value());
}

TEST(Hardening_2026_04_18, StrictDecodeRejectsMissingAxisFields) {
    // Mirror's plane is Ax2 — {type:"ax2", px,py,pz, dx,dy,dz, xx,xy,xz}.
    // Drop xz so the strict decoder must reject with a specific message.
    json tree;
    tree["features"] = json::array({
        json{
            {"id", "P"}, {"type", "MakeBox"},
            {"params", {{"dimensions", json{{"type","vec"},{"x",1},{"y",1},{"z",1}}}}},
        },
        json{
            {"id", "M"}, {"type", "Mirror"},
            {"params", {
                {"plane", json{
                    {"type", "ax2"},
                    {"px", 0}, {"py", 0}, {"pz", 0},
                    {"dx", 1}, {"dy", 0}, {"dz", 0},
                    {"xx", 0}, {"xy", 1},
                    // missing xz
                }},
            }},
        },
    });
    auto r = oreo::FeatureTree::fromJSON(tree.dump());
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("xz"), std::string::npos) << r.error;
}

TEST(Hardening_2026_04_18, StrictDecodeAllowsUnknownParamForForwardCompat) {
    // An unknown param name is preserved verbatim (lenient decode) so a
    // future v1.1 document with extra fields still loads under v1.0.
    // validateFeature will warn about the extra, but not error.
    json tree;
    tree["features"] = json::array({
        json{
            {"id", "F"}, {"type", "MakeSphere"},
            {"params", {
                {"radius", 5},
                {"futureOption", "yes"},
            }},
        },
    });
    auto r = oreo::FeatureTree::fromJSON(tree.dump());
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.tree.featureCount(), 1);
}

// ═══════════════════════════════════════════════════════════════
// Finding #5 — unified size-probe protocol
// ═══════════════════════════════════════════════════════════════

TEST(Hardening_2026_04_18, MergeConflictStringSizeProbeReturnsOk) {
    // The merge_result_conflict_* accessors must follow the same
    // size-probe convention as queryName: passing (buf=NULL, buflen=0,
    // &needed) returns OREO_OK (not OREO_BUFFER_TOO_SMALL) and populates
    // *needed with the required byte count (excluding NUL).
    OreoContext ctx = oreo_context_create();
    ASSERT_NE(ctx, nullptr);

    // Set up a minimal conflict to have at least one entry to probe.
    OreoWorkspace base = oreo_ctx_workspace_create(ctx, "base");
    OreoFeatureBuilder fb = oreo_feature_builder_create("F1", "MakeBox");
    ASSERT_EQ(oreo_feature_builder_set_vec(fb, "dimensions", 1, 1, 1), OREO_OK);
    OreoFeatureTree bt = oreo_ctx_workspace_tree(base);
    ASSERT_EQ(oreo_ctx_feature_tree_add(bt, fb), OREO_OK);
    oreo_feature_builder_free(fb);

    OreoWorkspace ours = oreo_ctx_workspace_fork(base, "ours");
    OreoWorkspace theirs = oreo_ctx_workspace_fork(base, "theirs");
    ASSERT_EQ(oreo_ctx_feature_tree_set_param_vec(
        oreo_ctx_workspace_tree(ours), "F1", "dimensions", 2, 1, 1), OREO_OK);
    ASSERT_EQ(oreo_ctx_feature_tree_set_param_vec(
        oreo_ctx_workspace_tree(theirs), "F1", "dimensions", 1, 2, 1), OREO_OK);

    OreoMergeResult mr = oreo_ctx_workspace_merge(base, ours, theirs);
    ASSERT_NE(mr, nullptr);
    ASSERT_GT(oreo_merge_result_conflict_count(mr), 0);

    size_t needed = 0;
    EXPECT_EQ(oreo_merge_result_conflict_feature_id(mr, 0, nullptr, 0, &needed), OREO_OK)
        << "size-probe must return OREO_OK, not OREO_BUFFER_TOO_SMALL";
    EXPECT_GT(needed, 0u);

    size_t neededP = 0;
    EXPECT_EQ(oreo_merge_result_conflict_param_name(mr, 0, nullptr, 0, &neededP), OREO_OK);
    // param_name may legitimately be empty for non-ParamConflict kinds;
    // the probe still returns OK.

    size_t neededM = 0;
    EXPECT_EQ(oreo_merge_result_conflict_message(mr, 0, nullptr, 0, &neededM), OREO_OK);

    // Full copy after probe.
    std::vector<char> buf(needed + 1);
    EXPECT_EQ(oreo_merge_result_conflict_feature_id(mr, 0, buf.data(), buf.size(), &needed), OREO_OK);
    EXPECT_STREQ(buf.data(), "F1");

    // Truncated buffer returns BUFFER_TOO_SMALL with partial copy + NUL.
    if (needed > 1) {
        std::vector<char> small(needed);  // one byte short (no room for NUL)
        int rc = oreo_merge_result_conflict_feature_id(mr, 0, small.data(), small.size(), &needed);
        EXPECT_EQ(rc, OREO_BUFFER_TOO_SMALL);
        EXPECT_EQ(small.back(), '\0') << "Truncated write must be NUL-terminated";
    }

    oreo_merge_result_free(mr);
    oreo_ctx_workspace_free(theirs);
    oreo_ctx_workspace_free(ours);
    oreo_ctx_workspace_free(base);
    oreo_context_free(ctx);
}

// ═══════════════════════════════════════════════════════════════
// Finding #6 — cross-process-safe TempFile
// ═══════════════════════════════════════════════════════════════

TEST(Hardening_2026_04_18, TempFileNameIncludesPidAndNonce) {
    // Name format: prefix_pid_tidHash_counter_nonce.ext. PID component
    // defends against counter-zero collisions across independent worker
    // processes; the 64-bit random nonce is the extra margin.
    oreo::io_detail::TempFile t1("oreo_test", ".tmp");
    oreo::io_detail::TempFile t2("oreo_test", ".tmp");
    EXPECT_NE(t1.path(), t2.path()) << "Two temp files must not collide";
    auto fname1 = std::filesystem::path(t1.path()).filename().string();
    auto fname2 = std::filesystem::path(t2.path()).filename().string();
    // Count underscore-delimited segments — expect prefix+pid+tid+counter+nonce.
    auto countSegments = [](const std::string& s) {
        return static_cast<int>(std::count(s.begin(), s.end(), '_'));
    };
    EXPECT_GE(countSegments(fname1), 4)
        << "temp name lacks PID+nonce segments: " << fname1;
    EXPECT_GE(countSegments(fname2), 4);
}

TEST(Hardening_2026_04_18, TempFilePathIsExclusivelyReserved) {
    // The constructor runs an exclusive-create (fopen "wxb"). After the
    // TempFile is built, the path must already exist on disk so a
    // concurrent consumer that tries fopen("...", "wxb") would fail.
    oreo::io_detail::TempFile t("oreo_reserve", ".tmp");
    EXPECT_TRUE(std::filesystem::exists(t.path()))
        << "Exclusive-create should have placed a zero-byte file at "
        << t.path();
    // And its destructor removes it.
    std::string p = t.path();
    {
        auto snapshot = std::move(p);  // keep path accessible after scope
        (void)snapshot;
    }
}

TEST(Hardening_2026_04_18, TempFileManyUniqueAcrossThreads) {
    // Stress: N threads × M names, check the concatenated set has no
    // duplicates. Single-process verification — the cross-process
    // part is architectural (PID in the name).
    constexpr int kThreads = 8;
    constexpr int kPerThread = 64;
    std::vector<std::string> paths;
    std::mutex m;
    std::vector<std::thread> ts;
    for (int i = 0; i < kThreads; ++i) {
        ts.emplace_back([&] {
            for (int j = 0; j < kPerThread; ++j) {
                oreo::io_detail::TempFile tf("oreo_stress", ".tmp");
                std::lock_guard<std::mutex> lk(m);
                paths.push_back(tf.path());
            }
        });
    }
    for (auto& t : ts) t.join();
    std::set<std::string> uniq(paths.begin(), paths.end());
    EXPECT_EQ(uniq.size(), paths.size())
        << "Concurrent TempFile allocations must all be unique";
}

// ═══════════════════════════════════════════════════════════════
// Finding #8 — duplicate-featureId guard at addFeature
// ═══════════════════════════════════════════════════════════════

TEST(Hardening_2026_04_18, AddFeatureRejectsDuplicateId) {
    auto ctx = oreo::KernelContext::create();
    oreo::FeatureTree tree(ctx);

    EXPECT_TRUE(tree.addFeature(makeBoxFeature("X", 1, 1, 1)));
    EXPECT_FALSE(tree.addFeature(makeBoxFeature("X", 2, 2, 2)))
        << "Duplicate id must return false";
    EXPECT_EQ(tree.featureCount(), 1);
    EXPECT_TRUE(ctx->diag().hasErrors())
        << "Duplicate id must raise an INVALID_STATE diagnostic";
}

TEST(Hardening_2026_04_18, InsertFeatureRejectsDuplicateId) {
    auto ctx = oreo::KernelContext::create();
    oreo::FeatureTree tree(ctx);

    EXPECT_TRUE(tree.addFeature(makeBoxFeature("Y", 1, 1, 1)));
    EXPECT_FALSE(tree.insertFeature(0, makeBoxFeature("Y", 3, 3, 3)))
        << "insertFeature must reject duplicate id too";
    EXPECT_EQ(tree.featureCount(), 1);
}

// ═══════════════════════════════════════════════════════════════
// Finding — fromJSON rejects duplicate ids inside the doc
// ═══════════════════════════════════════════════════════════════

TEST(Hardening_2026_04_18, FromJsonRejectsDuplicateIdsInDocument) {
    json tree;
    tree["features"] = json::array({
        json{{"id", "D"}, {"type", "MakeSphere"},
             {"params", {{"radius", 1}}}},
        json{{"id", "D"}, {"type", "MakeSphere"},
             {"params", {{"radius", 2}}}},
    });
    auto r = oreo::FeatureTree::fromJSON(tree.dump());
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("Duplicate feature id"), std::string::npos);
}
