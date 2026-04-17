// SPDX-License-Identifier: LGPL-2.1-or-later

// oreo_query.h — Shape introspection and measurement.
//
// Every query takes a KernelContext& as its first parameter.
// Every query takes a KernelContext& as its first parameter.

#ifndef OREO_QUERY_H
#define OREO_QUERY_H

#include "core/kernel_context.h"
#include "core/operation_result.h"
#include "core/diagnostic_scope.h"
#include "naming/named_shape.h"

namespace oreo {

struct BBox {
    double xmin, ymin, zmin;
    double xmax, ymax, zmax;
};

struct MassProperties {
    double volume;
    double surfaceArea;
    double centerOfMassX, centerOfMassY, centerOfMassZ;
    // Moments of inertia (Ixx, Iyy, Izz, Ixy, Ixz, Iyz)
    double ixx, iyy, izz, ixy, ixz, iyz;
};

// ═══════════════════════════════════════════════════════════════
// Context-aware query functions
// ═══════════════════════════════════════════════════════════════

// Axis-aligned bounding box
OperationResult<BBox> aabb(KernelContext& ctx, const NamedShape& shape);

// Change footprint: AABB of before vs after
OperationResult<BBox> footprint(KernelContext& ctx, const NamedShape& before, const NamedShape& after);

// Get all face sub-shapes with their element-map names
OperationResult<std::vector<NamedFace>> getFaces(KernelContext& ctx, const NamedShape& shape);

// Get all edge sub-shapes with their element-map names
OperationResult<std::vector<NamedEdge>> getEdges(KernelContext& ctx, const NamedShape& shape);

// Minimum distance between two entities
OperationResult<double> measureDistance(KernelContext& ctx, const NamedShape& a, const NamedShape& b);

// Mass properties (volume, surface area, center of mass, moments of inertia)
OperationResult<MassProperties> massProperties(KernelContext& ctx, const NamedShape& shape);

} // namespace oreo

#endif // OREO_QUERY_H
