// map_shape_elements.cpp — Bridge between geometry operations and the real
// FreeCAD element-mapping algorithm.
//
// Context-aware: receives KernelContext& for tag allocation and diagnostics.

#include "map_shape_elements.h"
#include "topo_shape_adapter.h"

namespace oreo {

NamedShape mapShapeElements(
    KernelContext& ctx,
    const TopoDS_Shape& resultShape,
    const ShapeMapper& mapper,
    const std::vector<NamedShape>& inputShapes,
    long opTag,
    const char* opName)
{
    if (resultShape.IsNull()) {
        auto map = std::make_shared<ElementMap>();
        return NamedShape(resultShape, map, opTag);
    }

    // Convert inputs to adapters
    auto inputAdapters = TopoShapeAdapter::fromNamedShapes(inputShapes);

    // Run the full FreeCAD algorithm
    TopoShapeAdapter adapter;
    adapter.Tag = opTag;
    adapter.buildElementMap(resultShape, mapper, inputAdapters, opName);

    // Convert back to NamedShape
    return adapter.toNamedShape();
}

} // namespace oreo
