// SPDX-License-Identifier: LGPL-2.1-or-later

// validation.cpp — Fail-closed input validation.
//
// Every check rejects NaN, Inf, and invalid values.
// No silent defaults. No no-ops. If the input is bad, it's rejected.

#include "validation.h"
#include "units.h"
#include "kernel_context.h"
#include "diagnostic.h"
#include "diagnostic.h"
#include "naming/named_shape.h"

#include <Precision.hxx>
#include <TopAbs.hxx>
#include <BRepCheck_Analyzer.hxx>

#include <cmath>
#include <limits>

namespace oreo {
namespace validation {

// ── Internal: reject NaN and Inf ─────────────────────────────

static bool isFinite(double value) {
    return std::isfinite(value);
}

// VA-4: Guard against nullptr paramName so string concatenation never UBs.
static const char* safeName(const char* p) { return p ? p : "<unnamed>"; }

// VA-1: human-readable TopAbs_ShapeEnum labels.
static const char* shapeTypeName(TopAbs_ShapeEnum t) {
    switch (t) {
        case TopAbs_COMPOUND:  return "COMPOUND";
        case TopAbs_COMPSOLID: return "COMPSOLID";
        case TopAbs_SOLID:     return "SOLID";
        case TopAbs_SHELL:     return "SHELL";
        case TopAbs_FACE:      return "FACE";
        case TopAbs_WIRE:      return "WIRE";
        case TopAbs_EDGE:      return "EDGE";
        case TopAbs_VERTEX:    return "VERTEX";
        case TopAbs_SHAPE:     return "SHAPE";
        default:               return "UNKNOWN";
    }
}

static bool rejectNonFinite(KernelContext& ctx, double value, const char* paramName) {
    if (!isFinite(value)) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + safeName(paramName)
                       + "' is NaN or Inf (value: " + std::to_string(value) + ")");
        return false;  // value is bad
    }
    return true;  // value is finite
}

// ── Silent checks (VA-7) ─────────────────────────────────────

bool checkNonNull(const NamedShape& shape) {
    return !shape.isNull();
}

bool checkShapeType(const TopoDS_Shape& shape, TopAbs_ShapeEnum expected) {
    if (shape.IsNull()) return false;
    return shape.ShapeType() == expected;
}

// ── Shape validation ─────────────────────────────────────────

bool requireNonNull(KernelContext& ctx, const NamedShape& shape, const char* paramName) {
    if (checkNonNull(shape)) return true;
    ctx.diag().error(ErrorCode::INVALID_INPUT,
                   std::string("Null shape for parameter '") + safeName(paramName) + "'");
    return false;
}

bool requireShapeType(KernelContext& ctx, const TopoDS_Shape& shape,
                      TopAbs_ShapeEnum expected, const char* paramName) {
    if (shape.IsNull()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                       std::string("Null shape for parameter '") + safeName(paramName) + "'");
        return false;
    }
    if (shape.ShapeType() != expected) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                       std::string("Wrong shape type for '") + safeName(paramName)
                       + "': expected " + shapeTypeName(expected)
                       + " but got " + shapeTypeName(shape.ShapeType()));
        return false;
    }
    return true;
}

bool requireValidEdge(KernelContext& ctx, const TopoDS_Shape& shape, const char* paramName) {
    return requireShapeType(ctx, shape, TopAbs_EDGE, paramName);
}

bool requireValidFace(KernelContext& ctx, const TopoDS_Shape& shape, const char* paramName) {
    return requireShapeType(ctx, shape, TopAbs_FACE, paramName);
}

bool requireValidWire(KernelContext& ctx, const TopoDS_Shape& shape, const char* paramName) {
    return requireShapeType(ctx, shape, TopAbs_WIRE, paramName);
}

// VA-9: BRepCheck-backed validity. Layered on top of the type checks so
// callers can pick either the cheap type-only variant or the deep one.
static bool requireValidGeometry_(KernelContext& ctx, const TopoDS_Shape& shape,
                                  TopAbs_ShapeEnum expected, const char* paramName) {
    if (!requireShapeType(ctx, shape, expected, paramName)) return false;
    BRepCheck_Analyzer analyzer(shape, Standard_True);
    if (!analyzer.IsValid()) {
        ctx.diag().error(ErrorCode::SHAPE_INVALID,
                       std::string("Parameter '") + safeName(paramName)
                       + "' (" + shapeTypeName(expected)
                       + ") failed BRepCheck_Analyzer validity");
        return false;
    }
    return true;
}

bool requireValidEdgeGeometry(KernelContext& ctx, const TopoDS_Shape& shape, const char* paramName) {
    return requireValidGeometry_(ctx, shape, TopAbs_EDGE, paramName);
}

