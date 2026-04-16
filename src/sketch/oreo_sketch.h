// oreo_sketch.h — Sketch solver API wrapping PlaneGCS.
// Converts between oreo-kernel types and PlaneGCS types.
//
// Every operation takes a KernelContext& as its first parameter.
// Every operation takes a KernelContext& as its first parameter.

#ifndef OREO_SKETCH_H
#define OREO_SKETCH_H

#include "core/kernel_context.h"
#include "naming/named_shape.h"

#include <string>
#include <vector>

namespace oreo {

// ── Sketch primitive types ───────────────────────────────────

struct SketchPoint { double x, y; };
struct SketchLine { SketchPoint p1, p2; };
struct SketchCircle { SketchPoint center; double radius; };
struct SketchArc { SketchPoint center, start, end; double radius; };

// ── Constraint types ─────────────────────────────────────────

enum class ConstraintType {
    Coincident,       // Two points are the same
    PointOnLine,      // Point lies on a line
    PointOnCircle,    // Point lies on a circle
    Parallel,         // Two lines are parallel
    Perpendicular,    // Two lines are perpendicular
    Tangent,          // Line tangent to circle, or circle tangent to circle
    Distance,         // Distance between two points
    DistanceX,        // Horizontal distance
    DistanceY,        // Vertical distance
    Angle,            // Angle between two lines
    Radius,           // Circle/arc radius
    Equal,            // Two segments have equal length
    Symmetric,        // Two points symmetric about a line
    Horizontal,       // Line is horizontal
    Vertical,         // Line is vertical
    Fixed,            // Point is fixed in place

    // ── Additional constraint types ──────────────────────
    MidpointOnLine,   // Point at midpoint of a line
    TangentLineArc,   // Line tangent to arc
    TangentArcArc,    // Arc tangent to arc
    TangentCircleCircle, // Circle tangent to circle
    TangentLineCircle,// Line tangent to circle (alias of Tangent)
    TangentCircleArc, // Circle tangent to arc
    PointOnArc,       // Point lies on an arc
    EqualRadius,      // Two circles/arcs have same radius
    EqualRadiusCircleArc, // Circle and arc same radius
    ArcLength,        // Arc has specific length
    CoordinateX,      // Point X coordinate fixed to value
    CoordinateY,      // Point Y coordinate fixed to value
    PointToLineDistance, // Distance from point to line
    CircleToLineDistance, // Distance from circle center to line
    PointToCircleDistance, // Distance from point to circle
    CircleDiameter,   // Circle/arc diameter
    Collinear,        // Two lines on same infinite line (via parallel + point-on-line)
    Concentric,       // Two circles same center (via coincident centers)
};

struct SketchConstraint {
    ConstraintType type;
    int entity1;       // Index into entities array
    int entity2;       // Index into entities array (or -1)
    int entity3;       // Index into entities array (or -1, for symmetric)
    double value;      // Dimension value (for distance, angle, radius)
};

// ── Solve result ─────────────────────────────────────────────

enum class SolveStatus {
    OK,
    Redundant,         // Over-constrained (redundant constraints)
    Conflicting,       // Conflicting constraints
    Underconstrained,  // Some DOF remaining (not necessarily an error)
    Failed,            // Solver did not converge
};

struct SolveResult {
    SolveStatus status;
    int degreesOfFreedom;
    std::vector<int> conflictingConstraints;  // Indices into constraints array
    std::vector<int> redundantConstraints;    // Indices into constraints array
};

// ═══════════════════════════════════════════════════════════════
// Context-aware sketch functions
// ═══════════════════════════════════════════════════════════════

// Solve a 2D sketch.
// Input: arrays of points, lines, circles, arcs, and constraints.
// Output: solved positions (entities modified in-place), solve result.
SolveResult solveSketch(
    KernelContext& ctx,
    std::vector<SketchPoint>& points,
    std::vector<SketchLine>& lines,
    std::vector<SketchCircle>& circles,
    std::vector<SketchArc>& arcs,
    const std::vector<SketchConstraint>& constraints);

// Convert a solved sketch to a wire.
// Takes lines, circles, arcs and produces a TopoDS_Wire.
NamedShape sketchToWire(
    KernelContext& ctx,
    const std::vector<SketchLine>& lines,
    const std::vector<SketchCircle>& circles,
    const std::vector<SketchArc>& arcs);

} // namespace oreo

#endif // OREO_SKETCH_H
