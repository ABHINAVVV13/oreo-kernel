// test_step_roundtrip.cpp — STEP export -> re-import -> compare.

#include <gtest/gtest.h>

#include "core/kernel_context.h"
#include "core/oreo_error.h"
#include "geometry/oreo_geometry.h"
#include "io/oreo_step.h"
#include "naming/named_shape.h"
#include "query/oreo_query.h"

#include <BRepPrimAPI_MakeBox.hxx>

namespace {

oreo::NamedShape makeBoxLocal(double x, double y, double z) {
    TopoDS_Shape box = BRepPrimAPI_MakeBox(x, y, z).Shape();
    return oreo::NamedShape(box, oreo::NamedShape::nextTag());
}

} // anonymous namespace

TEST(StepIO, ExportImportRoundTrip) {
    auto ctx = oreo::KernelContext::create();
    auto box = makeBoxLocal(10, 20, 30);

    // Export
    auto stepData = oreo::exportStep(*ctx, {box});
    ASSERT_FALSE(stepData.empty());

    // Re-import
    auto result = oreo::importStep(*ctx, stepData.data(), stepData.size());
    ASSERT_FALSE(result.isNull());

    auto& imported = result.shape;

    // Compare face/edge counts
    EXPECT_EQ(imported.countSubShapes(TopAbs_FACE), box.countSubShapes(TopAbs_FACE));
    EXPECT_EQ(imported.countSubShapes(TopAbs_EDGE), box.countSubShapes(TopAbs_EDGE));

    // Compare volume
    auto origProps = oreo::massProperties(*ctx, box);
    auto importProps = oreo::massProperties(*ctx, imported);
    EXPECT_NEAR(importProps.volume, origProps.volume, 1.0);
}

TEST(StepIO, ExportFileThenImport) {
    auto ctx = oreo::KernelContext::create();
    auto box = makeBoxLocal(15, 25, 35);

    std::string path = "test_step_roundtrip.step";
    ASSERT_TRUE(oreo::exportStepFile(*ctx, {box}, path));

    auto result = oreo::importStepFile(*ctx, path);
    ASSERT_FALSE(result.isNull());

    auto origProps = oreo::massProperties(*ctx, box);
    auto importProps = oreo::massProperties(*ctx, result.shape);
    EXPECT_NEAR(importProps.volume, origProps.volume, 1.0);

    // Cleanup
    std::remove(path.c_str());
}

TEST(StepIO, ImportInvalidData) {
    auto ctx = oreo::KernelContext::create();
    uint8_t garbage[] = {0, 1, 2, 3, 4};
    auto importResult = oreo::importStep(*ctx, garbage, sizeof(garbage));
    EXPECT_TRUE(importResult.isNull());
    EXPECT_TRUE(ctx->diag.hasErrors());
}
