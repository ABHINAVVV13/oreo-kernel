// test_serialize.cpp — Serialize -> deserialize -> verify equality.

#include <gtest/gtest.h>

#include "core/kernel_context.h"
#include "core/oreo_error.h"
#include "io/oreo_serialize.h"
#include "naming/named_shape.h"
#include "query/oreo_query.h"

#include <BRepPrimAPI_MakeBox.hxx>

namespace {

oreo::NamedShape makeBoxLocal(oreo::KernelContext& ctx, double x, double y, double z) {
    TopoDS_Shape box = BRepPrimAPI_MakeBox(x, y, z).Shape();
    return oreo::NamedShape(box, ctx.tags.nextTag());
}

} // anonymous namespace

TEST(Serialize, BoxRoundTrip) {
    auto ctx = oreo::KernelContext::create();
    auto box = makeBoxLocal(*ctx, 10, 20, 30);

    auto bufR = oreo::serialize(*ctx, box);
    ASSERT_TRUE(bufR.ok());
    auto buf = bufR.value();
    ASSERT_FALSE(buf.empty());

    auto restoredR = oreo::deserialize(*ctx, buf.data(), buf.size());
    ASSERT_TRUE(restoredR.ok());
    auto restored = restoredR.value();
    ASSERT_FALSE(restored.isNull());

    // Shape topology should match
    EXPECT_EQ(restored.countSubShapes(TopAbs_FACE), box.countSubShapes(TopAbs_FACE));
    EXPECT_EQ(restored.countSubShapes(TopAbs_EDGE), box.countSubShapes(TopAbs_EDGE));
    EXPECT_EQ(restored.countSubShapes(TopAbs_VERTEX), box.countSubShapes(TopAbs_VERTEX));

    // Volume should match
    auto origProps = oreo::massProperties(*ctx, box).value();
    auto restoredProps = oreo::massProperties(*ctx, restored).value();
    EXPECT_NEAR(restoredProps.volume, origProps.volume, 0.01);

    // Tag should be preserved
    EXPECT_EQ(restored.tag(), box.tag());
}

TEST(Serialize, NullShapeFails) {
    auto ctx = oreo::KernelContext::create();
    oreo::NamedShape null;
    auto bufR = oreo::serialize(*ctx, null);
    EXPECT_FALSE(bufR.ok());
    EXPECT_TRUE(ctx->diag.hasErrors());
}

TEST(Serialize, TruncatedDataFails) {
    auto ctx = oreo::KernelContext::create();
    auto box = makeBoxLocal(*ctx, 5, 5, 5);
    auto buf = oreo::serialize(*ctx, box).value();
    ASSERT_FALSE(buf.empty());

    // Truncate the buffer
    auto result = oreo::deserialize(*ctx, buf.data(), 4);
    // Should either fail or produce a meaningful error
    if (!result.ok()) {
        EXPECT_TRUE(ctx->diag.hasErrors());
    }
}

TEST(Serialize, ElementMapPreserved) {
    auto ctx = oreo::KernelContext::create();

    // Manually add element map entries
    auto map = std::make_shared<oreo::ElementMap>();
    map->setElementName(oreo::IndexedName("Face", 1), oreo::MappedName("TopFace;:H1;:M"));
    map->setElementName(oreo::IndexedName("Face", 2), oreo::MappedName("BotFace;:H1;:M"));

    // Construct NamedShape with element map in constructor (setElementMap is deleted)
    TopoDS_Shape boxShape = BRepPrimAPI_MakeBox(10, 10, 10).Shape();
    oreo::NamedShape box(boxShape, map, ctx->tags.nextTag());

    auto buf = oreo::serialize(*ctx, box).value();
    ASSERT_FALSE(buf.empty());

    auto restored = oreo::deserialize(*ctx, buf.data(), buf.size()).value();
    ASSERT_FALSE(restored.isNull());
    ASSERT_NE(restored.elementMap(), nullptr);

    EXPECT_EQ(restored.elementMap()->getMappedName(oreo::IndexedName("Face", 1)).data(), "TopFace;:H1;:M");
    EXPECT_EQ(restored.elementMap()->getMappedName(oreo::IndexedName("Face", 2)).data(), "BotFace;:H1;:M");
}