bool requireValidFaceGeometry(KernelContext& ctx, const TopoDS_Shape& shape, const char* paramName) {
    return requireValidGeometry_(ctx, shape, TopAbs_FACE, paramName);
}

bool requireValidWireGeometry(KernelContext& ctx, const TopoDS_Shape& shape, const char* paramName) {
    return requireValidGeometry_(ctx, shape, TopAbs_WIRE, paramName);
}

// ── Numeric validation (all reject NaN/Inf) ──────────────────

bool requirePositive(KernelContext& ctx, double value, const char* paramName) {
    if (!rejectNonFinite(ctx, value, paramName)) return false;
    if (value <= 0.0) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + safeName(paramName) + "' must be positive, got "
                       + std::to_string(value));
        return false;
    }
    return true;
}

bool requireNonNegative(KernelContext& ctx, double value, const char* paramName) {
    if (!rejectNonFinite(ctx, value, paramName)) return false;
    if (value < 0.0) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + safeName(paramName) + "' must be non-negative, got "
                       + std::to_string(value));
        return false;
    }
    return true;
}

bool requireInRange(KernelContext& ctx, double value, double min, double max, const char* paramName) {
    if (!rejectNonFinite(ctx, value, paramName)) return false;
    if (!std::isfinite(min) || !std::isfinite(max)) {
        ctx.diag().error(ErrorCode::INTERNAL_ERROR,
                       std::string("Parameter '") + safeName(paramName) + "' range bounds are NaN or Inf");
        return false;
    }
    if (value < min || value > max) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + safeName(paramName) + "' = " + std::to_string(value)
                       + " is out of range [" + std::to_string(min) + ", " + std::to_string(max) + "]");
        return false;
    }
    return true;
}

bool requireMinInt(KernelContext& ctx, int value, int minVal, const char* paramName) {
    if (value < minVal) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + safeName(paramName) + "' = " + std::to_string(value)
                       + " must be >= " + std::to_string(minVal));
        return false;
    }
    return true;
}

// VA-5: tolerance-specific validator. Rejects non-finite, non-positive, and
// values outside the caller-supplied envelope.
bool requireTolerance(KernelContext& ctx, double tol, const char* paramName,
                      double minAllowed, double maxAllowed) {
    if (!rejectNonFinite(ctx, tol, paramName)) return false;
    if (!std::isfinite(minAllowed) || !std::isfinite(maxAllowed) || minAllowed > maxAllowed) {
        ctx.diag().error(ErrorCode::INTERNAL_ERROR,
                       std::string("Parameter '") + safeName(paramName)
                       + "' tolerance envelope is ill-formed (min="
                       + std::to_string(minAllowed) + ", max="
                       + std::to_string(maxAllowed) + ")");
        return false;
    }
    if (tol <= 0.0) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + safeName(paramName)
                       + "' tolerance must be positive, got " + std::to_string(tol));
        return false;
    }
    if (tol < minAllowed || tol > maxAllowed) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + safeName(paramName)
                       + "' tolerance " + std::to_string(tol)
                       + " is outside allowed range [" + std::to_string(minAllowed)
                       + ", " + std::to_string(maxAllowed) + "]");
        return false;
    }
    return true;
}

// VA-10: every element finite and > 0; report index of the first violator.
bool requireAllPositive(KernelContext& ctx, const double* values, std::size_t n,
                        const char* paramName) {
    if (n > 0 && values == nullptr) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + safeName(paramName)
                       + "' is null but n=" + std::to_string(n));
        return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
        double v = values[i];
        if (!std::isfinite(v)) {
            ctx.diag().error(ErrorCode::INVALID_INPUT,
                           std::string("Parameter '") + safeName(paramName)
                           + "[" + std::to_string(i) + "]' is NaN or Inf (value: "
                           + std::to_string(v) + ")");
            return false;
        }
        if (v <= 0.0) {
            ctx.diag().error(ErrorCode::INVALID_INPUT,
                           std::string("Parameter '") + safeName(paramName)
                           + "[" + std::to_string(i) + "]' must be positive, got "
                           + std::to_string(v));
            return false;
        }
    }
    return true;
}

// ── Vector/direction validation ──────────────────────────────

bool requireNonZeroVec(KernelContext& ctx, const gp_Vec& vec, const char* paramName) {
    // Check all components for NaN/Inf
    if (!isFinite(vec.X()) || !isFinite(vec.Y()) || !isFinite(vec.Z())) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + safeName(paramName) + "' contains NaN or Inf components");
        return false;
    }
    if (vec.Magnitude() < Precision::Confusion()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + safeName(paramName) + "' has zero magnitude");
        return false;
    }
    return true;
}

