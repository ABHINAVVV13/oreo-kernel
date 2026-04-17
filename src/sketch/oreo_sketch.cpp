// SPDX-License-Identifier: LGPL-2.1-or-later

// oreo_sketch.cpp — Production-grade sketch solver API.
//
// Wraps PlaneGCS for constraint solving with:
//   - Proper parameter registration (no dummy constraints)
//   - All major constraint types supported
//   - Memory-safe parameter management (no leaks)
//   - Comprehensive solve status reporting
//   - Wire conversion for OCCT geometry pipeline

#include "oreo_sketch.h"
#include "GCS.h"
#include "core/oreo_error.h"
#include "core/operation_result.h"
#include "core/diagnostic_scope.h"
#include "naming/named_shape.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <gp_Circ.hxx>
#include <gp_Pnt.hxx>
#include <TopoDS_Wire.hxx>

#include <algorithm>
#include <cmath>
#include <memory>
#include <sstream>
#include <vector>

namespace oreo {

namespace {

// Parameter pool — owns all dynamically allocated doubles for constraint values.
// Destroyed when the solve operation completes, preventing memory leaks.
class ParamPool {
public:
    double* alloc(double value) {
        pool_.push_back(std::make_unique<double>(value));
        return pool_.back().get();
    }
private:
    std::vector<std::unique_ptr<double>> pool_;
};

} // anonymous namespace

// ── Constraint schema source of truth ────────────────────────
//
// Defined here (not as a lambda) so the C-API slot layer in
// src/capi/oreo_capi.cpp can reuse the same family rules when
// translating public stable entity IDs to live solver indices.
ConstraintSchemaEntry constraintSchemaFor(ConstraintType t) {
    using F = ConstraintEntityFamily;
    switch (t) {
        // Point–Point
        case ConstraintType::Coincident:    return {F::Point, F::Point, F::None, false, false};
        case ConstraintType::Distance:      return {F::Point, F::Point, F::None, true,  false};
        case ConstraintType::DistanceX:     return {F::Point, F::Point, F::None, true,  false};
        case ConstraintType::DistanceY:     return {F::Point, F::Point, F::None, true,  false};
        case ConstraintType::Symmetric:     return {F::Point, F::Point, F::Line, false, false};
        // Point–Curve / point on entity
        case ConstraintType::PointOnLine:   return {F::Point, F::Line,   F::None, false, false};
        case ConstraintType::PointOnCircle: return {F::Point, F::Circle, F::None, false, false};
        case ConstraintType::PointOnArc:    return {F::Point, F::Arc,    F::None, false, false};
        // MidpointOnLine: midpoint of line[entity1] lies on line[entity2].
        case ConstraintType::MidpointOnLine:return {F::Line,  F::Line,   F::None, false, false};
        case ConstraintType::PointToLineDistance:   return {F::Point, F::Line,   F::None, true, false};
        case ConstraintType::PointToCircleDistance: return {F::Point, F::Circle, F::None, true, false};
        case ConstraintType::CircleToLineDistance:  return {F::Circle, F::Line,  F::None, true, false};
        // Line/curve geometry
        case ConstraintType::Parallel:      return {F::Line, F::Line, F::None, false, false};
        case ConstraintType::Perpendicular: return {F::Line, F::Line, F::None, false, false};
        case ConstraintType::Angle:         return {F::Line, F::Line, F::None, true,  false};
        case ConstraintType::Equal:         return {F::Line, F::Line, F::None, false, false};
        case ConstraintType::Collinear:     return {F::Line, F::Line, F::None, false, false};
        case ConstraintType::Horizontal:    return {F::Line, F::None, F::None, false, false};
        case ConstraintType::Vertical:      return {F::Line, F::None, F::None, false, false};
        // Tangencies
        case ConstraintType::Tangent:               return {F::AnyCurve, F::AnyCurve, F::None, false, false};
        case ConstraintType::TangentLineArc:        return {F::Line,        F::Arc,         F::None, false, false};
        case ConstraintType::TangentArcArc:         return {F::Arc,         F::Arc,         F::None, false, false};
        case ConstraintType::TangentCircleCircle:   return {F::Circle,      F::Circle,      F::None, false, false};
        case ConstraintType::TangentLineCircle:     return {F::Line,        F::Circle,      F::None, false, false};
        case ConstraintType::TangentCircleArc:      return {F::Circle,      F::Arc,         F::None, false, false};
        // Radii / diameters
        case ConstraintType::Radius:        return {F::CircleOrArc, F::None, F::None, true, true};
        case ConstraintType::CircleDiameter:return {F::CircleOrArc, F::None, F::None, true, true};
        case ConstraintType::EqualRadius:   return {F::Circle, F::Circle, F::None, false, false};
        case ConstraintType::EqualRadiusCircleArc: return {F::Circle, F::Arc, F::None, false, false};
        case ConstraintType::ArcLength:     return {F::Arc, F::None, F::None, true, true};
        // Coordinate / fixed
        case ConstraintType::Fixed:         return {F::Point, F::None, F::None, false, false};
        case ConstraintType::CoordinateX:   return {F::Point, F::None, F::None, true, false};
        case ConstraintType::CoordinateY:   return {F::Point, F::None, F::None, true, false};
        case ConstraintType::Concentric:    return {F::Circle, F::Circle, F::None, false, false};
    }
    return {F::None, F::None, F::None, false, false};
}

OperationResult<SolveResult> solveSketch(
    KernelContext& ctx,
    std::vector<SketchPoint>& points,
    std::vector<SketchLine>& lines,
    std::vector<SketchCircle>& circles,
    std::vector<SketchArc>& arcs,
    const std::vector<SketchConstraint>& constraints)
{
    DiagnosticScope scope(ctx);
    SolveResult result;
    result.status = SolveStatus::OK;
    result.degreesOfFreedom = 0;

    // ── Pre-solve sanity sweep ───────────────────────────────
    //
    // PlaneGCS expects finite seed values; feeding it NaN/Inf either
    // hangs the solver or returns silent garbage. Reject hostile
    // inputs at the boundary with a structured diagnostic.
    auto isFin = [](double v) { return std::isfinite(v); };
    auto reportBad = [&](const char* what, std::size_t idx, const char* field) {
        std::ostringstream oss;
        oss << what << "[" << idx << "]." << field
            << " is NaN/Inf — sketch cannot be solved";
        ctx.diag().error(ErrorCode::INVALID_INPUT, oss.str());
    };
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (!isFin(points[i].x)) { reportBad("point", i, "x"); return scope.makeFailure<SolveResult>(); }
        if (!isFin(points[i].y)) { reportBad("point", i, "y"); return scope.makeFailure<SolveResult>(); }
    }
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (!isFin(lines[i].p1.x) || !isFin(lines[i].p1.y) ||
            !isFin(lines[i].p2.x) || !isFin(lines[i].p2.y)) {
            reportBad("line", i, "endpoint"); return scope.makeFailure<SolveResult>();
        }
    }
    for (std::size_t i = 0; i < circles.size(); ++i) {
        if (!isFin(circles[i].center.x) || !isFin(circles[i].center.y) ||
            !isFin(circles[i].radius)) {
            reportBad("circle", i, "center/radius"); return scope.makeFailure<SolveResult>();
        }
        if (circles[i].radius <= 0.0) {
            std::ostringstream oss;
            oss << "circle[" << i << "].radius must be positive (got " << circles[i].radius << ")";
            ctx.diag().error(ErrorCode::OUT_OF_RANGE, oss.str());
            return scope.makeFailure<SolveResult>();
        }
    }
    for (std::size_t i = 0; i < arcs.size(); ++i) {
        const auto& a = arcs[i];
        if (!isFin(a.center.x) || !isFin(a.center.y) ||
            !isFin(a.start.x)  || !isFin(a.start.y) ||
            !isFin(a.end.x)    || !isFin(a.end.y)   ||
            !isFin(a.radius)) {
            reportBad("arc", i, "geometry"); return scope.makeFailure<SolveResult>();
        }
        if (a.radius <= 0.0) {
            std::ostringstream oss;
            oss << "arc[" << i << "].radius must be positive (got " << a.radius << ")";
            ctx.diag().error(ErrorCode::OUT_OF_RANGE, oss.str());
            return scope.makeFailure<SolveResult>();
        }
    }

    // ── Constraint schema validation ────────────────────────
    //
    // Each constraint type knows up-front which entity family
    // (Point / Line / Circle / Arc) each entity slot must reference.
    // Validating up-front means a typo'd index surfaces as a
    // structured diagnostic instead of being silently dropped by the
    // dispatch switch below.
    using F = ConstraintEntityFamily;
    const int nP = static_cast<int>(points.size());
    const int nL = static_cast<int>(lines.size());
    const int nC = static_cast<int>(circles.size());
    const int nA = static_cast<int>(arcs.size());
    auto familyOk = [&](F f, int idx) {
        switch (f) {
            case F::None:        return idx == -1;
            case F::Point:       return idx >= 0 && idx < nP;
            case F::Line:        return idx >= 0 && idx < nL;
            case F::Circle:      return idx >= 0 && idx < nC;
            case F::Arc:         return idx >= 0 && idx < nA;
            case F::AnyCurve:    return idx >= 0 && (idx < nL || idx < nC || idx < nA);
            case F::CircleOrArc: return idx >= 0 && (idx < nC || idx < nA);
        }
        return false;
    };
    auto familyName = [](F f) -> const char* {
        switch (f) {
            case F::None:        return "<none>";
            case F::Point:       return "Point";
            case F::Line:        return "Line";
            case F::Circle:      return "Circle";
            case F::Arc:         return "Arc";
            case F::AnyCurve:    return "Line/Circle/Arc";
            case F::CircleOrArc: return "Circle/Arc";
        }
        return "?";
    };
    for (std::size_t ci = 0; ci < constraints.size(); ++ci) {
        const auto& c = constraints[ci];
        if (!isFin(c.value)) {
            std::ostringstream oss;
            oss << "constraint[" << ci << "].value " << c.value
                << " is NaN/Inf — refusing to solve";
            ctx.diag().error(ErrorCode::INVALID_INPUT, oss.str());
            return scope.makeFailure<SolveResult>();
        }
        const ConstraintSchemaEntry sch = constraintSchemaFor(c.type);
        if (sch.valueUsed && sch.valueMustBePositive && c.value <= 0.0) {
            std::ostringstream oss;
            oss << "constraint[" << ci << "].value must be positive (got "
                << c.value << ")";
            ctx.diag().error(ErrorCode::OUT_OF_RANGE, oss.str());
            return scope.makeFailure<SolveResult>();
        }
        // Slot 1 is always required; slot 2/3 may be F::None.
        if (sch.e1 != F::None && !familyOk(sch.e1, c.entity1)) {
            std::ostringstream oss;
            oss << "constraint[" << ci << "].entity1=" << c.entity1
                << " does not reference a valid " << familyName(sch.e1);
            ctx.diag().error(ErrorCode::OUT_OF_RANGE, oss.str());
            return scope.makeFailure<SolveResult>();
        }
        if (sch.e2 != F::None && !familyOk(sch.e2, c.entity2)) {
            std::ostringstream oss;
            oss << "constraint[" << ci << "].entity2=" << c.entity2
                << " does not reference a valid " << familyName(sch.e2);
            ctx.diag().error(ErrorCode::OUT_OF_RANGE, oss.str());
            return scope.makeFailure<SolveResult>();
        }
        if (sch.e3 != F::None && !familyOk(sch.e3, c.entity3)) {
            std::ostringstream oss;
            oss << "constraint[" << ci << "].entity3=" << c.entity3
                << " does not reference a valid " << familyName(sch.e3);
            ctx.diag().error(ErrorCode::OUT_OF_RANGE, oss.str());
            return scope.makeFailure<SolveResult>();
        }
    }

    GCS::System system;
    ParamPool pool;

    // ── Build GCS geometry objects ───────────────────────────
    // These reference the user's data directly through pointers,
    // so solved values are written back automatically.

    std::vector<GCS::Point> gcsPoints;
    gcsPoints.reserve(points.size());
    for (auto& p : points) {
        GCS::Point gp;
        gp.x = &p.x;
        gp.y = &p.y;
        gcsPoints.push_back(gp);
    }

    std::vector<GCS::Line> gcsLines;
    gcsLines.reserve(lines.size());
    for (auto& l : lines) {
        GCS::Line gl;
        gl.p1.x = &l.p1.x; gl.p1.y = &l.p1.y;
        gl.p2.x = &l.p2.x; gl.p2.y = &l.p2.y;
        gcsLines.push_back(gl);
    }

    std::vector<GCS::Circle> gcsCircles;
    gcsCircles.reserve(circles.size());
    for (auto& c : circles) {
        GCS::Circle gc;
        gc.center.x = &c.center.x;
        gc.center.y = &c.center.y;
        gc.rad = &c.radius;
        gcsCircles.push_back(gc);
    }

    std::vector<GCS::Arc> gcsArcs;
    gcsArcs.reserve(arcs.size());
    for (auto& a : arcs) {
        GCS::Arc ga;
        ga.center.x = &a.center.x;
        ga.center.y = &a.center.y;
        ga.start.x = &a.start.x;
        ga.start.y = &a.start.y;
        ga.end.x = &a.end.x;
        ga.end.y = &a.end.y;
        ga.rad = &a.radius;
        // startAngle/endAngle will be computed by GCS from start/end points
        double startAngle = std::atan2(a.start.y - a.center.y, a.start.x - a.center.x);
        double endAngle = std::atan2(a.end.y - a.center.y, a.end.x - a.center.x);
        ga.startAngle = pool.alloc(startAngle);
        ga.endAngle = pool.alloc(endAngle);
        gcsArcs.push_back(ga);
    }

    // ── Add constraints ──────────────────────────────────────
    // Each constraint gets a unique tag (its index in the constraints array).
    // Value parameters are allocated from the pool (no leaks).

    for (size_t i = 0; i < constraints.size(); ++i) {
        const auto& c = constraints[i];
        int tag = static_cast<int>(i);

        switch (c.type) {

        // ── Point-Point constraints ──────────────────────────
        case ConstraintType::Coincident:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsPoints.size() &&
                c.entity2 >= 0 && c.entity2 < (int)gcsPoints.size()) {
                system.addConstraintP2PCoincident(
                    gcsPoints[c.entity1], gcsPoints[c.entity2], tag);
            }
            break;

        case ConstraintType::Distance:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsPoints.size() &&
                c.entity2 >= 0 && c.entity2 < (int)gcsPoints.size()) {
                system.addConstraintP2PDistance(
                    gcsPoints[c.entity1], gcsPoints[c.entity2],
                    pool.alloc(c.value), tag);
            }
            break;

        case ConstraintType::DistanceX:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsPoints.size() &&
                c.entity2 >= 0 && c.entity2 < (int)gcsPoints.size()) {
                system.addConstraintDifference(
                    gcsPoints[c.entity1].x, gcsPoints[c.entity2].x,
                    pool.alloc(c.value), tag);
            }
            break;

        case ConstraintType::DistanceY:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsPoints.size() &&
                c.entity2 >= 0 && c.entity2 < (int)gcsPoints.size()) {
                system.addConstraintDifference(
                    gcsPoints[c.entity1].y, gcsPoints[c.entity2].y,
                    pool.alloc(c.value), tag);
            }
            break;

        // ── Point-Line constraints ───────────────────────────
        case ConstraintType::PointOnLine:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsPoints.size() &&
                c.entity2 >= 0 && c.entity2 < (int)gcsLines.size()) {
                system.addConstraintPointOnLine(
                    gcsPoints[c.entity1], gcsLines[c.entity2], tag);
            }
            break;

        // ── Point-Circle constraints ─────────────────────────
        case ConstraintType::PointOnCircle:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsPoints.size() &&
                c.entity2 >= 0 && c.entity2 < (int)gcsCircles.size()) {
                system.addConstraintPointOnCircle(
                    gcsPoints[c.entity1], gcsCircles[c.entity2], tag);
            }
            break;

        // ── Line-Line constraints ────────────────────────────
        case ConstraintType::Horizontal:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsLines.size()) {
                system.addConstraintHorizontal(gcsLines[c.entity1], tag);
            }
            break;

        case ConstraintType::Vertical:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsLines.size()) {
                system.addConstraintVertical(gcsLines[c.entity1], tag);
            }
            break;

        case ConstraintType::Parallel:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsLines.size() &&
                c.entity2 >= 0 && c.entity2 < (int)gcsLines.size()) {
                system.addConstraintParallel(
                    gcsLines[c.entity1], gcsLines[c.entity2], tag);
            }
            break;

        case ConstraintType::Perpendicular:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsLines.size() &&
                c.entity2 >= 0 && c.entity2 < (int)gcsLines.size()) {
                system.addConstraintPerpendicular(
                    gcsLines[c.entity1], gcsLines[c.entity2], tag);
            }
            break;

        case ConstraintType::Angle:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsLines.size() &&
                c.entity2 >= 0 && c.entity2 < (int)gcsLines.size()) {
                system.addConstraintL2LAngle(
                    gcsLines[c.entity1], gcsLines[c.entity2],
                    pool.alloc(c.value), tag);
            }
            break;

        case ConstraintType::Equal:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsLines.size() &&
                c.entity2 >= 0 && c.entity2 < (int)gcsLines.size()) {
                system.addConstraintEqualLength(
                    gcsLines[c.entity1], gcsLines[c.entity2], tag);
            }
            break;

        // ── Circle/Arc constraints ───────────────────────────
        case ConstraintType::Radius:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsCircles.size()) {
                system.addConstraintCircleRadius(
                    gcsCircles[c.entity1], pool.alloc(c.value), tag);
            } else if (c.entity1 >= 0 && c.entity1 < (int)gcsArcs.size()) {
                system.addConstraintArcRadius(
                    gcsArcs[c.entity1], pool.alloc(c.value), tag);
            }
            break;

        case ConstraintType::Tangent:
            // Line tangent to circle
            if (c.entity1 >= 0 && c.entity1 < (int)gcsLines.size() &&
                c.entity2 >= 0 && c.entity2 < (int)gcsCircles.size()) {
                system.addConstraintTangent(
                    gcsLines[c.entity1], gcsCircles[c.entity2], tag);
            }
            break;

        // ── Symmetry ─────────────────────────────────────────
        case ConstraintType::Symmetric:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsPoints.size() &&
                c.entity2 >= 0 && c.entity2 < (int)gcsPoints.size() &&
                c.entity3 >= 0 && c.entity3 < (int)gcsLines.size()) {
                system.addConstraintP2PSymmetric(
                    gcsPoints[c.entity1], gcsPoints[c.entity2],
                    gcsLines[c.entity3], tag);
            }
            break;

        // ── Fixed point ──────────────────────────────────────
        case ConstraintType::Fixed:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsPoints.size()) {
                system.addConstraintCoordinateX(gcsPoints[c.entity1],
                    pool.alloc(gcsPoints[c.entity1].x ? *gcsPoints[c.entity1].x : 0.0), tag);
                system.addConstraintCoordinateY(gcsPoints[c.entity1],
                    pool.alloc(gcsPoints[c.entity1].y ? *gcsPoints[c.entity1].y : 0.0), tag);
            }
            break;

        // ── Newly wired constraint types ─────────────────────

        case ConstraintType::MidpointOnLine:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsLines.size() &&
                c.entity2 >= 0 && c.entity2 < (int)gcsLines.size()) {
                system.addConstraintMidpointOnLine(gcsLines[c.entity1], gcsLines[c.entity2], tag);
            }
            break;

        case ConstraintType::TangentLineArc:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsLines.size() &&
                c.entity2 >= 0 && c.entity2 < (int)gcsArcs.size()) {
                system.addConstraintTangent(gcsLines[c.entity1], gcsArcs[c.entity2], tag);
            }
            break;

        case ConstraintType::TangentArcArc:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsArcs.size() &&
                c.entity2 >= 0 && c.entity2 < (int)gcsArcs.size()) {
                system.addConstraintTangent(gcsArcs[c.entity1], gcsArcs[c.entity2], tag);
            }
            break;

        case ConstraintType::TangentCircleCircle:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsCircles.size() &&
                c.entity2 >= 0 && c.entity2 < (int)gcsCircles.size()) {
                system.addConstraintTangent(gcsCircles[c.entity1], gcsCircles[c.entity2], tag);
            }
            break;

        case ConstraintType::TangentLineCircle:
            // Alias of Tangent for line-circle
            if (c.entity1 >= 0 && c.entity1 < (int)gcsLines.size() &&
                c.entity2 >= 0 && c.entity2 < (int)gcsCircles.size()) {
                system.addConstraintTangent(gcsLines[c.entity1], gcsCircles[c.entity2], tag);
            }
            break;

        case ConstraintType::TangentCircleArc:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsCircles.size() &&
                c.entity2 >= 0 && c.entity2 < (int)gcsArcs.size()) {
                system.addConstraintTangent(gcsCircles[c.entity1], gcsArcs[c.entity2], tag);
            }
            break;

        case ConstraintType::PointOnArc:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsPoints.size() &&
                c.entity2 >= 0 && c.entity2 < (int)gcsArcs.size()) {
                system.addConstraintPointOnArc(gcsPoints[c.entity1], gcsArcs[c.entity2], tag);
            }
            break;

        case ConstraintType::EqualRadius:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsCircles.size() &&
                c.entity2 >= 0 && c.entity2 < (int)gcsCircles.size()) {
                system.addConstraintEqualRadius(gcsCircles[c.entity1], gcsCircles[c.entity2], tag);
            }
            break;

        case ConstraintType::EqualRadiusCircleArc:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsCircles.size() &&
                c.entity2 >= 0 && c.entity2 < (int)gcsArcs.size()) {
                system.addConstraintEqualRadius(gcsCircles[c.entity1], gcsArcs[c.entity2], tag);
            }
            break;

        case ConstraintType::ArcLength:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsArcs.size()) {
                system.addConstraintArcLength(gcsArcs[c.entity1], pool.alloc(c.value), tag);
            }
            break;

        case ConstraintType::CoordinateX:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsPoints.size()) {
                system.addConstraintCoordinateX(gcsPoints[c.entity1], pool.alloc(c.value), tag);
            }
            break;

        case ConstraintType::CoordinateY:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsPoints.size()) {
                system.addConstraintCoordinateY(gcsPoints[c.entity1], pool.alloc(c.value), tag);
            }
            break;

        case ConstraintType::PointToLineDistance:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsPoints.size() &&
                c.entity2 >= 0 && c.entity2 < (int)gcsLines.size()) {
                system.addConstraintP2LDistance(gcsPoints[c.entity1], gcsLines[c.entity2],
                                                pool.alloc(c.value), tag);
            }
            break;

        case ConstraintType::CircleToLineDistance:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsCircles.size() &&
                c.entity2 >= 0 && c.entity2 < (int)gcsLines.size()) {
                system.addConstraintC2LDistance(gcsCircles[c.entity1], gcsLines[c.entity2],
                                                pool.alloc(c.value), tag);
            }
            break;

        case ConstraintType::PointToCircleDistance:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsPoints.size() &&
                c.entity2 >= 0 && c.entity2 < (int)gcsCircles.size()) {
                system.addConstraintP2CDistance(gcsPoints[c.entity1], gcsCircles[c.entity2],
                                                pool.alloc(c.value), tag);
            }
            break;

        case ConstraintType::CircleDiameter:
            if (c.entity1 >= 0 && c.entity1 < (int)gcsArcs.size()) {
                system.addConstraintArcDiameter(gcsArcs[c.entity1], pool.alloc(c.value), tag);
            } else if (c.entity1 >= 0 && c.entity1 < (int)gcsCircles.size()) {
                // Diameter = 2 * radius
                system.addConstraintCircleRadius(gcsCircles[c.entity1],
                                                  pool.alloc(c.value / 2.0), tag);
            }
            break;

        case ConstraintType::Collinear:
            // Two lines are collinear = parallel + first point of line2 on line1
            if (c.entity1 >= 0 && c.entity1 < (int)gcsLines.size() &&
                c.entity2 >= 0 && c.entity2 < (int)gcsLines.size()) {
                system.addConstraintParallel(gcsLines[c.entity1], gcsLines[c.entity2], tag);
                system.addConstraintPointOnLine(gcsLines[c.entity2].p1, gcsLines[c.entity1], tag);
            }
            break;

        case ConstraintType::Concentric:
            // Two circles have same center = coincident centers
            if (c.entity1 >= 0 && c.entity1 < (int)gcsCircles.size() &&
                c.entity2 >= 0 && c.entity2 < (int)gcsCircles.size()) {
                system.addConstraintP2PCoincident(
                    gcsCircles[c.entity1].center, gcsCircles[c.entity2].center, tag);
            }
            break;

        } // switch
    }

    // ── Declare unknown parameters ────────────────────────────
    // PlaneGCS requires all unknowns declared before solving. We also keep a
    // parallel snapshot of the pre-solve values so we can restore them if the
    // solve fails or is conflicting — see "Fail-closed" section below.
    GCS::VEC_pD params;
    std::vector<double> preSolveSnapshot;
    {
        for (auto& p : points) {
            params.push_back(&p.x);
            params.push_back(&p.y);
        }
        for (auto& l : lines) {
            params.push_back(&l.p1.x); params.push_back(&l.p1.y);
            params.push_back(&l.p2.x); params.push_back(&l.p2.y);
        }
        for (auto& c : circles) {
            params.push_back(&c.center.x); params.push_back(&c.center.y);
            params.push_back(&c.radius);
        }
        for (auto& a : arcs) {
            params.push_back(&a.center.x); params.push_back(&a.center.y);
            params.push_back(&a.start.x); params.push_back(&a.start.y);
            params.push_back(&a.end.x); params.push_back(&a.end.y);
            params.push_back(&a.radius);
        }
        preSolveSnapshot.reserve(params.size());
        for (double* p : params) preSolveSnapshot.push_back(*p);
    }

    // Helper: restore every parameter to its pre-solve value. Used on both
    // explicit Failed/Conflicting exits AND on any thrown exception from
    // PlaneGCS/Eigen (solve() / initSolution() can throw on NaN input or
    // ill-conditioned systems).
    auto restoreSnapshot = [&]() noexcept {
        for (size_t i = 0; i < params.size() && i < preSolveSnapshot.size(); ++i) {
            *params[i] = preSolveSnapshot[i];
        }
    };

    try {
        system.declareUnknowns(params);
        system.initSolution();

        // ── Solve ────────────────────────────────────────────────
        // Try DogLeg first (fastest), then LevenbergMarquardt (more robust).
        // Note: solve(bool isFine, Algorithm alg) — must pass isFine explicitly.
        int solveStatus = system.solve(true, GCS::DogLeg);

        if (solveStatus != GCS::Success && solveStatus != GCS::Converged) {
            // Retry with Levenberg-Marquardt (more robust for ill-conditioned systems)
            solveStatus = system.solve(true, GCS::LevenbergMarquardt);
        }

        if (solveStatus != GCS::Success && solveStatus != GCS::Converged) {
            // Last resort: BFGS
            solveStatus = system.solve(true, GCS::BFGS);
        }

        switch (solveStatus) {
            case GCS::Success:
            case GCS::Converged:
                result.status = SolveStatus::OK;
                break;
            case GCS::Failed:
                result.status = SolveStatus::Failed;
                ctx.diag().error(ErrorCode::SKETCH_SOLVE_FAILED,
                             "Constraint solver did not converge after 3 algorithm attempts",
                             {}, "Check for conflicting constraints or try simplifying the sketch");
                break;
            default:
                result.status = SolveStatus::Failed;
                break;
        }

        // ── Diagnose ─────────────────────────────────────────────
        // Must call diagnose() before getRedundant/getConflicting/dofsNumber
        system.diagnose();
        result.degreesOfFreedom = system.dofsNumber();

        // Check for redundant constraints
        system.getRedundant(result.redundantConstraints);
        if (!result.redundantConstraints.empty() && result.status == SolveStatus::OK) {
            result.status = SolveStatus::Redundant;
        }

        // Check for conflicting constraints
        system.getConflicting(result.conflictingConstraints);
        if (!result.conflictingConstraints.empty()) {
            result.status = SolveStatus::Conflicting;
            ctx.diag().error(ErrorCode::SKETCH_CONFLICTING,
                         "Sketch has " + std::to_string(result.conflictingConstraints.size())
                         + " conflicting constraint(s)");
        }

        // Fail-closed: commit solved values only when the solver produced a
        // valid solution. On Failed or Conflicting, restore from the snapshot.
        if (result.status == SolveStatus::OK || result.status == SolveStatus::Redundant) {
            system.applySolution();
        } else {
            restoreSnapshot();
        }
    } catch (const std::exception& e) {
        // PlaneGCS / Eigen can throw on NaN / ill-conditioned systems. Restore
        // the snapshot so sketch state is unchanged, then report.
        restoreSnapshot();
        result.status = SolveStatus::Failed;
        ctx.diag().error(ErrorCode::SKETCH_SOLVE_FAILED,
                     std::string("Sketch solver threw an exception: ") + e.what(),
                     {}, "Check for NaN inputs or extremely ill-conditioned geometry");
    } catch (...) {
        restoreSnapshot();
        result.status = SolveStatus::Failed;
        ctx.diag().error(ErrorCode::SKETCH_SOLVE_FAILED,
                     "Sketch solver threw an unknown exception");
    }

    return scope.makeResult(std::move(result));
}

