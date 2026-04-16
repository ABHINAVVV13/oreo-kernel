// oreo_query.cpp — Shape queries implementation.

#include "oreo_query.h"
#include "core/oreo_error.h"

#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

namespace oreo {

BBox aabb(KernelContext& ctx, const NamedShape& shape) {
    ctx.beginOperation();
    BBox box = {0,0,0,0,0,0};

    if (shape.isNull()) {
        ctx.diag.error(ErrorCode::INVALID_INPUT, "Cannot compute AABB of null shape");
        return box;
    }

    Bnd_Box bbox;
    BRepBndLib::Add(shape.shape(), bbox);
    if (bbox.IsVoid()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "AABB computation returned void");
        return box;
    }

    bbox.Get(box.xmin, box.ymin, box.zmin, box.xmax, box.ymax, box.zmax);
    return box;
}

BBox footprint(KernelContext& ctx, const NamedShape& before, const NamedShape& after) {
    BBox a = aabb(ctx, before);
    BBox b = aabb(ctx, after);
    return {
        b.xmin - a.xmin, b.ymin - a.ymin, b.zmin - a.zmin,
        b.xmax - a.xmax, b.ymax - a.ymax, b.zmax - a.zmax
    };
}

std::vector<NamedFace> getFaces(KernelContext& ctx, const NamedShape& shape) {
    std::vector<NamedFace> result;
    if (shape.isNull()) return result;

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
    return result;
}

std::vector<NamedEdge> getEdges(KernelContext& ctx, const NamedShape& shape) {
    std::vector<NamedEdge> result;
    if (shape.isNull()) return result;

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
    return result;
}

double measureDistance(KernelContext& ctx, const NamedShape& a, const NamedShape& b) {
    ctx.beginOperation();

    if (a.isNull() || b.isNull()) {
        ctx.diag.error(ErrorCode::INVALID_INPUT, "Cannot measure distance with null shape");
        return -1.0;
    }

    BRepExtrema_DistShapeShape dist(a.shape(), b.shape());
    if (!dist.IsDone()) {
        ctx.diag.error(ErrorCode::OCCT_FAILURE, "Distance computation failed");
        return -1.0;
    }

    return dist.Value();
}

MassProperties massProperties(KernelContext& ctx, const NamedShape& shape) {
    ctx.beginOperation();
    MassProperties props = {};

    if (shape.isNull()) {
        ctx.diag.error(ErrorCode::INVALID_INPUT, "Cannot compute mass properties of null shape");
        return props;
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

    return props;
}

} // namespace oreo
