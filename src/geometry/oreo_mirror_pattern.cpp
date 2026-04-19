// SPDX-License-Identifier: LGPL-2.1-or-later

// oreo_mirror_pattern.cpp — Production-grade mirror and pattern operations.
//
// Mirror uses BRepBuilderAPI_Transform with proper element mapping.
// Pattern operations build incrementally with element tracking per instance.

#include "oreo_geometry.h"
#include "core/diagnostic_scope.h"
#include "core/occt_try.h"
#include "core/diagnostic.h"
#include "core/kernel_context.h"
#include "core/shape_identity_v1.h"
#include "core/validation.h"
#include "io/shape_fix.h"
#include "naming/map_shape_elements.h"
#include "naming/shape_mapper.h"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <gp_Trsf.hxx>
#include <Precision.hxx>

#include <cmath>

namespace oreo {

GeomResult mirror(KernelContext& ctx, const NamedShape& solid, const gp_Ax2& plane) {
    DiagnosticScope scope(ctx);

    if (!validation::requireNonNull(ctx, solid, "solid")) return scope.makeFailure<NamedShape>();

    OREO_OCCT_TRY
    gp_Trsf trsf;
    trsf.SetMirror(plane);

    BRepBuilderAPI_Transform maker(solid.shape(), trsf, Standard_True /*copy*/);
    if (!maker.IsDone()) {
        ctx.diag().error(ErrorCode::OCCT_FAILURE, "Mirror transform failed");
        return scope.makeFailure<NamedShape>();
    }

    TopoDS_Shape result = maker.Shape();
    if (result.IsNull()) {
        ctx.diag().error(ErrorCode::OCCT_FAILURE, "Mirror produced null shape");
        return scope.makeFailure<NamedShape>();
    }

    auto tag = ctx.tags().nextShapeIdentity();
    MakerMapper mapper(maker);
    auto mapped = mapShapeElements(ctx, result, mapper, {solid}, tag, "Mirror");
    return scope.makeResult(mapped);
    OREO_OCCT_CATCH_NS(scope, ctx, "mirror")
}

GeomResult patternLinear(KernelContext& ctx, const NamedShape& solid, const gp_Vec& direction, int count, double spacing) {
    DiagnosticScope scope(ctx);

    if (!validation::requireNonNull(ctx, solid, "solid")) return scope.makeFailure<NamedShape>();
    if (!validation::requireMinInt(ctx, count, 2, "count")) return scope.makeFailure<NamedShape>();
    if (!validation::requirePositive(ctx, spacing, "spacing")) return scope.makeFailure<NamedShape>();
    if (!validation::requireNonZeroVec(ctx, direction, "direction")) return scope.makeFailure<NamedShape>();

    OREO_OCCT_TRY
    // Unit conversion: spacing is a length, direction components are lengths
    const double kSpacing = ctx.units().toKernelLength(spacing);
    const gp_Vec kDirection(ctx.units().toKernelLength(direction.X()),
                            ctx.units().toKernelLength(direction.Y()),
                            ctx.units().toKernelLength(direction.Z()));

    gp_Vec unitDir = kDirection.Normalized();

    // Build all instances first, then fuse them
    std::vector<TopoDS_Shape> instances;
    instances.push_back(solid.shape());

    for (int i = 1; i < count; ++i) {
        gp_Trsf trsf;
        trsf.SetTranslation(unitDir * (kSpacing * i));

        BRepBuilderAPI_Transform xform(solid.shape(), trsf, Standard_True);
        if (!xform.IsDone()) {
            ctx.diag().error(ErrorCode::OCCT_FAILURE,
                           "Linear pattern transform failed at instance " + std::to_string(i));
            return scope.makeFailure<NamedShape>();
        }
        instances.push_back(xform.Shape());
    }

    // Fuse all instances together
    TopoDS_Shape accumulated = instances[0];
    for (size_t i = 1; i < instances.size(); ++i) {
        BRepAlgoAPI_Fuse fuser(accumulated, instances[i]);

        // Auto-fuzzy tolerance
        Bnd_Box bounds;
        BRepBndLib::Add(accumulated, bounds);
        BRepBndLib::Add(instances[i], bounds);
        if (!bounds.IsVoid()) {
            double fuzzy = ctx.tolerance().booleanFuzzyFactor * std::sqrt(bounds.SquareExtent()) * Precision::Confusion();
            if (fuzzy > 0.0) fuser.SetFuzzyValue(fuzzy);
        }

        fuser.SetRunParallel(Standard_True);
        fuser.Build();

        if (!fuser.IsDone()) {
            ctx.diag().error(ErrorCode::BOOLEAN_FAILED,
                           "Linear pattern fuse failed at instance " + std::to_string(i),
                           "Try increasing spacing or simplifying geometry");
            return scope.makeFailure<NamedShape>();
        }
        accumulated = fuser.Shape();
    }

    if (accumulated.IsNull()) {
        ctx.diag().error(ErrorCode::OCCT_FAILURE, "Linear pattern produced null result");
        return scope.makeFailure<NamedShape>();
    }

    if (!isShapeValid(accumulated)) {
        fixShape(accumulated, ctx.tolerance());
        if (!isShapeValid(accumulated)) {
            Diagnostic d;
            d.severity = Severity::Warning;
            d.code = ErrorCode::SHAPE_INVALID;
            d.message = "LinearPattern result invalid after ShapeFix — geometry may be degraded";
            d.geometryDegraded = true;
            ctx.diag().report(d);
        }
    }

    // Element map: pattern has complex multi-instance history.
    // We create a map referencing the base shape's map as a child,
    // and give each output element a pattern-specific name.
    auto tag = ctx.tags().nextShapeIdentity();
    NullMapper nullMapper;
    auto mapped = mapShapeElements(ctx, accumulated, nullMapper, {solid}, tag, "LinearPattern");

    // Attach the original shape's element map as child for reference
    if (solid.elementMap()) {
        ChildElementMap child;
        child.map = solid.elementMap();
        // Phase 3 native v2: NamedShape carries the full ShapeIdentity,
        // so we can grab it directly — no v1 scalar round-trip, no
        // documentId squeeze.
        child.id = solid.shapeId();
        child.postfix = ";:Plin";
        mapped.elementMap()->addChild(child);
    }

    return scope.makeResult(mapped);
    OREO_OCCT_CATCH_NS(scope, ctx, "patternLinear")
}

GeomResult patternCircular(KernelContext& ctx, const NamedShape& solid, const gp_Ax1& axis, int count, double totalAngleRad) {
    DiagnosticScope scope(ctx);

    if (!validation::requireNonNull(ctx, solid, "solid")) return scope.makeFailure<NamedShape>();
    if (!validation::requireValidAxis(ctx, axis, "axis")) return scope.makeFailure<NamedShape>();
    if (!validation::requireMinInt(ctx, count, 2, "count")) return scope.makeFailure<NamedShape>();
    if (!std::isfinite(totalAngleRad)) {
        ctx.diag().error(ErrorCode::INVALID_INPUT, "Pattern angle is NaN or Inf");
        return scope.makeFailure<NamedShape>();
    }
    if (std::abs(totalAngleRad) < Precision::Angular()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT, "Pattern angle is effectively zero");
        return scope.makeFailure<NamedShape>();
    }

