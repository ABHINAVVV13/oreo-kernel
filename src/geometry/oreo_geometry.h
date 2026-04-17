// SPDX-License-Identifier: LGPL-2.1-or-later

// oreo_geometry.h — All geometry operations for oreo-kernel.
//
// Every operation:
//   1. Takes KernelContext& as first parameter
//   2. Returns OperationResult<NamedShape> with attached diagnostics
//   3. Uses DiagnosticScope for composable diagnostics
//   4. Converts dimension parameters through ctx.units
//   5. Validates all inputs fail-closed (NaN/Inf rejected)
//
// OperationResult carries:
//   - The NamedShape value (if operation succeeded)
//   - Diagnostics from this specific operation
//   - Success/failure flag + degraded-geometry/naming flags

#ifndef OREO_GEOMETRY_H
#define OREO_GEOMETRY_H

#include "core/kernel_context.h"
#include "core/operation_result.h"
#include "naming/named_shape.h"

#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <vector>

namespace oreo {

// Shorthand for the common return type
using GeomResult = OperationResult<NamedShape>;

// ═══════════════════════════════════════════════════════════════
// Primitives
// ═══════════════════════════════════════════════════════════════

GeomResult makeBox(KernelContext& ctx, double dx, double dy, double dz);
GeomResult makeBox(KernelContext& ctx, const gp_Pnt& origin, double dx, double dy, double dz);
GeomResult makeCylinder(KernelContext& ctx, double radius, double height);
GeomResult makeCylinder(KernelContext& ctx, const gp_Ax2& axis, double radius, double height);
GeomResult makeSphere(KernelContext& ctx, double radius);
GeomResult makeSphere(KernelContext& ctx, const gp_Pnt& center, double radius);
GeomResult makeCone(KernelContext& ctx, double radius1, double radius2, double height);
GeomResult makeTorus(KernelContext& ctx, double majorRadius, double minorRadius);
GeomResult makeWedge(KernelContext& ctx, double dx, double dy, double dz, double ltx);

// ═══════════════════════════════════════════════════════════════
// Core Operations
// ═══════════════════════════════════════════════════════════════

GeomResult extrude(KernelContext& ctx, const NamedShape& base, const gp_Vec& direction);
GeomResult revolve(KernelContext& ctx, const NamedShape& base, const gp_Ax1& axis, double angleRad);
GeomResult fillet(KernelContext& ctx, const NamedShape& solid,
                  const std::vector<NamedEdge>& edges, double radius);
GeomResult chamfer(KernelContext& ctx, const NamedShape& solid,
                   const std::vector<NamedEdge>& edges, double distance);
GeomResult booleanUnion(KernelContext& ctx, const NamedShape& a, const NamedShape& b, double tolerance = -1.0);
GeomResult booleanSubtract(KernelContext& ctx, const NamedShape& target, const NamedShape& tool, double tolerance = -1.0);
GeomResult booleanIntersect(KernelContext& ctx, const NamedShape& a, const NamedShape& b, double tolerance = -1.0);
GeomResult shell(KernelContext& ctx, const NamedShape& solid,
                 const std::vector<NamedFace>& facesToRemove, double thickness);
GeomResult loft(KernelContext& ctx, const std::vector<NamedShape>& profiles, bool makeSolid = true);
GeomResult sweep(KernelContext& ctx, const NamedShape& profile, const NamedShape& path);
GeomResult mirror(KernelContext& ctx, const NamedShape& solid, const gp_Ax2& plane);
GeomResult patternLinear(KernelContext& ctx, const NamedShape& solid, const gp_Vec& direction, int count, double spacing);
GeomResult patternCircular(KernelContext& ctx, const NamedShape& solid, const gp_Ax1& axis, int count, double totalAngleRad);

// ═══════════════════════════════════════════════════════════════
// Manufacturing Operations
// ═══════════════════════════════════════════════════════════════

GeomResult draft(KernelContext& ctx, const NamedShape& solid,
                 const std::vector<NamedFace>& faces, double angleDeg, const gp_Dir& pullDirection);

enum class HoleType { Through, Blind, Counterbore, Countersink };

GeomResult hole(KernelContext& ctx, const NamedShape& solid, const NamedFace& face,
                const gp_Pnt& center, double diameter, double depth,
                HoleType type = HoleType::Blind);

GeomResult rib(KernelContext& ctx, const NamedShape& solid, const NamedShape& ribProfile,
               const gp_Dir& direction, double thickness);

GeomResult pocket(KernelContext& ctx, const NamedShape& solid, const NamedShape& profile, double depth);

// ═══════════════════════════════════════════════════════════════
// Surface / Face Operations
// ═══════════════════════════════════════════════════════════════

GeomResult offset(KernelContext& ctx, const NamedShape& solid, double distance);
GeomResult thicken(KernelContext& ctx, const NamedShape& shellOrFace, double thickness);
GeomResult splitBody(KernelContext& ctx, const NamedShape& solid, const gp_Pln& plane);

// ═══════════════════════════════════════════════════════════════
// Advanced Fillet/Chamfer
// ═══════════════════════════════════════════════════════════════

GeomResult filletVariable(KernelContext& ctx, const NamedShape& solid,
                          const NamedEdge& edge, double startRadius, double endRadius);

// ═══════════════════════════════════════════════════════════════
// Wire / Sketch Geometry
// ═══════════════════════════════════════════════════════════════

GeomResult wireOffset(KernelContext& ctx, const NamedShape& wire, double distance);
GeomResult wireFillet(KernelContext& ctx, const NamedShape& wire,
                      const std::vector<NamedEdge>& vertices, double radius);
GeomResult makeFaceFromWire(KernelContext& ctx, const NamedShape& wire);

// ═══════════════════════════════════════════════════════════════
// Multi-body
// ═══════════════════════════════════════════════════════════════

GeomResult combine(KernelContext& ctx, const std::vector<NamedShape>& shapes);

} // namespace oreo

#endif // OREO_GEOMETRY_H
