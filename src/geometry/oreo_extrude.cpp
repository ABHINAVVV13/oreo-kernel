// oreo_extrude.cpp — Extrude with OperationResult + DiagnosticScope.

#include "oreo_geometry.h"
#include "core/diagnostic_scope.h"
#include "core/validation.h"
#include "io/shape_fix.h"
#include "naming/map_shape_elements.h"
#include "naming/shape_mapper.h"

#include <BRepPrimAPI_MakePrism.hxx>

namespace oreo {

GeomResult extrude(KernelContext& ctx, const NamedShape& base, const gp_Vec& direction) {
    DiagnosticScope scope(ctx);

    if (!validation::requireNonNull(ctx, base, "base")) return scope.makeFailure<NamedShape>();
    if (!validation::requireNonZeroVec(ctx, direction, "direction")) return scope.makeFailure<NamedShape>();

    // Convert direction vector from document units to kernel units
    gp_Vec kernelDir(ctx.units.toKernelLength(direction.X()),
                     ctx.units.toKernelLength(direction.Y()),
                     ctx.units.toKernelLength(direction.Z()));

    BRepPrimAPI_MakePrism maker(base.shape(), kernelDir);
    if (!maker.IsDone()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "BRepPrimAPI_MakePrism failed");
        return scope.makeFailure<NamedShape>();
    }

    TopoDS_Shape result = maker.Shape();
    if (result.IsNull()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "Extrude produced null shape");
        return scope.makeFailure<NamedShape>();
    }

    if (!isShapeValid(result)) {
        fixShape(result);
        if (!isShapeValid(result)) {
            Diagnostic d;
            d.severity = Severity::Warning;
            d.code = ErrorCode::SHAPE_INVALID;
            d.message = "Extrude result invalid after ShapeFix — geometry may be degraded";
            d.geometryDegraded = true;
            ctx.diag.report(d);
        }
    }

    auto tag = ctx.tags.nextTag();
    MakerMapper mapper(maker);
    auto mapped = mapShapeElements(ctx, result, mapper, {base}, tag, "Extrude");
    return scope.makeResult(mapped);
}

} // namespace oreo
