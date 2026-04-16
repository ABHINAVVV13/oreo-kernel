// validation.h — Strict input validation for all kernel operations.
//
// Every public kernel operation must validate its inputs before execution.
// Invalid inputs produce a Diagnostic with severity Error and return false.
// No silent fallbacks, no default values, no ignored errors.
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

#include <gp_Vec.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <TopoDS_Shape.hxx>
#include <TopAbs_ShapeEnum.hxx>

#include <string>

namespace oreo {

// Forward declarations
class KernelContext;
class NamedShape;

namespace validation {

// ─── Shape validation ────────────────────────────────────────

// Shape must not be null
bool requireNonNull(KernelContext& ctx, const NamedShape& shape, const char* paramName);

// Shape must be of a specific type (face, edge, vertex, etc.)
bool requireShapeType(KernelContext& ctx, const TopoDS_Shape& shape,
                      TopAbs_ShapeEnum expected, const char* paramName);

// Shape must be a valid edge
bool requireValidEdge(KernelContext& ctx, const TopoDS_Shape& shape, const char* paramName);

// Shape must be a valid face
bool requireValidFace(KernelContext& ctx, const TopoDS_Shape& shape, const char* paramName);

// Shape must be a valid wire
bool requireValidWire(KernelContext& ctx, const TopoDS_Shape& shape, const char* paramName);

// ─── Numeric validation ──────────────────────────────────────

// Value must be > 0
bool requirePositive(KernelContext& ctx, double value, const char* paramName);

// Value must be >= 0
bool requireNonNegative(KernelContext& ctx, double value, const char* paramName);

// Value must be within [min, max]
bool requireInRange(KernelContext& ctx, double value, double min, double max, const char* paramName);

// Integer must be >= minVal
bool requireMinInt(KernelContext& ctx, int value, int minVal, const char* paramName);

// ─── Vector/direction validation ─────────────────────────────

// Vector must have non-zero magnitude
bool requireNonZeroVec(KernelContext& ctx, const gp_Vec& vec, const char* paramName);

// Direction must be valid (non-degenerate)
bool requireValidDir(KernelContext& ctx, const gp_Dir& dir, const char* paramName);

// ─── Collection validation ───────────────────────────────────

// Collection must not be empty
template<typename Container>
bool requireNonEmpty(KernelContext& ctx, const Container& collection, const char* paramName);

// ─── String validation ───────────────────────────────────────

// String must not be empty
bool requireNonEmptyString(KernelContext& ctx, const std::string& str, const char* paramName);

} // namespace validation
} // namespace oreo

#endif // OREO_VALIDATION_H
