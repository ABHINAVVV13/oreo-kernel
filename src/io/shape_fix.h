// shape_fix.h — Automatic shape healing pipeline.
// Runs ShapeFix after STEP imports and failed boolean operations.

#ifndef OREO_SHAPE_FIX_H
#define OREO_SHAPE_FIX_H

#include <TopoDS_Shape.hxx>

namespace oreo {

// Forward declaration
struct TolerancePolicy;

// Attempt to fix an invalid shape using OCCT's ShapeFix.
// Returns true if the shape was modified (fixed).
// The shape is modified in-place.
// Overload with explicit tolerance policy.
bool fixShape(TopoDS_Shape& shape, const TolerancePolicy& tol);

// Convenience overload using default tolerance.
bool fixShape(TopoDS_Shape& shape);

// Check if a shape is valid (BRepCheck_Analyzer).
bool isShapeValid(const TopoDS_Shape& shape);

} // namespace oreo

#endif // OREO_SHAPE_FIX_H
