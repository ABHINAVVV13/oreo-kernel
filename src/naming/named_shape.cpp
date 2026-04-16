// named_shape.cpp — NamedShape implementation.

#include "named_shape.h"

#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

namespace oreo {

int NamedShape::countSubShapes(TopAbs_ShapeEnum type) const {
    // Use IndexedMapOfShape for unique sub-shapes (not duplicates from shared faces/edges)
    TopTools_IndexedMapOfShape map;
    TopExp::MapShapes(shape_, type, map);
    return map.Extent();
}

TopoDS_Shape NamedShape::getSubShape(TopAbs_ShapeEnum type, int index) const {
    TopTools_IndexedMapOfShape map;
    TopExp::MapShapes(shape_, type, map);
    if (index < 1 || index > map.Extent()) return {};
    return map(index);
}

MappedName NamedShape::getElementName(const IndexedName& index) const {
    if (!elementMap_) return {};
    return elementMap_->getMappedName(index);
}

IndexedName NamedShape::getIndexedName(const MappedName& name) const {
    if (!elementMap_) return {};
    return elementMap_->getIndexedName(name);
}

} // namespace oreo
