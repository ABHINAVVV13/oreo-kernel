// SPDX-License-Identifier: LGPL-2.1-or-later

// Smoke test: verify OCCT links, GTest works, we can create a box.

#include <gtest/gtest.h>

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <TopoDS_Shape.hxx>

TEST(Smoke, OcctMakeBox) {
    TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape();
    ASSERT_FALSE(box.IsNull());

    Bnd_Box bbox;
    BRepBndLib::Add(box, bbox);

    double xmin, ymin, zmin, xmax, ymax, zmax;
    bbox.Get(xmin, ymin, zmin, xmax, ymax, zmax);

    EXPECT_NEAR(xmax - xmin, 10.0, 1e-6);
    EXPECT_NEAR(ymax - ymin, 20.0, 1e-6);
    EXPECT_NEAR(zmax - zmin, 30.0, 1e-6);
}

TEST(Smoke, GTestWorks) {
    EXPECT_EQ(1 + 1, 2);
}