    OREO_OCCT_TRY
    // Unit conversion: totalAngleRad is an angle
    const double kTotalAngle = ctx.units().toKernelAngle(totalAngleRad);

    double stepAngle = kTotalAngle / count;

    // Build all instances
    std::vector<TopoDS_Shape> instances;
    instances.push_back(solid.shape());

    for (int i = 1; i < count; ++i) {
        gp_Trsf trsf;
        trsf.SetRotation(axis, stepAngle * i);

        BRepBuilderAPI_Transform xform(solid.shape(), trsf, Standard_True);
        if (!xform.IsDone()) {
            ctx.diag().error(ErrorCode::OCCT_FAILURE,
                           "Circular pattern transform failed at instance " + std::to_string(i));
            return scope.makeFailure<NamedShape>();
        }
        instances.push_back(xform.Shape());
    }

    // Fuse all instances
    TopoDS_Shape accumulated = instances[0];
    for (size_t i = 1; i < instances.size(); ++i) {
        BRepAlgoAPI_Fuse fuser(accumulated, instances[i]);

        Bnd_Box bounds;
        BRepBndLib::Add(accumulated, bounds);
        BRepBndLib::Add(instances[i], bounds);
        if (!bounds.IsVoid()) {
            double fuzzy = ctx.tolerance().booleanFuzzyFactor * std::sqrt(bounds.SquareExtent()) * Precision::Confusion();
            if (fuzzy > 0.0) fuser.SetFuzzyValue(fuzzy);
        }

        fuser.SetRunParallel(Standard_True);
        fuser.Build();

        if (!fuser.IsDone()) {
            ctx.diag().error(ErrorCode::BOOLEAN_FAILED,
                           "Circular pattern fuse failed at instance " + std::to_string(i),
                           "Try reducing count or simplifying geometry");
            return scope.makeFailure<NamedShape>();
        }
        accumulated = fuser.Shape();
    }

    if (accumulated.IsNull()) {
        ctx.diag().error(ErrorCode::OCCT_FAILURE, "Circular pattern produced null result");
        return scope.makeFailure<NamedShape>();
    }

    if (!isShapeValid(accumulated)) {
        fixShape(accumulated, ctx.tolerance());
        if (!isShapeValid(accumulated)) {
            Diagnostic d;
            d.severity = Severity::Warning;
            d.code = ErrorCode::SHAPE_INVALID;
            d.message = "CircularPattern result invalid after ShapeFix — geometry may be degraded";
            d.geometryDegraded = true;
            ctx.diag().report(d);
        }
    }

    auto tag = ctx.tags().nextShapeIdentity();
    NullMapper nullMapper;
    auto mapped = mapShapeElements(ctx, accumulated, nullMapper, {solid}, tag, "CircularPattern");

    if (solid.elementMap()) {
        ChildElementMap child;
        child.map = solid.elementMap();
        // Phase 3 native v2: see LinearPattern — shapeId() directly.
        child.id = solid.shapeId();
        child.postfix = ";:Pcirc";
        mapped.elementMap()->addChild(child);
    }

    return scope.makeResult(mapped);
    OREO_OCCT_CATCH_NS(scope, ctx, "patternCircular")
}

} // namespace oreo
