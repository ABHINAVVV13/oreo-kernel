// oreo_shell_loft_sweep.cpp — Shell, loft, and sweep operations with OperationResult + DiagnosticScope.

#include "oreo_geometry.h"
#include "core/diagnostic_scope.h"
#include "core/validation.h"
#include "io/shape_fix.h"
#include "naming/map_shape_elements.h"
#include "naming/shape_mapper.h"

#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepOffsetAPI_MakePipe.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>

#include <cmath>

namespace oreo {

GeomResult shell(KernelContext& ctx,
                 const NamedShape& solid,
                 const std::vector<NamedFace>& facesToRemove,
                 double thickness)
{
    DiagnosticScope scope(ctx);

    if (!validation::requireNonNull(ctx, solid, "solid")) return scope.makeFailure<NamedShape>();
    if (!validation::requireNonEmpty(ctx, facesToRemove, "facesToRemove")) return scope.makeFailure<NamedShape>();
    if (!std::isfinite(thickness)) {
        ctx.diag.error(ErrorCode::INVALID_INPUT, "Shell thickness is NaN or Inf");
        return scope.makeFailure<NamedShape>();
    }
    if (std::abs(thickness) < 1e-10) {
        ctx.diag.error(ErrorCode::INVALID_INPUT, "Shell thickness is zero");
        return scope.makeFailure<NamedShape>();
    }

    // Unit conversion: thickness is a length
    const double kThickness = ctx.units().toKernelLength(thickness);

    TopTools_ListOfShape faceList;
    for (auto& f : facesToRemove) {
        if (!f.face.IsNull()) faceList.Append(f.face);
    }

    BRepOffsetAPI_MakeThickSolid maker;
    maker.MakeThickSolidByJoin(solid.shape(), faceList, kThickness, ctx.tolerance().linearPrecision);
    maker.Build();

    if (!maker.IsDone()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "BRepOffsetAPI_MakeThickSolid failed");
        return scope.makeFailure<NamedShape>();
    }

    TopoDS_Shape result = maker.Shape();
    if (result.IsNull()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "Shell produced null shape");
        return scope.makeFailure<NamedShape>();
    }

    if (!isShapeValid(result)) {
        fixShape(result, ctx.tolerance());
        if (!isShapeValid(result)) {
            Diagnostic d;
            d.severity = Severity::Warning;
            d.code = ErrorCode::SHAPE_INVALID;
            d.message = "Shell result invalid after ShapeFix — geometry may be degraded";
            d.geometryDegraded = true;
            ctx.diag.report(d);
        }
    }

    auto tag = ctx.tags.nextTag();
    MakerMapper mapper(maker);
    auto mapped = mapShapeElements(ctx, result, mapper, {solid}, tag, "Shell");
    return scope.makeResult(mapped);
}

GeomResult loft(KernelContext& ctx, const std::vector<NamedShape>& profiles, bool makeSolid) {
    DiagnosticScope scope(ctx);

    if (!validation::requireMinInt(ctx, static_cast<int>(profiles.size()), 2, "profiles.size")) return scope.makeFailure<NamedShape>();

    BRepOffsetAPI_ThruSections maker(makeSolid ? Standard_True : Standard_False);
    int wireCount = 0;
    for (auto& p : profiles) {
        if (p.isNull()) continue;
        if (p.shape().ShapeType() == TopAbs_WIRE) {
            maker.AddWire(TopoDS::Wire(p.shape()));
            ++wireCount;
        }
    }
    if (wireCount < 2) {
        ctx.diag.error(ErrorCode::INVALID_INPUT,
                       "Loft requires at least 2 valid wire profiles, got " + std::to_string(wireCount));
        return scope.makeFailure<NamedShape>();
    }

    maker.Build();
    if (!maker.IsDone()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "BRepOffsetAPI_ThruSections failed");
        return scope.makeFailure<NamedShape>();
    }

    TopoDS_Shape result = maker.Shape();
    if (result.IsNull()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "Loft produced null shape");
        return scope.makeFailure<NamedShape>();
    }

    if (!isShapeValid(result)) {
        fixShape(result, ctx.tolerance());
        if (!isShapeValid(result)) {
            Diagnostic d;
            d.severity = Severity::Warning;
            d.code = ErrorCode::SHAPE_INVALID;
            d.message = "Loft result invalid after ShapeFix — geometry may be degraded";
            d.geometryDegraded = true;
            ctx.diag.report(d);
        }
    }

    auto tag = ctx.tags.nextTag();
    MakerMapper mapper(maker);
    auto mapped = mapShapeElements(ctx, result, mapper, profiles, tag, "Loft");
    return scope.makeResult(mapped);
}

GeomResult sweep(KernelContext& ctx, const NamedShape& profile, const NamedShape& path) {
    DiagnosticScope scope(ctx);

    if (!validation::requireNonNull(ctx, profile, "profile")) return scope.makeFailure<NamedShape>();
    if (!validation::requireNonNull(ctx, path, "path")) return scope.makeFailure<NamedShape>();
    if (!validation::requireShapeType(ctx, path.shape(), TopAbs_WIRE, "path")) return scope.makeFailure<NamedShape>();

    BRepOffsetAPI_MakePipe maker(TopoDS::Wire(path.shape()), profile.shape());
    if (!maker.IsDone()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "BRepOffsetAPI_MakePipe failed");
        return scope.makeFailure<NamedShape>();
    }

    TopoDS_Shape result = maker.Shape();
    if (result.IsNull()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "Sweep produced null shape");
        return scope.makeFailure<NamedShape>();
    }

    if (!isShapeValid(result)) {
        fixShape(result, ctx.tolerance());
        if (!isShapeValid(result)) {
            Diagnostic d;
            d.severity = Severity::Warning;
            d.code = ErrorCode::SHAPE_INVALID;
            d.message = "Sweep result invalid after ShapeFix — geometry may be degraded";
            d.geometryDegraded = true;
            ctx.diag.report(d);
        }
    }

    auto tag = ctx.tags.nextTag();
    MakerMapper mapper(maker);
    auto mapped = mapShapeElements(ctx, result, mapper, {profile, path}, tag, "Sweep");
    return scope.makeResult(mapped);
}

} // namespace oreo