OperationResult<NamedShape> sketchToWire(
    KernelContext& ctx,
    const std::vector<SketchLine>& lines,
    const std::vector<SketchCircle>& circles,
    const std::vector<SketchArc>& arcs)
{
    DiagnosticScope scope(ctx);

    if (lines.empty() && circles.empty() && arcs.empty()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT, "No sketch entities to convert to wire");
        return scope.makeFailure<NamedShape>();
    }

    BRepBuilderAPI_MakeWire wireBuilder;
    bool hasEdges = false;

    // Add line edges
    for (auto& l : lines) {
        gp_Pnt p1(l.p1.x, l.p1.y, 0.0);
        gp_Pnt p2(l.p2.x, l.p2.y, 0.0);
        if (p1.Distance(p2) < 1e-8) continue;  // Skip degenerate edges

        BRepBuilderAPI_MakeEdge edgeMaker(p1, p2);
        if (edgeMaker.IsDone()) {
            wireBuilder.Add(edgeMaker.Edge());
            hasEdges = true;
        }
    }

    // Add circle edges (full circles)
    for (auto& c : circles) {
        if (c.radius <= 1e-8) continue;
        gp_Circ circ(gp_Ax2(gp_Pnt(c.center.x, c.center.y, 0.0), gp_Dir(0, 0, 1)), c.radius);
        BRepBuilderAPI_MakeEdge edgeMaker(circ);
        if (edgeMaker.IsDone()) {
            wireBuilder.Add(edgeMaker.Edge());
            hasEdges = true;
        }
    }

    // Add arc edges
    for (auto& a : arcs) {
        if (a.radius <= 1e-8) continue;
        gp_Pnt startPt(a.start.x, a.start.y, 0.0);
        gp_Pnt endPt(a.end.x, a.end.y, 0.0);
        gp_Pnt centerPt(a.center.x, a.center.y, 0.0);

        if (startPt.Distance(endPt) < 1e-8) continue;

        // Compute a mid-point on the arc for GC_MakeArcOfCircle
        double startAngle = std::atan2(a.start.y - a.center.y, a.start.x - a.center.x);
        double endAngle = std::atan2(a.end.y - a.center.y, a.end.x - a.center.x);
        double midAngle = (startAngle + endAngle) / 2.0;
        if (endAngle < startAngle) midAngle += M_PI;

        gp_Pnt midPt(a.center.x + a.radius * std::cos(midAngle),
                      a.center.y + a.radius * std::sin(midAngle),
                      0.0);

        GC_MakeArcOfCircle arcMaker(startPt, midPt, endPt);
        if (arcMaker.IsDone()) {
            BRepBuilderAPI_MakeEdge edgeMaker(arcMaker.Value());
            if (edgeMaker.IsDone()) {
                wireBuilder.Add(edgeMaker.Edge());
                hasEdges = true;
            }
        }
    }

    if (!hasEdges) {
        ctx.diag().error(ErrorCode::OCCT_FAILURE,
                     "No valid edges could be created from sketch entities");
        return scope.makeFailure<NamedShape>();
    }

    if (wireBuilder.Error() != BRepBuilderAPI_WireDone) {
        // Try to build what we have even if the wire isn't closed
        // Some operations (like extrude) can work with open wires
    }

    TopoDS_Wire wire = wireBuilder.Wire();
    if (wire.IsNull()) {
        ctx.diag().error(ErrorCode::OCCT_FAILURE,
                     "Failed to build wire from sketch entities",
                     {}, "Edges may not connect. Check that sketch entities form a closed or connected path.");
        return scope.makeFailure<NamedShape>();
    }

    auto tag = ctx.tags().nextShapeIdentity();
    return scope.makeResult(NamedShape(wire, tag));
}

} // namespace oreo
