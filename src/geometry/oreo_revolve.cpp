// oreo_revolve.cpp — Revolve operation with OperationResult + DiagnosticScope.

#include "oreo_geometry.h"
#include "core/diagnostic_scope.h"
#include "core/occt_try.h"
#include "core/validation.h"
#include "io/shape_fix.h"
#include "naming/map_shape_elements.h"
#include "naming/shape_mapper.h"

#include <BRepPrimAPI_MakeRevol.hxx>

namespace oreo {

GeomResult revolve(KernelContext& ctx, const NamedShape& base, const gp_Ax1& axis, double angleRad) {
    DiagnosticScope scope(ctx);

    if (!validation::requireNonNull(ctx, base, "base")) return scope.makeFailure<NamedShape>();
    if (!validation::requireValidAxis(ctx, axis, "axis")) return scope.makeFailure<NamedShape>();

    // Convert angle from document units to kernel units (radians)
    double kernelAngle = ctx.units().toKernelAngle(angleRad);
    if (!validation::requireInRange(ctx, kernelAngle, 0.0 + 1e-12, 2.0 * M_PI + 1e-6, "angle")) return scope.makeFailure<NamedShape>();

    OREO_OCCT_TRY
        BRepPrimAPI_MakeRevol maker(base.shape(), axis, kernelAngle);
        if (!maker.IsDone()) {
            ctx.diag().error(ErrorCode::OCCT_FAILURE, "BRepPrimAPI_MakeRevol failed");
            return scope.makeFailure<NamedShape>();
        }

        TopoDS_Shape result = maker.Shape();
        if (result.IsNull()) {
            ctx.diag().error(ErrorCode::OCCT_FAILURE, "Revolve produced null shape");
            return scope.makeFailure<NamedShape>();
        }

        if (!isShapeValid(result)) {
            fixShape(result, ctx.tolerance());
            if (!isShapeValid(result)) {
                Diagnostic d;
                d.severity = Severity::Warning;
                d.code = ErrorCode::SHAPE_INVALID;
                d.message = "Revolve result invalid after ShapeFix — geometry may be degraded";
                d.geometryDegraded = true;
                ctx.diag().report(d);
            }
        }

        auto tag = ctx.tags().nextShapeIdentity();
        MakerMapper mapper(maker);
        auto mapped = mapShapeElements(ctx, result, mapper, {base}, tag, "Revolve");
        return scope.makeResult(mapped);
    OREO_OCCT_CATCH_NS(scope, ctx, "revolve")
}

} // namespace oreo
