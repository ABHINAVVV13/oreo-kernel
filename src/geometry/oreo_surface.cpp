// oreo_surface.cpp — Surface/face operations: offset, thicken, split, variable fillet.

#include "oreo_geometry.h"
#include "core/diagnostic_scope.h"
#include "core/oreo_error.h"
#include "core/oreo_tolerance.h"
#include "core/validation.h"
#include "io/shape_fix.h"
#include "naming/map_shape_elements.h"
#include "naming/shape_mapper.h"

#include <BRepOffsetAPI_MakeOffsetShape.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepFilletAPI_MakeFillet2d.hxx>
#include <BRepOffsetAPI_MakeOffset.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRep_Builder.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopTools_ListOfShape.hxx>

#include <cmath>

namespace oreo {

GeomResult offset(KernelContext& ctx, const NamedShape& solid, double distance) {
    DiagnosticScope scope(ctx);

    if (!validation::requireNonNull(ctx, solid, "solid")) return scope.makeFailure<NamedShape>();
    if (!std::isfinite(distance)) {
        ctx.diag.error(ErrorCode::INVALID_INPUT, "Offset distance is NaN or Inf");
        return scope.makeFailure<NamedShape>();
    }
    if (std::abs(distance) < 1e-10) {
        ctx.diag.error(ErrorCode::INVALID_INPUT, "Offset distance is effectively zero");
        return scope.makeFailure<NamedShape>();
    }

    // Unit conversion: distance is a length
    const double kDistance = ctx.units().toKernelLength(distance);

    BRepOffsetAPI_MakeOffsetShape maker;
    maker.PerformByJoin(solid.shape(), kDistance, ctx.tolerance().linearPrecision);

    if (!maker.IsDone()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "BRepOffsetAPI_MakeOffsetShape failed",
                     {}, "Try smaller offset distance or simplify geometry");
        return scope.makeFailure<NamedShape>();
    }

    TopoDS_Shape result = maker.Shape();
    if (result.IsNull()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "Offset produced null shape");
        return scope.makeFailure<NamedShape>();
    }

    if (!isShapeValid(result)) {
        fixShape(result, ctx.tolerance());
        if (!isShapeValid(result)) {
            Diagnostic d;
            d.severity = Severity::Warning;
            d.code = ErrorCode::SHAPE_INVALID;
            d.message = "Offset result invalid after ShapeFix — geometry may be degraded";
            d.geometryDegraded = true;
            ctx.diag.report(d);
        }
    }

    auto tag = ctx.tags.nextTag();
    MakerMapper mapper(maker);
    auto mapped = mapShapeElements(ctx, result, mapper, {solid}, tag, "Offset");
    return scope.makeResult(mapped);
}

GeomResult thicken(KernelContext& ctx, const NamedShape& shellOrFace, double thickness) {
    DiagnosticScope scope(ctx);

    if (!validation::requireNonNull(ctx, shellOrFace, "shellOrFace")) return scope.makeFailure<NamedShape>();
    if (!std::isfinite(thickness)) {
        ctx.diag.error(ErrorCode::INVALID_INPUT, "Thicken thickness is NaN or Inf");
        return scope.makeFailure<NamedShape>();
    }
    if (std::abs(thickness) < 1e-10) {
        ctx.diag.error(ErrorCode::INVALID_INPUT, "Thicken thickness is effectively zero");
        return scope.makeFailure<NamedShape>();
    }

    // Unit conversion: thickness is a length
    const double kThickness = ctx.units().toKernelLength(thickness);

    BRepOffsetAPI_MakeThickSolid maker;
    TopTools_ListOfShape emptyFaceList;

    maker.MakeThickSolidByJoin(shellOrFace.shape(), emptyFaceList, kThickness, ctx.tolerance().linearPrecision);
    maker.Build();

    if (!maker.IsDone()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "BRepOffsetAPI_MakeThickSolid (thicken) failed",
                     {}, "Check that the input is a valid shell or face");
        return scope.makeFailure<NamedShape>();
    }

    TopoDS_Shape result = maker.Shape();
    if (result.IsNull()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "Thicken produced null shape");
        return scope.makeFailure<NamedShape>();
    }

    if (!isShapeValid(result)) {
        fixShape(result, ctx.tolerance());
        if (!isShapeValid(result)) {
            Diagnostic d;
            d.severity = Severity::Warning;
            d.code = ErrorCode::SHAPE_INVALID;
            d.message = "Thicken result invalid after ShapeFix — geometry may be degraded";
            d.geometryDegraded = true;
            ctx.diag.report(d);
        }
    }

    auto tag = ctx.tags.nextTag();
    MakerMapper mapper(maker);
    auto mapped = mapShapeElements(ctx, result, mapper, {shellOrFace}, tag, "Thicken");
    return scope.makeResult(mapped);
}

