// validation.cpp — Fail-closed input validation.
//
// Every check rejects NaN, Inf, and invalid values.
// No silent defaults. No no-ops. If the input is bad, it's rejected.

#include "validation.h"
#include "units.h"
#include "kernel_context.h"
#include "diagnostic.h"
#include "oreo_error.h"
#include "naming/named_shape.h"

#include <Precision.hxx>
#include <TopAbs.hxx>

#include <cmath>
#include <limits>

namespace oreo {
namespace validation {

// ── Internal: reject NaN and Inf ─────────────────────────────

static bool isFinite(double value) {
    return std::isfinite(value);
}

static bool rejectNonFinite(KernelContext& ctx, double value, const char* paramName) {
    if (!isFinite(value)) {
        ctx.diag.error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + paramName
                       + "' is NaN or Inf (value: " + std::to_string(value) + ")");
        return false;  // value is bad
    }
    return true;  // value is finite
}

// ── Shape validation ─────────────────────────────────────────

bool requireNonNull(KernelContext& ctx, const NamedShape& shape, const char* paramName) {
    if (shape.isNull()) {
        ctx.diag.error(ErrorCode::INVALID_INPUT,
                       std::string("Null shape for parameter '") + paramName + "'");
        return false;
    }
    return true;
}

bool requireShapeType(KernelContext& ctx, const TopoDS_Shape& shape,
                      TopAbs_ShapeEnum expected, const char* paramName) {
    if (shape.IsNull()) {
        ctx.diag.error(ErrorCode::INVALID_INPUT,
                       std::string("Null shape for parameter '") + paramName + "'");
        return false;
    }
    if (shape.ShapeType() != expected) {
        ctx.diag.error(ErrorCode::INVALID_INPUT,
                       std::string("Wrong shape type for '") + paramName
                       + "': expected " + std::to_string(expected)
                       + " but got " + std::to_string(shape.ShapeType()));
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

// ── Numeric validation (all reject NaN/Inf) ──────────────────

bool requirePositive(KernelContext& ctx, double value, const char* paramName) {
    if (!rejectNonFinite(ctx, value, paramName)) return false;
    if (value <= 0.0) {
        ctx.diag.error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + paramName + "' must be positive, got "
                       + std::to_string(value));
        return false;
    }
    return true;
}

bool requireNonNegative(KernelContext& ctx, double value, const char* paramName) {
    if (!rejectNonFinite(ctx, value, paramName)) return false;
    if (value < 0.0) {
        ctx.diag.error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + paramName + "' must be non-negative, got "
                       + std::to_string(value));
        return false;
    }
    return true;
}

bool requireInRange(KernelContext& ctx, double value, double min, double max, const char* paramName) {
    if (!rejectNonFinite(ctx, value, paramName)) return false;
    if (value < min || value > max) {
        ctx.diag.error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + paramName + "' = " + std::to_string(value)
                       + " is out of range [" + std::to_string(min) + ", " + std::to_string(max) + "]");
        return false;
    }
    return true;
}

bool requireMinInt(KernelContext& ctx, int value, int minVal, const char* paramName) {
    if (value < minVal) {
        ctx.diag.error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + paramName + "' = " + std::to_string(value)
                       + " must be >= " + std::to_string(minVal));
        return false;
    }
    return true;
}

// ── Vector/direction validation ──────────────────────────────

bool requireNonZeroVec(KernelContext& ctx, const gp_Vec& vec, const char* paramName) {
    // Check all components for NaN/Inf
    if (!isFinite(vec.X()) || !isFinite(vec.Y()) || !isFinite(vec.Z())) {
        ctx.diag.error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + paramName + "' contains NaN or Inf components");
        return false;
    }
    if (vec.Magnitude() < Precision::Confusion()) {
        ctx.diag.error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + paramName + "' has zero magnitude");
        return false;
    }
    return true;
}

bool requireValidDir(KernelContext& ctx, const gp_Dir& dir, const char* paramName) {
    // gp_Dir normalizes on construction, but check for degenerate/NaN components
    if (!isFinite(dir.X()) || !isFinite(dir.Y()) || !isFinite(dir.Z())) {
        ctx.diag.error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + paramName + "' direction contains NaN or Inf");
        return false;
    }
    // Check magnitude (should be ~1.0 after normalization)
    double mag = std::sqrt(dir.X()*dir.X() + dir.Y()*dir.Y() + dir.Z()*dir.Z());
    if (mag < 0.5 || mag > 1.5) {
        ctx.diag.error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + paramName + "' direction is degenerate (magnitude: "
                       + std::to_string(mag) + ")");
        return false;
    }
    return true;
}

// ── Point validation ─────────────────────────────────────────

bool requireFinitePoint(KernelContext& ctx, const gp_Pnt& point, const char* paramName) {
    if (!isFinite(point.X()) || !isFinite(point.Y()) || !isFinite(point.Z())) {
        ctx.diag.error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + paramName
                       + "' point contains NaN or Inf coordinates");
        return false;
    }
    return true;
}

// ── Axis validation ──────────────────────────────────────────

bool requireValidAxis(KernelContext& ctx, const gp_Ax1& axis, const char* paramName) {
    std::string locName = std::string(paramName) + ".location";
    std::string dirName = std::string(paramName) + ".direction";
    if (!requireFinitePoint(ctx, axis.Location(), locName.c_str())) return false;
    if (!requireValidDir(ctx, axis.Direction(), dirName.c_str())) return false;
    return true;
}

bool requireValidAxis2(KernelContext& ctx, const gp_Ax2& axis, const char* paramName) {
    std::string locName = std::string(paramName) + ".location";
    std::string dirName = std::string(paramName) + ".direction";
    std::string xDirName = std::string(paramName) + ".xDirection";
    if (!requireFinitePoint(ctx, axis.Location(), locName.c_str())) return false;
    if (!requireValidDir(ctx, axis.Direction(), dirName.c_str())) return false;
    if (!requireValidDir(ctx, axis.XDirection(), xDirName.c_str())) return false;
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
    ctx.diag.error(ErrorCode::INVALID_INPUT,
                   std::string("Parameter '") + paramName
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
    ctx.diag.error(ErrorCode::INVALID_INPUT,
                   std::string("Parameter '") + paramName
                   + "' has unknown AngleUnit value ("
                   + std::to_string(static_cast<int>(unit)) + ")");
    return false;
}

// ── String validation ────────────────────────────────────────

bool requireNonEmptyString(KernelContext& ctx, const std::string& str, const char* paramName) {
    if (str.empty()) {
        ctx.diag.error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + paramName + "' must not be empty");
        return false;
    }
    return true;
}

// ── Collection validation ────────────────────────────────────

template<typename Container>
bool requireNonEmpty(KernelContext& ctx, const Container& collection, const char* paramName) {
    if (collection.empty()) {
        ctx.diag.error(ErrorCode::INVALID_INPUT,
                       std::string("Parameter '") + paramName + "' must not be empty");
        return false;
    }
    return true;
}

template bool requireNonEmpty(KernelContext& ctx, const std::vector<NamedEdge>& collection, const char* paramName);
template bool requireNonEmpty(KernelContext& ctx, const std::vector<NamedFace>& collection, const char* paramName);
template bool requireNonEmpty(KernelContext& ctx, const std::vector<NamedShape>& collection, const char* paramName);

} // namespace validation
} // namespace oreo
