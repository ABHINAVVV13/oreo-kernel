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
#include "naming/named_shape.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <gp_Circ.hxx>
#include <gp_Pnt.hxx>
#include <TopoDS_Wire.hxx>

#include <cmath>
#include <memory>
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

SolveResult solveSketch(
    KernelContext& ctx,
    std::vector<SketchPoint>& points,
    std::vector<SketchLine>& lines,
    std::vector<SketchCircle>& circles,
    std::vector<SketchArc>& arcs,
    const std::vector<SketchConstraint>& constraints)
{
    ctx.beginOperation();
    SolveResult result;
    result.status = SolveStatus::OK;
    result.degreesOfFreedom = 0;

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
    // PlaneGCS requires all unknowns declared before solving.
    {
        GCS::VEC_pD params;
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
        system.declareUnknowns(params);
        system.initSolution();
    }

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
            ctx.diag.error(ErrorCode::SKETCH_SOLVE_FAILED,
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
        ctx.diag.error(ErrorCode::SKETCH_CONFLICTING,
                     "Sketch has " + std::to_string(result.conflictingConstraints.size())
                     + " conflicting constraint(s)");
    }

    // Apply solved values
    system.applySolution();

    return result;
}

NamedShape sketchToWire(
    KernelContext& ctx,
    const std::vector<SketchLine>& lines,
    const std::vector<SketchCircle>& circles,
    const std::vector<SketchArc>& arcs)
{
    ctx.beginOperation();

    if (lines.empty() && circles.empty() && arcs.empty()) {
        ctx.diag.error(ErrorCode::INVALID_INPUT, "No sketch entities to convert to wire");
        return {};
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
        ctx.diag.error(ErrorCode::OCCT_FAILURE,
                     "No valid edges could be created from sketch entities");
        return {};
    }

    if (wireBuilder.Error() != BRepBuilderAPI_WireDone) {
        // Try to build what we have even if the wire isn't closed
        // Some operations (like extrude) can work with open wires
    }

    TopoDS_Wire wire = wireBuilder.Wire();
    if (wire.IsNull()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE,
                     "Failed to build wire from sketch entities",
                     {}, "Edges may not connect. Check that sketch entities form a closed or connected path.");
        return {};
    }

    auto tag = ctx.tags.nextTag();
    return NamedShape(wire, tag);
}

} // namespace oreo