GeomResult splitBody(KernelContext& ctx, const NamedShape& solid, const gp_Pln& plane) {
    DiagnosticScope scope(ctx);

    if (!validation::requireNonNull(ctx, solid, "solid")) return scope.makeFailure<NamedShape>();

    // Size the splitting plane based on the input shape's bounding box
    double extent = 1000.0; // fallback
    {
        Bnd_Box bbox;
        BRepBndLib::Add(solid.shape(), bbox);
        if (!bbox.IsVoid()) {
            double xmin, ymin, zmin, xmax, ymax, zmax;
            bbox.Get(xmin, ymin, zmin, xmax, ymax, zmax);
            double diagonal = std::sqrt((xmax-xmin)*(xmax-xmin) + (ymax-ymin)*(ymax-ymin) + (zmax-zmin)*(zmax-zmin));
            extent = diagonal * 2.0; // 2x diagonal ensures full coverage
        }
    }
    BRepBuilderAPI_MakeFace planeFace(plane, -extent, extent, -extent, extent);
    if (!planeFace.IsDone()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "Failed to create splitting plane face");
        return scope.makeFailure<NamedShape>();
    }

    BRepAlgoAPI_Splitter splitter;
    TopTools_ListOfShape args, tools;
    args.Append(solid.shape());
    tools.Append(planeFace.Shape());
    splitter.SetArguments(args);
    splitter.SetTools(tools);
    splitter.Build();

    if (!splitter.IsDone()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "BRepAlgoAPI_Splitter failed");
        return scope.makeFailure<NamedShape>();
    }

    TopoDS_Shape result = splitter.Shape();
    if (result.IsNull()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "Split produced null shape");
        return scope.makeFailure<NamedShape>();
    }

    auto tag = ctx.tags.nextTag();
    MakerMapper mapper(splitter);
    auto mapped = mapShapeElements(ctx, result, mapper, {solid}, tag, "Split");
    return scope.makeResult(mapped);
}

GeomResult filletVariable(KernelContext& ctx,
                          const NamedShape& solid,
                          const NamedEdge& edge,
                          double startRadius,
                          double endRadius)
{
    DiagnosticScope scope(ctx);

    if (!validation::requireNonNull(ctx, solid, "solid")) return scope.makeFailure<NamedShape>();
    if (edge.edge.IsNull() || edge.edge.ShapeType() != TopAbs_EDGE) {
        ctx.diag.error(ErrorCode::INVALID_INPUT, "Invalid edge for variable fillet");
        return scope.makeFailure<NamedShape>();
    }
    if (!validation::requirePositive(ctx, startRadius, "startRadius")) return scope.makeFailure<NamedShape>();
    if (!validation::requirePositive(ctx, endRadius, "endRadius")) return scope.makeFailure<NamedShape>();

    // Unit conversion: radii are lengths
    const double kStartRadius = ctx.units().toKernelLength(startRadius);
    const double kEndRadius   = ctx.units().toKernelLength(endRadius);

    BRepFilletAPI_MakeFillet maker(solid.shape());
    TopoDS_Edge e = TopoDS::Edge(edge.edge);
    maker.Add(kStartRadius, kEndRadius, e);
    maker.Build();

    if (!maker.IsDone()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "Variable fillet failed",
                     {}, "Try reducing radii or check edge compatibility");
        return scope.makeFailure<NamedShape>();
    }

    TopoDS_Shape result = maker.Shape();
    if (result.IsNull()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "Variable fillet produced null shape");
        return scope.makeFailure<NamedShape>();
    }

    if (!isShapeValid(result)) {
        fixShape(result, ctx.tolerance());
        if (!isShapeValid(result)) {
            Diagnostic d;
            d.severity = Severity::Warning;
            d.code = ErrorCode::SHAPE_INVALID;
            d.message = "VariableFillet result invalid after ShapeFix — geometry may be degraded";
            d.geometryDegraded = true;
            ctx.diag.report(d);
        }
    }

    auto tag = ctx.tags.nextTag();
    MakerMapper mapper(maker);
    auto mapped = mapShapeElements(ctx, result, mapper, {solid}, tag, "VariableFillet");
    return scope.makeResult(mapped);
}

GeomResult wireOffset(KernelContext& ctx, const NamedShape& wire, double distance) {
    DiagnosticScope scope(ctx);

    if (!validation::requireNonNull(ctx, wire, "wire")) return scope.makeFailure<NamedShape>();
    if (wire.shape().ShapeType() != TopAbs_WIRE) {
        ctx.diag.error(ErrorCode::INVALID_INPUT, "Input must be a wire for offset");
        return scope.makeFailure<NamedShape>();
    }
    if (!std::isfinite(distance)) {
        ctx.diag.error(ErrorCode::INVALID_INPUT, "Wire offset distance is NaN or Inf");
        return scope.makeFailure<NamedShape>();
    }

    // Unit conversion: distance is a length
    const double kDistance = ctx.units().toKernelLength(distance);

    BRepOffsetAPI_MakeOffset maker(TopoDS::Wire(wire.shape()));
    maker.Perform(kDistance);

    if (!maker.IsDone()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "Wire offset failed");
        return scope.makeFailure<NamedShape>();
    }

    TopoDS_Shape result = maker.Shape();
    if (result.IsNull()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "Wire offset produced null shape");
        return scope.makeFailure<NamedShape>();
    }

    auto tag = ctx.tags.nextTag();
    MakerMapper mapper(maker);
    auto mapped = mapShapeElements(ctx, result, mapper, {wire}, tag, "WireOffset");
    return scope.makeResult(mapped);
}

