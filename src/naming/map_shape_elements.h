// SPDX-License-Identifier: LGPL-2.1-or-later

// map_shape_elements.h — Build element map for an operation result.
//
// Central function for topological naming. After every OCCT geometry
// operation, call mapShapeElements() to trace which input elements
// became which output elements, and encode that history.

#ifndef OREO_MAP_SHAPE_ELEMENTS_H
#define OREO_MAP_SHAPE_ELEMENTS_H

#include "core/kernel_context.h"
#include "core/shape_identity.h"
#include "named_shape.h"
#include "shape_mapper.h"

#include <cstdint>
#include <string>
#include <vector>

namespace oreo {

// Phase 3: opId is the per-operation ShapeIdentity from
// ctx.tags().nextShapeIdentity() — a full 64+64 identity, no squeeze.
NamedShape mapShapeElements(
    KernelContext& ctx,
    const TopoDS_Shape& resultShape,
    const ShapeMapper& mapper,
    const std::vector<NamedShape>& inputShapes,
    ShapeIdentity opId,
    const char* opName = nullptr);

} // namespace oreo

#endif // OREO_MAP_SHAPE_ELEMENTS_H
