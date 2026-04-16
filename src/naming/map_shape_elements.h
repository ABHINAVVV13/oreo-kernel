// map_shape_elements.h — Build element map for an operation result.
//
// Central function for topological naming. After every OCCT geometry
// operation, call mapShapeElements() to trace which input elements
// became which output elements, and encode that history.

#ifndef OREO_MAP_SHAPE_ELEMENTS_H
#define OREO_MAP_SHAPE_ELEMENTS_H

#include "named_shape.h"
#include "shape_mapper.h"
#include "core/kernel_context.h"

#include <string>
#include <vector>

namespace oreo {

// Context-aware version (preferred)
NamedShape mapShapeElements(
    KernelContext& ctx,
    const TopoDS_Shape& resultShape,
    const ShapeMapper& mapper,
    const std::vector<NamedShape>& inputShapes,
    long opTag,
    const char* opName = nullptr);

} // namespace oreo

#endif // OREO_MAP_SHAPE_ELEMENTS_H