GeomResult makeFaceFromWire(KernelContext& ctx, const NamedShape& wire) {
    DiagnosticScope scope(ctx);

    if (wire.isNull()) {
        ctx.diag.error(ErrorCode::INVALID_INPUT, "Cannot make face from null wire");
        return scope.makeFailure<NamedShape>();
    }
    if (wire.shape().ShapeType() != TopAbs_WIRE) {
        ctx.diag.error(ErrorCode::INVALID_INPUT, "Input must be a wire");
        return scope.makeFailure<NamedShape>();
    }

    BRepBuilderAPI_MakeFace maker(TopoDS::Wire(wire.shape()));
    if (!maker.IsDone()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "BRepBuilderAPI_MakeFace failed",
                     {}, "Wire must be planar and closed");
        return scope.makeFailure<NamedShape>();
    }

    TopoDS_Shape result = maker.Shape();
    if (result.IsNull()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "MakeFace produced null shape");
        return scope.makeFailure<NamedShape>();
    }

    auto tag = ctx.tags.nextTag();
    NullMapper mapper;
    auto mapped = mapShapeElements(ctx, result, mapper, {wire}, tag, "MakeFace");
    return scope.makeResult(mapped);
}

GeomResult wireFillet(KernelContext& ctx,
                      const NamedShape& wire,
                      const std::vector<NamedEdge>& vertices,
                      double radius)
{
    DiagnosticScope scope(ctx);

    if (!validation::requireNonNull(ctx, wire, "wire")) return scope.makeFailure<NamedShape>();
    if (wire.shape().ShapeType() != TopAbs_WIRE && wire.shape().ShapeType() != TopAbs_FACE) {
        ctx.diag.error(ErrorCode::INVALID_INPUT, "Input must be a wire or face for 2D fillet");
        return scope.makeFailure<NamedShape>();
    }
    if (vertices.empty()) {
        ctx.diag.error(ErrorCode::INVALID_INPUT, "No vertices specified for wire fillet");
        return scope.makeFailure<NamedShape>();
    }
    if (!validation::requirePositive(ctx, radius, "radius")) return scope.makeFailure<NamedShape>();

    // Unit conversion: radius is a length
    const double kRadius = ctx.units().toKernelLength(radius);

    // BRepFilletAPI_MakeFillet2d works on faces
    TopoDS_Face face;
    if (wire.shape().ShapeType() == TopAbs_FACE) {
        face = TopoDS::Face(wire.shape());
    } else {
        // Make a face from the wire first
        BRepBuilderAPI_MakeFace fm(TopoDS::Wire(wire.shape()));
        if (!fm.IsDone()) {
            ctx.diag.error(ErrorCode::OCCT_FAILURE, "Cannot make face from wire for 2D fillet");
            return scope.makeFailure<NamedShape>();
        }
        face = fm.Face();
    }

    BRepFilletAPI_MakeFillet2d fillet2d(face);
    for (auto& v : vertices) {
        if (!v.edge.IsNull() && v.edge.ShapeType() == TopAbs_VERTEX) {
            fillet2d.AddFillet(TopoDS::Vertex(v.edge), kRadius);
        }
    }
    fillet2d.Build();

    if (!fillet2d.IsDone()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "BRepFilletAPI_MakeFillet2d failed",
                     {}, "Check vertex selection and radius");
        return scope.makeFailure<NamedShape>();
    }

    TopoDS_Shape result = fillet2d.Shape();
    if (result.IsNull()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "Wire fillet produced null shape");
        return scope.makeFailure<NamedShape>();
    }

    auto tag = ctx.tags.nextTag();
    MakerMapper mapper(fillet2d);
    auto mapped = mapShapeElements(ctx, result, mapper, {wire}, tag, "WireFillet");
    return scope.makeResult(mapped);
}

GeomResult combine(KernelContext& ctx, const std::vector<NamedShape>& shapes) {
    DiagnosticScope scope(ctx);

    if (!validation::requireNonEmpty(ctx, shapes, "shapes")) return scope.makeFailure<NamedShape>();

    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);

    for (auto& s : shapes) {
        if (!s.isNull()) {
            builder.Add(compound, s.shape());
        }
    }

    auto tag = ctx.tags.nextTag();
    NullMapper mapper;
    auto mapped = mapShapeElements(ctx, compound, mapper, shapes, tag, "Combine");
    return scope.makeResult(mapped);
}

} // namespace oreo
