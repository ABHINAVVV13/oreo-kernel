// SPDX-License-Identifier: LGPL-2.1-or-later

// shape_mapper.h — Abstract interface for tracking element identity through operations.
// Inspired by FreeCAD 1.0's TopoShapeMapper (LGPL-2.1+), clean reimplementation.
//
// When OCCT performs a boolean, fillet, or extrude, input faces/edges become
// output faces/edges. The Mapper interface queries OCCT's history to find
// which output elements came from which input elements.
//
// Two key queries:
//   modified(input) → list of output shapes that are modified versions of input
//   generated(input) → list of output shapes that were newly created from input

#ifndef OREO_SHAPE_MAPPER_H
#define OREO_SHAPE_MAPPER_H

#include <BRepBuilderAPI_MakeShape.hxx>
#include <BRepTools_History.hxx>
#include <ShapeFix_Root.hxx>
#include <TopoDS_Shape.hxx>
#include <TopTools_ListOfShape.hxx>

#include <memory>
#include <vector>

namespace oreo {

// Abstract mapper interface
class ShapeMapper {
public:
    virtual ~ShapeMapper() = default;
    virtual std::vector<TopoDS_Shape> modified(const TopoDS_Shape& input) const = 0;
    virtual std::vector<TopoDS_Shape> generated(const TopoDS_Shape& input) const = 0;
};

// Mapper for BRepBuilderAPI_MakeShape (extrude, revolve, fillet, etc.)
class MakerMapper : public ShapeMapper {
public:
    explicit MakerMapper(BRepBuilderAPI_MakeShape& maker) : maker_(maker) {}

    std::vector<TopoDS_Shape> modified(const TopoDS_Shape& input) const override {
        std::vector<TopoDS_Shape> result;
        const TopTools_ListOfShape& list = maker_.Modified(input);
        for (TopTools_ListIteratorOfListOfShape it(list); it.More(); it.Next()) {
            result.push_back(it.Value());
        }
        return result;
    }

    std::vector<TopoDS_Shape> generated(const TopoDS_Shape& input) const override {
        std::vector<TopoDS_Shape> result;
        const TopTools_ListOfShape& list = maker_.Generated(input);
        for (TopTools_ListIteratorOfListOfShape it(list); it.More(); it.Next()) {
            result.push_back(it.Value());
        }
        return result;
    }

private:
    BRepBuilderAPI_MakeShape& maker_;
};

// Mapper for BRepTools_History (lower-level tracking, used by booleans in OCCT 7.4+)
class HistoryMapper : public ShapeMapper {
public:
    explicit HistoryMapper(const Handle(BRepTools_History)& history) : history_(history) {}

    std::vector<TopoDS_Shape> modified(const TopoDS_Shape& input) const override {
        std::vector<TopoDS_Shape> result;
        if (history_.IsNull()) return result;
        const TopTools_ListOfShape& list = history_->Modified(input);
        for (TopTools_ListIteratorOfListOfShape it(list); it.More(); it.Next()) {
            result.push_back(it.Value());
        }
        return result;
    }

    std::vector<TopoDS_Shape> generated(const TopoDS_Shape& input) const override {
        std::vector<TopoDS_Shape> result;
        if (history_.IsNull()) return result;
        const TopTools_ListOfShape& list = history_->Generated(input);
        for (TopTools_ListIteratorOfListOfShape it(list); it.More(); it.Next()) {
            result.push_back(it.Value());
        }
        return result;
    }

private:
    Handle(BRepTools_History) history_;
};

// Null mapper (identity — no history tracking)
class NullMapper : public ShapeMapper {
public:
    std::vector<TopoDS_Shape> modified(const TopoDS_Shape&) const override { return {}; }
    std::vector<TopoDS_Shape> generated(const TopoDS_Shape&) const override { return {}; }
};

} // namespace oreo

#endif // OREO_SHAPE_MAPPER_H
