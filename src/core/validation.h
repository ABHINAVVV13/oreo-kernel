// SPDX-License-Identifier: LGPL-2.1-or-later

// validation.h — Strict input validation for all kernel operations.
//
// Every public kernel operation must validate its inputs before execution.
// Invalid inputs produce a Diagnostic with severity Error and return false.
// No silent fallbacks, no default values, no ignored errors.
//
// Two flavors of helper are provided:
//   require*  — validate input and report a diagnostic on failure (primary API).
//   check*    — silent validation; returns bool without reporting. Use when
//               the caller wants to branch on validity without producing a
//               diagnostic (e.g. inside speculative fit routines).
//
// Usage pattern:
//   NamedShape extrude(KernelContext& ctx, const NamedShape& base, const gp_Vec& dir) {
//       if (!validation::requireNonNull(ctx, base, "base")) return {};
//       if (!validation::requireNonZeroVec(ctx, dir, "direction")) return {};
//       // ... proceed with OCCT call
//   }

#ifndef OREO_VALIDATION_H
#define OREO_VALIDATION_H

#include "thread_safety.h"
#include "diagnostic.h"
#include "kernel_context.h"

#include <gp_Vec.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <TopoDS_Shape.hxx>
#include <TopAbs_ShapeEnum.hxx>

#include <cstddef>
#include <string>
#include <vector>

namespace oreo {

// Forward declarations
class NamedShape;
enum class LengthUnit;
enum class AngleUnit;

namespace validation {

// ─── Shape validation ────────────────────────────────────────

// Silent checks — return bool without reporting. VA-7.
bool checkNonNull(const NamedShape& shape);
bool checkShapeType(const TopoDS_Shape& shape, TopAbs_ShapeEnum expected);

// Shape must not be null
bool requireNonNull(KernelContext& ctx, const NamedShape& shape, const char* paramName);

// Shape must be of a specific type (face, edge, vertex, etc.)
bool requireShapeType(KernelContext& ctx, const TopoDS_Shape& shape,
                      TopAbs_ShapeEnum expected, const char* paramName);

// Shape must be a valid edge (type check only)
bool requireValidEdge(KernelContext& ctx, const TopoDS_Shape& shape, const char* paramName);

// Shape must be a valid face (type check only)
bool requireValidFace(KernelContext& ctx, const TopoDS_Shape& shape, const char* paramName);

// Shape must be a valid wire (type check only)
bool requireValidWire(KernelContext& ctx, const TopoDS_Shape& shape, const char* paramName);

// VA-9: stricter validators built on BRepCheck_Analyzer — verify type AND
// underlying geometry validity. Prefer these for load-bearing inputs.
bool requireValidEdgeGeometry(KernelContext& ctx, const TopoDS_Shape& shape, const char* paramName);
bool requireValidFaceGeometry(KernelContext& ctx, const TopoDS_Shape& shape, const char* paramName);
bool requireValidWireGeometry(KernelContext& ctx, const TopoDS_Shape& shape, const char* paramName);

// ─── Numeric validation ──────────────────────────────────────

// Value must be > 0
bool requirePositive(KernelContext& ctx, double value, const char* paramName);

// Value must be >= 0
bool requireNonNegative(KernelContext& ctx, double value, const char* paramName);

// Value must be within [min, max]
bool requireInRange(KernelContext& ctx, double value, double min, double max, const char* paramName);

// Integer must be >= minVal
bool requireMinInt(KernelContext& ctx, int value, int minVal, const char* paramName);

// VA-5: tolerance must be finite, positive, and within [minAllowed, maxAllowed].
// Defaults cover the usable range for OCCT linear tolerances.
bool requireTolerance(KernelContext& ctx, double tol, const char* paramName,
                      double minAllowed = 1e-12, double maxAllowed = 1.0);

// VA-10: every element of values[0..n) must be finite and > 0. Reports the
// index of the first violator on failure.
bool requireAllPositive(KernelContext& ctx, const double* values, std::size_t n,
                        const char* paramName);

// ─── Vector/direction validation ─────────────────────────────

// Vector must have non-zero magnitude
bool requireNonZeroVec(KernelContext& ctx, const gp_Vec& vec, const char* paramName);

// Direction must be valid (non-degenerate, unit-length)
bool requireValidDir(KernelContext& ctx, const gp_Dir& dir, const char* paramName);

// Point must have finite coordinates (no NaN or Inf)
bool requireFinitePoint(KernelContext& ctx, const gp_Pnt& point, const char* paramName);

// Axis must have finite location and valid direction
bool requireValidAxis(KernelContext& ctx, const gp_Ax1& axis, const char* paramName);
bool requireValidAxis2(KernelContext& ctx, const gp_Ax2& axis, const char* paramName);

// ─── Geometric relationship validation ──────────────────────
// VA-6: validate relationships between collections of geometric primitives.

// All points must lie on a single plane within tol. A plane is fit from the
// first three non-collinear points; the remainder must have plane distance < tol.
// Requires at least 3 points.
bool requireCoplanar(KernelContext& ctx, const std::vector<gp_Pnt>& pts,
                     double tol, const char* paramName);

// |a . b| must be < tol (i.e. a and b are perpendicular).
bool requirePerpendicular(KernelContext& ctx, const gp_Dir& a, const gp_Dir& b,
                          double tol, const char* paramName);

// All points must be collinear. Direction is formed from the first two
// non-coincident points; remaining points must have cross-product magnitude < tol.
// Requires at least 2 points.
bool requireCollinear(KernelContext& ctx, const std::vector<gp_Pnt>& pts,
                      double tol, const char* paramName);

// ─── Unit enum validation ───────────────────────────────────

// Validate that a LengthUnit value is a known enum member.
// Returns false and reports error for unknown values.
bool requireValidLengthUnit(KernelContext& ctx, LengthUnit unit, const char* paramName);
bool requireValidAngleUnit(KernelContext& ctx, AngleUnit unit, const char* paramName);

// ─── Collection validation ───────────────────────────────────

// VA-3: header-only so any container type T works without explicit instantiation.
template<typename Container>
bool requireNonEmpty(KernelContext& ctx, const Container& collection, const char* paramName) {
    if (collection.empty()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '")
                       + (paramName ? paramName : "<unnamed>")
                       + "' must not be empty");
        return false;
    }
    return true;
}

// ─── String validation ───────────────────────────────────────

// String must not be empty
bool requireNonEmptyString(KernelContext& ctx, const std::string& str, const char* paramName);

} // namespace validation
} // namespace oreo

#endif // OREO_VALIDATION_H
