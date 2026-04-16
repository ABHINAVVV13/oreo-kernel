// named_shape.h — TopoDS_Shape + ElementMap: the core type for oreo-kernel.
// Every geometry operation returns a NamedShape.
//
// A NamedShape carries:
//   1. The OCCT TopoDS_Shape (the actual geometry)
//   2. An ElementMap (stable names for all sub-shapes)
//   3. A tag (unique ID for this operation in the history chain)

#ifndef OREO_NAMED_SHAPE_H
#define OREO_NAMED_SHAPE_H

#include "element_map.h"

#include <TopoDS_Shape.hxx>
#include <TopExp_Explorer.hxx>
#include <TopAbs_ShapeEnum.hxx>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace oreo {

class NamedShape {
public:
    NamedShape() : tag_(0) {}

    NamedShape(const TopoDS_Shape& shape, int64_t tag = 0)
        : shape_(shape)
        , elementMap_(std::make_shared<ElementMap>())
        , tag_(tag)
    {}

    NamedShape(const TopoDS_Shape& shape, ElementMapPtr map, int64_t tag)
        : shape_(shape)
        , elementMap_(map ? map : std::make_shared<ElementMap>())
        , tag_(tag)
    {}

    // --- Accessors ---

    const TopoDS_Shape& shape() const { return shape_; }
    TopoDS_Shape& shape() { return shape_; }
    void setShape(const TopoDS_Shape& s) { shape_ = s; }

    ElementMapPtr elementMap() const { return elementMap_; }
    void setElementMap(ElementMapPtr map) { elementMap_ = map; }

    int64_t tag() const { return tag_; }
    void setTag(int64_t t) { tag_ = t; }

    bool isNull() const { return shape_.IsNull(); }

    // --- Sub-shape queries ---

    // Count sub-shapes of a given type
    int countSubShapes(TopAbs_ShapeEnum type) const;

    // Get the i-th sub-shape (1-based, matching OCCT convention)
    TopoDS_Shape getSubShape(TopAbs_ShapeEnum type, int index) const;

    // Get the stable name for an indexed sub-shape
    MappedName getElementName(const IndexedName& index) const;

    // Get the indexed name for a stable name
    IndexedName getIndexedName(const MappedName& name) const;

    // --- Tag generation ---

    // Generate a unique tag for a new operation
    static int64_t nextTag();

private:
    TopoDS_Shape shape_;
    ElementMapPtr elementMap_;
    int64_t tag_;

    static std::atomic<int64_t> tagCounter_;
};

// Convenience: list of named edges for fillet/chamfer input
struct NamedEdge {
    IndexedName name;
    TopoDS_Shape edge;  // The actual TopoDS_Edge
};

// Convenience: list of named faces for shell input
struct NamedFace {
    IndexedName name;
    TopoDS_Shape face;  // The actual TopoDS_Face
};

} // namespace oreo

#endif // OREO_NAMED_SHAPE_H
