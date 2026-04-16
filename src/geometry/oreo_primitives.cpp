// oreo_primitives.cpp — Primitive shape creation with unit enforcement.
//
// All dimension parameters are in DOCUMENT units (from ctx.units).
// They are converted to kernel units (MM) before passing to OCCT.

#include "oreo_geometry.h"
#include "core/diagnostic_scope.h"
#include "core/validation.h"
#include "naming/map_shape_elements.h"
#include "naming/shape_mapper.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeTorus.hxx>
#include <BRepPrimAPI_MakeWedge.hxx>

namespace oreo {

GeomResult makeBox(KernelContext& ctx, double dx, double dy, double dz) {
    DiagnosticScope scope(ctx);
    if (!validation::requirePositive(ctx, dx, "dx")) return scope.makeFailure<NamedShape>();
    if (!validation::requirePositive(ctx, dy, "dy")) return scope.makeFailure<NamedShape>();
    if (!validation::requirePositive(ctx, dz, "dz")) return scope.makeFailure<NamedShape>();

    // Convert document units -> kernel units
    double kdx = ctx.units().toKernelLength(dx);
    double kdy = ctx.units().toKernelLength(dy);
    double kdz = ctx.units().toKernelLength(dz);

    TopoDS_Shape shape = BRepPrimAPI_MakeBox(kdx, kdy, kdz).Shape();
    auto tag = ctx.tags.nextTag();
    NullMapper mapper;
    auto mapped = mapShapeElements(ctx, shape, mapper, {}, tag, "Box");
    return scope.makeResult(mapped);
}

GeomResult makeBox(KernelContext& ctx, const gp_Pnt& origin, double dx, double dy, double dz) {
    DiagnosticScope scope(ctx);
    if (!validation::requireFinitePoint(ctx, origin, "origin")) return scope.makeFailure<NamedShape>();
    if (!validation::requirePositive(ctx, dx, "dx")) return scope.makeFailure<NamedShape>();
    if (!validation::requirePositive(ctx, dy, "dy")) return scope.makeFailure<NamedShape>();
    if (!validation::requirePositive(ctx, dz, "dz")) return scope.makeFailure<NamedShape>();

    double kdx = ctx.units().toKernelLength(dx);
    double kdy = ctx.units().toKernelLength(dy);
    double kdz = ctx.units().toKernelLength(dz);

    // Origin point is assumed already in kernel units (it's a geometric position)
    TopoDS_Shape shape = BRepPrimAPI_MakeBox(origin, kdx, kdy, kdz).Shape();
    auto tag = ctx.tags.nextTag();
    NullMapper mapper;
    auto mapped = mapShapeElements(ctx, shape, mapper, {}, tag, "Box");
    return scope.makeResult(mapped);
}

GeomResult makeCylinder(KernelContext& ctx, double radius, double height) {
    DiagnosticScope scope(ctx);
    if (!validation::requirePositive(ctx, radius, "radius")) return scope.makeFailure<NamedShape>();
    if (!validation::requirePositive(ctx, height, "height")) return scope.makeFailure<NamedShape>();

    double kr = ctx.units().toKernelLength(radius);
    double kh = ctx.units().toKernelLength(height);

    TopoDS_Shape shape = BRepPrimAPI_MakeCylinder(kr, kh).Shape();
    auto tag = ctx.tags.nextTag();
    NullMapper mapper;
    auto mapped = mapShapeElements(ctx, shape, mapper, {}, tag, "Cylinder");
    return scope.makeResult(mapped);
}

GeomResult makeCylinder(KernelContext& ctx, const gp_Ax2& axis, double radius, double height) {
    DiagnosticScope scope(ctx);
    if (!validation::requirePositive(ctx, radius, "radius")) return scope.makeFailure<NamedShape>();
    if (!validation::requirePositive(ctx, height, "height")) return scope.makeFailure<NamedShape>();

    double kr = ctx.units().toKernelLength(radius);
    double kh = ctx.units().toKernelLength(height);

    TopoDS_Shape shape = BRepPrimAPI_MakeCylinder(axis, kr, kh).Shape();
    auto tag = ctx.tags.nextTag();
    NullMapper mapper;
    auto mapped = mapShapeElements(ctx, shape, mapper, {}, tag, "Cylinder");
    return scope.makeResult(mapped);
}

GeomResult makeSphere(KernelContext& ctx, double radius) {
    DiagnosticScope scope(ctx);
    if (!validation::requirePositive(ctx, radius, "radius")) return scope.makeFailure<NamedShape>();

    double kr = ctx.units().toKernelLength(radius);

    TopoDS_Shape shape = BRepPrimAPI_MakeSphere(kr).Shape();
    auto tag = ctx.tags.nextTag();
    NullMapper mapper;
    auto mapped = mapShapeElements(ctx, shape, mapper, {}, tag, "Sphere");
    return scope.makeResult(mapped);
}

GeomResult makeSphere(KernelContext& ctx, const gp_Pnt& center, double radius) {
    DiagnosticScope scope(ctx);
    if (!validation::requirePositive(ctx, radius, "radius")) return scope.makeFailure<NamedShape>();

    double kr = ctx.units().toKernelLength(radius);

    TopoDS_Shape shape = BRepPrimAPI_MakeSphere(center, kr).Shape();
    auto tag = ctx.tags.nextTag();
    NullMapper mapper;
    auto mapped = mapShapeElements(ctx, shape, mapper, {}, tag, "Sphere");
    return scope.makeResult(mapped);
}

GeomResult makeCone(KernelContext& ctx, double radius1, double radius2, double height) {
    DiagnosticScope scope(ctx);
    if (!validation::requireNonNegative(ctx, radius1, "radius1")) return scope.makeFailure<NamedShape>();
    if (!validation::requireNonNegative(ctx, radius2, "radius2")) return scope.makeFailure<NamedShape>();
    if (!validation::requirePositive(ctx, height, "height")) return scope.makeFailure<NamedShape>();
    if (radius1 == 0 && radius2 == 0) {
        ctx.diag.error(ErrorCode::INVALID_INPUT, "At least one cone radius must be positive");
        return scope.makeFailure<NamedShape>();
    }

    double kr1 = ctx.units().toKernelLength(radius1);
    double kr2 = ctx.units().toKernelLength(radius2);
    double kh = ctx.units().toKernelLength(height);

    TopoDS_Shape shape = BRepPrimAPI_MakeCone(kr1, kr2, kh).Shape();
    auto tag = ctx.tags.nextTag();
    NullMapper mapper;
    auto mapped = mapShapeElements(ctx, shape, mapper, {}, tag, "Cone");
    return scope.makeResult(mapped);
}

GeomResult makeTorus(KernelContext& ctx, double majorRadius, double minorRadius) {
    DiagnosticScope scope(ctx);
    if (!validation::requirePositive(ctx, majorRadius, "majorRadius")) return scope.makeFailure<NamedShape>();
    if (!validation::requirePositive(ctx, minorRadius, "minorRadius")) return scope.makeFailure<NamedShape>();
    if (minorRadius >= majorRadius) {
        ctx.diag.error(ErrorCode::INVALID_INPUT, "Torus minor radius must be less than major radius");
        return scope.makeFailure<NamedShape>();
    }

    double kmaj = ctx.units().toKernelLength(majorRadius);
    double kmin = ctx.units().toKernelLength(minorRadius);

    TopoDS_Shape shape = BRepPrimAPI_MakeTorus(kmaj, kmin).Shape();
    auto tag = ctx.tags.nextTag();
    NullMapper mapper;
    auto mapped = mapShapeElements(ctx, shape, mapper, {}, tag, "Torus");
    return scope.makeResult(mapped);
}

GeomResult makeWedge(KernelContext& ctx, double dx, double dy, double dz, double ltx) {
    DiagnosticScope scope(ctx);
    if (!validation::requirePositive(ctx, dx, "dx")) return scope.makeFailure<NamedShape>();
    if (!validation::requirePositive(ctx, dy, "dy")) return scope.makeFailure<NamedShape>();
    if (!validation::requirePositive(ctx, dz, "dz")) return scope.makeFailure<NamedShape>();

    double kdx = ctx.units().toKernelLength(dx);
    double kdy = ctx.units().toKernelLength(dy);
    double kdz = ctx.units().toKernelLength(dz);
    double kltx = ctx.units().toKernelLength(ltx);

    TopoDS_Shape shape = BRepPrimAPI_MakeWedge(kdx, kdy, kdz, kltx).Shape();
    auto tag = ctx.tags.nextTag();
    NullMapper mapper;
    auto mapped = mapShapeElements(ctx, shape, mapper, {}, tag, "Wedge");
    return scope.makeResult(mapped);
}

} // namespace oreo
