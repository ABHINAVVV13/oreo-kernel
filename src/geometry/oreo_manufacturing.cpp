// oreo_manufacturing.cpp — Manufacturing operations: draft, hole, rib, pocket.

#include "oreo_geometry.h"
#include "core/diagnostic_scope.h"
#include "core/oreo_error.h"
#include "core/oreo_tolerance.h"
#include "core/validation.h"
#include "io/shape_fix.h"
#include "naming/map_shape_elements.h"
#include "naming/shape_mapper.h"

#include <BRepOffsetAPI_DraftAngle.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <TopoDS.hxx>
#include <gp_Trsf.hxx>
#include <Precision.hxx>

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace oreo {

GeomResult draft(KernelContext& ctx,
                 const NamedShape& solid,
                 const std::vector<NamedFace>& faces,
                 double angleDeg,
                 const gp_Dir& pullDirection)
{
    DiagnosticScope scope(ctx);

    if (!validation::requireNonNull(ctx, solid, "solid")) return scope.makeFailure<NamedShape>();
    if (!validation::requireNonEmpty(ctx, faces, "faces")) return scope.makeFailure<NamedShape>();
    if (!std::isfinite(angleDeg)) {
        ctx.diag.error(ErrorCode::INVALID_INPUT, "Draft angle is NaN or Inf");
        return scope.makeFailure<NamedShape>();
    }
    if (std::abs(angleDeg) < Precision::Angular()) {
        ctx.diag.error(ErrorCode::INVALID_INPUT, "Draft angle is effectively zero");
        return scope.makeFailure<NamedShape>();
    }

    // Unit conversion: angleDeg is an angle
    const double kAngleRad = ctx.units().toKernelAngle(angleDeg);

    BRepOffsetAPI_DraftAngle maker(solid.shape());
    for (auto& f : faces) {
        if (f.face.IsNull() || f.face.ShapeType() != TopAbs_FACE) continue;
        try {
            maker.Add(TopoDS::Face(f.face), pullDirection, kAngleRad, gp_Pln());
        } catch (const Standard_Failure&) {
            ctx.diag.error(ErrorCode::OCCT_FAILURE, "Failed to add face to draft operation",
                         f.name.toString(), "Check that the face can be drafted with the given angle");
            return scope.makeFailure<NamedShape>();
        }
    }

    maker.Build();
    if (!maker.IsDone()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "BRepOffsetAPI_DraftAngle failed",
                     {}, "Try reducing the draft angle or simplifying the geometry");
        return scope.makeFailure<NamedShape>();
    }

    TopoDS_Shape result = maker.Shape();
    if (result.IsNull()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "Draft produced null shape");
        return scope.makeFailure<NamedShape>();
    }

    if (!isShapeValid(result)) {
        fixShape(result, ctx.tolerance());
        if (!isShapeValid(result)) {
            Diagnostic d;
            d.severity = Severity::Warning;
            d.code = ErrorCode::SHAPE_INVALID;
            d.message = "Draft result invalid after ShapeFix — geometry may be degraded";
            d.geometryDegraded = true;
            ctx.diag.report(d);
        }
    }

    auto tag = ctx.tags.nextTag();
    MakerMapper mapper(maker);
    auto mapped = mapShapeElements(ctx, result, mapper, {solid}, tag, "Draft");
    return scope.makeResult(mapped);
}

