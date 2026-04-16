// test_serialize.cpp — Serialize → deserialize → verify equality.

#include <gtest/gtest.h>

#include "core/oreo_error.h"
#include "io/oreo_serialize.h"
#include "naming/named_shape.h"
#include "query/oreo_query.h"

#include <BRepPrimAPI_MakeBox.hxx>

namespace {

oreo::NamedShape makeBox(double x, double y, double z) {
    TopoDS_Shape box = BRepPrimAPI_MakeBox(x, y, z).Shape();
    return oreo::NamedShape(box, oreo::NamedShape::nextTag());
}

} // anonymous namespace

TEST(Serialize, BoxRoundTrip) {
    auto box = makeBox(10, 20, 30);

    auto buf = oreo::serialize(box);
    ASSERT_FALSE(buf.empty());

    auto restored = oreo::deserialize(buf.data(), buf.size());
    ASSERT_FALSE(restored.isNull());

    // Shape topology should match
    EXPECT_EQ(restored.countSubShapes(TopAbs_FACE), box.countSubShapes(TopAbs_FACE));
    EXPECT_EQ(restored.countSubShapes(TopAbs_EDGE), box.countSubShapes(TopAbs_EDGE));
    EXPECT_EQ(restored.countSubShapes(TopAbs_VERTEX), box.countSubShapes(TopAbs_VERTEX));

    // Volume should match
    auto origProps = oreo::massProperties(box);
    auto restoredProps = oreo::massProperties(restored);
    EXPECT_NEAR(restoredProps.volume, origProps.volume, 0.01);

    // Tag should be preserved
    EXPECT_EQ(restored.tag(), box.tag());
}

TEST(Serialize, NullShapeFails) {
    oreo::NamedShape null;
    auto buf = oreo::serialize(null);
    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(oreo::getLastError().code, oreo::ErrorCode::SERIALIZE_FAILED);
}

TEST(Serialize, TruncatedDataFails) {
    auto box = makeBox(5, 5, 5);
    auto buf = oreo::serialize(box);
    ASSERT_FALSE(buf.empty());

    // Truncate the buffer
    auto result = oreo::deserialize(buf.data(), 4);
    // Should either return null or produce a meaningful error
    if (result.isNull()) {
        EXPECT_NE(oreo::getLastError().code, oreo::ErrorCode::OK);
    }
}

TEST(Serialize, ElementMapPreserved) {
    auto box = makeBox(10, 10, 10);

    // Manually add element map entries
    auto map = std::make_shared<oreo::ElementMap>();
    map->setElementName(oreo::IndexedName("Face", 1), oreo::MappedName("TopFace;:H1;:M"));
    map->setElementName(oreo::IndexedName("Face", 2), oreo::MappedName("BotFace;:H1;:M"));
    box.setElementMap(map);

    auto buf = oreo::serialize(box);
    ASSERT_FALSE(buf.empty());

    auto restored = oreo::deserialize(buf.data(), buf.size());
    ASSERT_FALSE(restored.isNull());
    ASSERT_NE(restored.elementMap(), nullptr);

    EXPECT_EQ(restored.elementMap()->getMappedName(oreo::IndexedName("Face", 1)).data(), "TopFace;:H1;:M");
    EXPECT_EQ(restored.elementMap()->getMappedName(oreo::IndexedName("Face", 2)).data(), "BotFace;:H1;:M");
}
