// test_step_roundtrip.cpp — STEP export → re-import → compare.

#include <gtest/gtest.h>

#include "core/oreo_error.h"
#include "geometry/oreo_geometry.h"
#include "io/oreo_step.h"
#include "naming/named_shape.h"
#include "query/oreo_query.h"

#include <BRepPrimAPI_MakeBox.hxx>

namespace {

oreo::NamedShape makeBox(double x, double y, double z) {
    TopoDS_Shape box = BRepPrimAPI_MakeBox(x, y, z).Shape();
    return oreo::NamedShape(box, oreo::NamedShape::nextTag());
}

} // anonymous namespace

TEST(StepIO, ExportImportRoundTrip) {
    auto box = makeBox(10, 20, 30);

    // Export
    auto stepData = oreo::exportStep({box});
    ASSERT_FALSE(stepData.empty());

    // Re-import
    auto result = oreo::importStep(stepData.data(), stepData.size());
    ASSERT_FALSE(result.isNull());

    auto& imported = result.shape;

    // Compare face/edge counts
    EXPECT_EQ(imported.countSubShapes(TopAbs_FACE), box.countSubShapes(TopAbs_FACE));
    EXPECT_EQ(imported.countSubShapes(TopAbs_EDGE), box.countSubShapes(TopAbs_EDGE));

    // Compare volume
    auto origProps = oreo::massProperties(box);
    auto importProps = oreo::massProperties(imported);
    EXPECT_NEAR(importProps.volume, origProps.volume, 1.0);
}

TEST(StepIO, ExportFileThenImport) {
    auto box = makeBox(15, 25, 35);

    std::string path = "test_step_roundtrip.step";
    ASSERT_TRUE(oreo::exportStepFile({box}, path));

    auto result = oreo::importStepFile(path);
    ASSERT_FALSE(result.isNull());

    auto origProps = oreo::massProperties(box);
    auto importProps = oreo::massProperties(result.shape);
    EXPECT_NEAR(importProps.volume, origProps.volume, 1.0);

    // Cleanup
    std::remove(path.c_str());
}

TEST(StepIO, ImportInvalidData) {
    uint8_t garbage[] = {0, 1, 2, 3, 4};
    auto importResult = oreo::importStep(garbage, sizeof(garbage));
    EXPECT_TRUE(importResult.isNull());
    EXPECT_EQ(oreo::getLastError().code, oreo::ErrorCode::STEP_IMPORT_FAILED);
}