GeomResult hole(KernelContext& ctx,
                const NamedShape& solid,
                const NamedFace& face,
                const gp_Pnt& center,
                double diameter,
                double depth,
                HoleType type)
{
    DiagnosticScope scope(ctx);

    if (!validation::requireNonNull(ctx, solid, "solid")) return scope.makeFailure<NamedShape>();
    if (!validation::requirePositive(ctx, diameter, "diameter")) return scope.makeFailure<NamedShape>();
    if (type != HoleType::Through) {
        if (!validation::requirePositive(ctx, depth, "depth")) return scope.makeFailure<NamedShape>();
    }

    // Unit conversion: diameter and depth are lengths
    const double kDiameter = ctx.units().toKernelLength(diameter);
    const double kDepth    = ctx.units().toKernelLength(depth);

    double radius = kDiameter / 2.0;

    // Determine the face normal to orient the hole
    gp_Dir normal(0, 0, -1);
    if (!face.face.IsNull()) {
        try {
            BRepAdaptor_Surface adapt(TopoDS::Face(face.face));
            gp_Pnt facePt;
            gp_Vec u, v;
            adapt.D1(adapt.FirstUParameter(), adapt.FirstVParameter(), facePt, u, v);
            gp_Vec n = u.Crossed(v);
            if (n.Magnitude() > Precision::Confusion()) {
                normal = gp_Dir(n);
                // Flip normal to point into the solid
                if (face.face.Orientation() == TopAbs_REVERSED) {
                    normal.Reverse();
                }
            }
        } catch (const Standard_Failure&) {
            ctx.diag.warning(ErrorCode::OCCT_FAILURE, "Could not determine face normal, using default (0,0,-1)");
        } catch (...) {
            ctx.diag.warning(ErrorCode::INTERNAL_ERROR, "Unknown exception determining face normal, using default (0,0,-1)");
        }
    }

    // Create the hole cylinder
    double holeDepth = (type == HoleType::Through) ? 1000.0 : kDepth;

    gp_Ax2 cylAxis(center, normal);
    auto cylResult = makeCylinder(ctx, cylAxis, radius, holeDepth);
    if (!cylResult) return scope.makeFailure<NamedShape>();

    // Subtract the cylinder from the solid
    auto cutResult = booleanSubtract(ctx, solid, cylResult.value());
    if (!cutResult) return scope.makeFailure<NamedShape>();

    return scope.makeResult(cutResult.value());
}

GeomResult rib(KernelContext& ctx,
               const NamedShape& solid,
               const NamedShape& ribProfile,
               const gp_Dir& direction,
               double thickness)
{
    DiagnosticScope scope(ctx);

    if (!validation::requireNonNull(ctx, solid, "solid")) return scope.makeFailure<NamedShape>();
    if (!validation::requireNonNull(ctx, ribProfile, "ribProfile")) return scope.makeFailure<NamedShape>();
    if (!validation::requirePositive(ctx, thickness, "thickness")) return scope.makeFailure<NamedShape>();

    // Unit conversion: thickness is a length
    const double kThickness = ctx.units().toKernelLength(thickness);

    // Extrude the rib profile in both directions (half thickness each)
    gp_Vec halfVec(direction);
    halfVec.Scale(kThickness / 2.0);

    // Extrude the profile
    auto extResult = extrude(ctx, ribProfile, gp_Vec(direction).Scaled(kThickness));
    if (!extResult) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "Failed to extrude rib profile");
        return scope.makeFailure<NamedShape>();
    }

    // Boolean union with the solid
    auto fuseResult = booleanUnion(ctx, solid, extResult.value());
    if (!fuseResult) return scope.makeFailure<NamedShape>();

    return scope.makeResult(fuseResult.value());
}

GeomResult pocket(KernelContext& ctx,
                  const NamedShape& solid,
                  const NamedShape& profile,
                  double depth)
{
    DiagnosticScope scope(ctx);

    if (!validation::requireNonNull(ctx, solid, "solid")) return scope.makeFailure<NamedShape>();
    if (!validation::requireNonNull(ctx, profile, "profile")) return scope.makeFailure<NamedShape>();
    if (!validation::requirePositive(ctx, depth, "depth")) return scope.makeFailure<NamedShape>();

    // Unit conversion: depth is a length
    const double kDepth = ctx.units().toKernelLength(depth);

    // Extrude the profile downward
    gp_Vec cutVec(0, 0, -kDepth);
    auto extResult = extrude(ctx, profile, cutVec);
    if (!extResult) {
        // Try positive Z
        cutVec = gp_Vec(0, 0, kDepth);
        extResult = extrude(ctx, profile, cutVec);
        if (!extResult) {
            ctx.diag.error(ErrorCode::OCCT_FAILURE, "Failed to extrude pocket profile");
            return scope.makeFailure<NamedShape>();
        }
    }

    auto cutResult = booleanSubtract(ctx, solid, extResult.value());
    if (!cutResult) return scope.makeFailure<NamedShape>();

    return scope.makeResult(cutResult.value());
}

} // namespace oreo
