// oreo_query.cpp — Shape queries implementation.

#include "oreo_query.h"
#include "core/oreo_error.h"
#include "core/operation_result.h"
#include "core/diagnostic_scope.h"

#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

namespace oreo {

OperationResult<BBox> aabb(KernelContext& ctx, const NamedShape& shape) {
    DiagnosticScope scope(ctx);
    BBox box = {0,0,0,0,0,0};

    if (shape.isNull()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT, "Cannot compute AABB of null shape");
        return scope.makeFailure<BBox>();
    }

    Bnd_Box bbox;
    BRepBndLib::Add(shape.shape(), bbox);
    if (bbox.IsVoid()) {
        ctx.diag().error(ErrorCode::OCCT_FAILURE, "AABB computation returned void");
        return scope.makeFailure<BBox>();
    }

    bbox.Get(box.xmin, box.ymin, box.zmin, box.xmax, box.ymax, box.zmax);
    return scope.makeResult(box);
}

OperationResult<BBox> footprint(KernelContext& ctx, const NamedShape& before, const NamedShape& after) {
    DiagnosticScope scope(ctx);
    auto a = aabb(ctx, before);
    auto b = aabb(ctx, after);
    if (!a.ok() || !b.ok()) {
        return scope.makeFailure<BBox>();
    }
    BBox result = {
        b.value().xmin - a.value().xmin, b.value().ymin - a.value().ymin, b.value().zmin - a.value().zmin,
        b.value().xmax - a.value().xmax, b.value().ymax - a.value().ymax, b.value().zmax - a.value().zmax
    };
    return scope.makeResult(result);
}

OperationResult<std::vector<NamedFace>> getFaces(KernelContext& ctx, const NamedShape& shape) {
    DiagnosticScope scope(ctx);
    std::vector<NamedFace> result;
    if (shape.isNull()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT, "Cannot get faces of null shape");
        return scope.makeFailure<std::vector<NamedFace>>();
    }

    int i = 0;
    for (TopExp_Explorer ex(shape.shape(), TopAbs_FACE); ex.More(); ex.Next()) {
        ++i;
        IndexedName idx("Face", i);
        MappedName name;
        if (shape.elementMap()) {
            name = shape.elementMap()->getMappedName(idx);
        }
        result.push_back({idx, ex.Current()});
    }
    return scope.makeResult(std::move(result));
}

OperationResult<std::vector<NamedEdge>> getEdges(KernelContext& ctx, const NamedShape& shape) {
    DiagnosticScope scope(ctx);
    std::vector<NamedEdge> result;
    if (shape.isNull()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT, "Cannot get edges of null shape");
        return scope.makeFailure<std::vector<NamedEdge>>();
    }

    int i = 0;
    for (TopExp_Explorer ex(shape.shape(), TopAbs_EDGE); ex.More(); ex.Next()) {
        ++i;
        IndexedName idx("Edge", i);
        MappedName name;
        if (shape.elementMap()) {
            name = shape.elementMap()->getMappedName(idx);
        }
        result.push_back({idx, ex.Current()});
    }
    return scope.makeResult(std::move(result));
}

OperationResult<double> measureDistance(KernelContext& ctx, const NamedShape& a, const NamedShape& b) {
    DiagnosticScope scope(ctx);

    if (a.isNull() || b.isNull()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT, "Cannot measure distance with null shape");
        return scope.makeFailure<double>();
    }

    BRepExtrema_DistShapeShape dist(a.shape(), b.shape());
    if (!dist.IsDone()) {
        ctx.diag().error(ErrorCode::OCCT_FAILURE, "Distance computation failed");
        return scope.makeFailure<double>();
    }

    return scope.makeResult(dist.Value());
}

OperationResult<MassProperties> massProperties(KernelContext& ctx, const NamedShape& shape) {
    DiagnosticScope scope(ctx);
    MassProperties props = {};

    if (shape.isNull()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT, "Cannot compute mass properties of null shape");
        return scope.makeFailure<MassProperties>();
    }

    GProp_GProps volProps;
    BRepGProp::VolumeProperties(shape.shape(), volProps);
    props.volume = volProps.Mass();
    gp_Pnt com = volProps.CentreOfMass();
    props.centerOfMassX = com.X();
    props.centerOfMassY = com.Y();
    props.centerOfMassZ = com.Z();

    gp_Mat inertia = volProps.MatrixOfInertia();
    props.ixx = inertia(1, 1);
    props.iyy = inertia(2, 2);
    props.izz = inertia(3, 3);
    props.ixy = inertia(1, 2);
    props.ixz = inertia(1, 3);
    props.iyz = inertia(2, 3);

    GProp_GProps surfProps;
    BRepGProp::SurfaceProperties(shape.shape(), surfProps);
    props.surfaceArea = surfProps.Mass();

    return scope.makeResult(props);
}

} // namespace oreo