bool requireValidDir(KernelContext& ctx, const gp_Dir& dir, const char* paramName) {
    // gp_Dir normalizes on construction, but check for degenerate/NaN components.
    if (!isFinite(dir.X()) || !isFinite(dir.Y()) || !isFinite(dir.Z())) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + safeName(paramName) + "' direction contains NaN or Inf");
        return false;
    }
    // VA-8: gp_Dir must be normalized; looser checks mask upstream bugs.
    double mag = std::sqrt(dir.X()*dir.X() + dir.Y()*dir.Y() + dir.Z()*dir.Z());
    if (mag < 1.0 - 1e-6 || mag > 1.0 + 1e-6) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + safeName(paramName)
                       + "' direction is not unit-length (magnitude: "
                       + std::to_string(mag) + ")");
        return false;
    }
    return true;
}

// ── Point validation ─────────────────────────────────────────

bool requireFinitePoint(KernelContext& ctx, const gp_Pnt& point, const char* paramName) {
    if (!isFinite(point.X()) || !isFinite(point.Y()) || !isFinite(point.Z())) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + safeName(paramName)
                       + "' point contains NaN or Inf coordinates");
        return false;
    }
    return true;
}

// ── Axis validation ──────────────────────────────────────────

bool requireValidAxis(KernelContext& ctx, const gp_Ax1& axis, const char* paramName) {
    std::string base = safeName(paramName);
    std::string locName = base + ".location";
    std::string dirName = base + ".direction";
    if (!requireFinitePoint(ctx, axis.Location(), locName.c_str())) return false;
    if (!requireValidDir(ctx, axis.Direction(), dirName.c_str())) return false;
    return true;
}

bool requireValidAxis2(KernelContext& ctx, const gp_Ax2& ax2, const char* paramName) {
    std::string base = safeName(paramName);
    std::string locName = base + ".location";
    std::string dirName = base + ".direction";
    std::string xDirName = base + ".xDirection";
    if (!requireFinitePoint(ctx, ax2.Location(), locName.c_str())) return false;
    if (!requireValidDir(ctx, ax2.Direction(), dirName.c_str())) return false;
    if (!requireValidDir(ctx, ax2.XDirection(), xDirName.c_str())) return false;

    // VA-2: verify perpendicularity of Direction and XDirection.
    double dot = ax2.Direction().Dot(ax2.XDirection());
    if (std::abs(dot) > 1e-6) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                       base + ": Direction and XDirection not perpendicular (dot="
                       + std::to_string(dot) + ")");
        return false;
    }
    return true;
}

// ── Geometric relationship validation (VA-6) ─────────────────

bool requirePerpendicular(KernelContext& ctx, const gp_Dir& a, const gp_Dir& b,
                          double tol, const char* paramName) {
    if (!requireValidDir(ctx, a, paramName)) return false;
    if (!requireValidDir(ctx, b, paramName)) return false;
    if (!rejectNonFinite(ctx, tol, paramName)) return false;
    if (tol <= 0.0) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + safeName(paramName)
                       + "' tolerance must be positive, got " + std::to_string(tol));
        return false;
    }
    double dot = a.Dot(b);
    if (std::abs(dot) >= tol) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + safeName(paramName)
                       + "' directions are not perpendicular (|dot|="
                       + std::to_string(std::abs(dot)) + ", tol="
                       + std::to_string(tol) + ")");
        return false;
    }
    return true;
}

bool requireCollinear(KernelContext& ctx, const std::vector<gp_Pnt>& pts,
                      double tol, const char* paramName) {
    if (pts.size() < 2) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + safeName(paramName)
                       + "' requires at least 2 points, got " + std::to_string(pts.size()));
        return false;
    }
    if (!rejectNonFinite(ctx, tol, paramName)) return false;
    if (tol <= 0.0) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + safeName(paramName)
                       + "' tolerance must be positive, got " + std::to_string(tol));
        return false;
    }
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const gp_Pnt& p = pts[i];
        if (!isFinite(p.X()) || !isFinite(p.Y()) || !isFinite(p.Z())) {
            ctx.diag().error(ErrorCode::INVALID_INPUT,
                           std::string("Parameter '") + safeName(paramName)
                           + "[" + std::to_string(i) + "]' has NaN or Inf coordinates");
            return false;
        }
    }

    // Find the first pair of non-coincident points to define the line.
    const gp_Pnt& p0 = pts[0];
    std::size_t anchorIdx = 1;
    gp_Vec dirVec(0, 0, 0);
    double dirMag = 0.0;
    for (; anchorIdx < pts.size(); ++anchorIdx) {
        dirVec = gp_Vec(p0, pts[anchorIdx]);
        dirMag = dirVec.Magnitude();
        if (dirMag > tol) break;
    }
    if (anchorIdx >= pts.size()) {
        // All points coincide with p0 within tol — trivially collinear.
        return true;
    }

    // Remaining points: the cross product (p_i - p0) x dirVec must have
    // magnitude below tol * |dirVec| to lie on the line.
    for (std::size_t i = anchorIdx + 1; i < pts.size(); ++i) {
        gp_Vec v(p0, pts[i]);
        gp_Vec cross = v.Crossed(dirVec);
        double crossMag = cross.Magnitude();
        if (crossMag > tol * dirMag) {
            ctx.diag().error(ErrorCode::INVALID_INPUT,
                           std::string("Parameter '") + safeName(paramName)
                           + "[" + std::to_string(i) + "]' is not collinear (perpendicular distance="
                           + std::to_string(crossMag / dirMag) + ", tol="
                           + std::to_string(tol) + ")");
            return false;
        }
    }
    return true;
}

