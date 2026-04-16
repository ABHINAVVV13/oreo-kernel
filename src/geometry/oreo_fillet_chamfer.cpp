// oreo_fillet_chamfer.cpp — Fillet and chamfer operations with OperationResult + DiagnosticScope.

#include "oreo_geometry.h"
#include "core/diagnostic_scope.h"
#include "core/validation.h"
#include "io/shape_fix.h"
#include "naming/map_shape_elements.h"
#include "naming/shape_mapper.h"

#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <TopoDS.hxx>

namespace oreo {

GeomResult fillet(KernelContext& ctx,
                  const NamedShape& solid,
                  const std::vector<NamedEdge>& edges,
                  double radius)
{
    DiagnosticScope scope(ctx);

    if (!validation::requireNonNull(ctx, solid, "solid")) return scope.makeFailure<NamedShape>();
    if (!validation::requireNonEmpty(ctx, edges, "edges")) return scope.makeFailure<NamedShape>();
    if (!validation::requirePositive(ctx, radius, "radius")) return scope.makeFailure<NamedShape>();

    double kernelRadius = ctx.units().toKernelLength(radius);

    BRepFilletAPI_MakeFillet maker(solid.shape());
    int validEdgeCount = 0;
    for (auto& e : edges) {
        if (e.edge.IsNull() || e.edge.ShapeType() != TopAbs_EDGE) continue;
        maker.Add(kernelRadius, TopoDS::Edge(e.edge));
        ++validEdgeCount;
    }
    if (validEdgeCount == 0) {
        ctx.diag.error(ErrorCode::INVALID_INPUT, "No valid edges for fillet — all edges were null or wrong type");
        return scope.makeFailure<NamedShape>();
    }

    maker.Build();
    if (!maker.IsDone()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "BRepFilletAPI_MakeFillet failed");
        return scope.makeFailure<NamedShape>();
    }

    TopoDS_Shape result = maker.Shape();
    if (result.IsNull()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "Fillet produced null shape");
        return scope.makeFailure<NamedShape>();
    }

    if (!isShapeValid(result)) {
        fixShape(result, ctx.tolerance());
        if (!isShapeValid(result)) {
            Diagnostic d;
            d.severity = Severity::Warning;
            d.code = ErrorCode::SHAPE_INVALID;
            d.message = "Fillet result invalid after ShapeFix — geometry may be degraded";
            d.geometryDegraded = true;
            ctx.diag.report(d);
        }
    }

    auto tag = ctx.tags.nextTag();
    MakerMapper mapper(maker);
    auto mapped = mapShapeElements(ctx, result, mapper, {solid}, tag, "Fillet");
    return scope.makeResult(mapped);
}

GeomResult chamfer(KernelContext& ctx,
                   const NamedShape& solid,
                   const std::vector<NamedEdge>& edges,
                   double distance)
{
    DiagnosticScope scope(ctx);

    if (!validation::requireNonNull(ctx, solid, "solid")) return scope.makeFailure<NamedShape>();
    if (!validation::requireNonEmpty(ctx, edges, "edges")) return scope.makeFailure<NamedShape>();
    if (!validation::requirePositive(ctx, distance, "distance")) return scope.makeFailure<NamedShape>();

    double kernelDist = ctx.units().toKernelLength(distance);

    BRepFilletAPI_MakeChamfer maker(solid.shape());
    int validEdgeCount = 0;
    for (auto& e : edges) {
        if (e.edge.IsNull() || e.edge.ShapeType() != TopAbs_EDGE) continue;
        maker.Add(kernelDist, TopoDS::Edge(e.edge));
        ++validEdgeCount;
    }
    if (validEdgeCount == 0) {
        ctx.diag.error(ErrorCode::INVALID_INPUT, "No valid edges for chamfer — all edges were null or wrong type");
        return scope.makeFailure<NamedShape>();
    }

    maker.Build();
    if (!maker.IsDone()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "BRepFilletAPI_MakeChamfer failed");
        return scope.makeFailure<NamedShape>();
    }

    TopoDS_Shape result = maker.Shape();
    if (result.IsNull()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "Chamfer produced null shape");
        return scope.makeFailure<NamedShape>();
    }

    if (!isShapeValid(result)) {
        fixShape(result, ctx.tolerance());
        if (!isShapeValid(result)) {
            Diagnostic d;
            d.severity = Severity::Warning;
            d.code = ErrorCode::SHAPE_INVALID;
            d.message = "Chamfer result invalid after ShapeFix — geometry may be degraded";
            d.geometryDegraded = true;
            ctx.diag.report(d);
        }
    }

    auto tag = ctx.tags.nextTag();
    MakerMapper mapper(maker);
    auto mapped = mapShapeElements(ctx, result, mapper, {solid}, tag, "Chamfer");
    return scope.makeResult(mapped);
}

} // namespace oreo