bool requireCoplanar(KernelContext& ctx, const std::vector<gp_Pnt>& pts,
                     double tol, const char* paramName) {
    if (pts.size() < 3) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + safeName(paramName)
                       + "' requires at least 3 points, got " + std::to_string(pts.size()));
        return false;
    }
    if (!rejectNonFinite(ctx, tol, paramName)) return false;
    if (tol <= 0.0) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + safeName(paramName)
                       + "' tolerance must be positive, got " + std::to_string(tol));
        return false;
    }
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const gp_Pnt& p = pts[i];
        if (!isFinite(p.X()) || !isFinite(p.Y()) || !isFinite(p.Z())) {
            ctx.diag().error(ErrorCode::INVALID_INPUT,
                           std::string("Parameter '") + safeName(paramName)
                           + "[" + std::to_string(i) + "]' has NaN or Inf coordinates");
            return false;
        }
    }

    // Fit a plane via the first three non-collinear points: normal = v1 x v2.
    const gp_Pnt& p0 = pts[0];
    gp_Vec v1(p0, pts[1]);
    gp_Vec normal(0, 0, 0);
    double normalMag = 0.0;
    std::size_t thirdIdx = 2;
    for (; thirdIdx < pts.size(); ++thirdIdx) {
        gp_Vec v2(p0, pts[thirdIdx]);
        normal = v1.Crossed(v2);
        normalMag = normal.Magnitude();
        if (normalMag > tol) break;
    }
    if (thirdIdx >= pts.size()) {
        // Could not find three non-collinear points — all points lie on a
        // single line, which is trivially coplanar.
        return true;
    }

    // For every remaining point, perpendicular distance to the plane is
    // |(p_i - p0) . normal| / |normal|. Must be < tol.
    for (std::size_t i = thirdIdx + 1; i < pts.size(); ++i) {
        gp_Vec v(p0, pts[i]);
        double dist = std::abs(v.Dot(normal)) / normalMag;
        if (dist > tol) {
            ctx.diag().error(ErrorCode::INVALID_INPUT,
                           std::string("Parameter '") + safeName(paramName)
                           + "[" + std::to_string(i) + "]' is not coplanar (distance="
                           + std::to_string(dist) + ", tol="
                           + std::to_string(tol) + ")");
            return false;
        }
    }
    return true;
}

// ── Unit enum validation ─────────────────────────────────────

bool requireValidLengthUnit(KernelContext& ctx, LengthUnit unit, const char* paramName) {
    switch (unit) {
        case LengthUnit::Meter:
        case LengthUnit::Millimeter:
        case LengthUnit::Centimeter:
        case LengthUnit::Micrometer:
        case LengthUnit::Inch:
        case LengthUnit::Foot:
            return true;
    }
    ctx.diag().error(ErrorCode::INVALID_INPUT,
                   std::string("Parameter '") + safeName(paramName)
                   + "' has unknown LengthUnit value ("
                   + std::to_string(static_cast<int>(unit)) + ")");
    return false;
}

bool requireValidAngleUnit(KernelContext& ctx, AngleUnit unit, const char* paramName) {
    switch (unit) {
        case AngleUnit::Radian:
        case AngleUnit::Degree:
            return true;
    }
    ctx.diag().error(ErrorCode::INVALID_INPUT,
                   std::string("Parameter '") + safeName(paramName)
                   + "' has unknown AngleUnit value ("
                   + std::to_string(static_cast<int>(unit)) + ")");
    return false;
}

// ── String validation ────────────────────────────────────────

bool requireNonEmptyString(KernelContext& ctx, const std::string& str, const char* paramName) {
    if (str.empty()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + safeName(paramName) + "' must not be empty");
        return false;
    }
    return true;
}

// VA-3: requireNonEmpty<> is header-only. No explicit instantiations here —
// any container type is instantiated on demand at the call site.

} // namespace validation
} // namespace oreo
