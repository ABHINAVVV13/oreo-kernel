// oreo_capi.cpp — C API wrapper over C++ internals.

#include "oreo_kernel.h"

#include "core/kernel_context.h"
#include "core/diagnostic.h"
#include "geometry/oreo_geometry.h"
#include "io/oreo_step.h"
#include "io/oreo_serialize.h"
#include "naming/named_shape.h"
#include "query/oreo_query.h"
#include "sketch/oreo_sketch.h"
#include "mesh/oreo_mesh.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS.hxx>

#include <cstring>
#include <memory>
#include <string>

// Handle types
struct OreoContext_T { std::shared_ptr<oreo::KernelContext> ctx; };
struct OreoSolid_T { oreo::NamedShape ns; };
struct OreoWire_T  { oreo::NamedShape ns; };
struct OreoEdge_T  { oreo::NamedShape ns; };
struct OreoFace_T  { oreo::NamedShape ns; };

struct OreoSketch_T {
    std::vector<oreo::SketchPoint>      points;
    std::vector<oreo::SketchLine>       lines;
    std::vector<oreo::SketchCircle>     circles;
    std::vector<oreo::SketchArc>        arcs;
    std::vector<oreo::SketchConstraint> constraints;
    oreo::SolveResult                   lastResult = {};
};

namespace {

// Internal default context for legacy C API functions that don't take OreoContext.
std::shared_ptr<oreo::KernelContext>& internalDefaultCtx() {
    static auto ctx = oreo::KernelContext::create();
    return ctx;
}

// Thread-local storage for name strings returned by oreo_face_name etc.
thread_local std::string tl_nameBuffer;

OreoSolid wrapSolid(oreo::NamedShape&& ns) {
    if (ns.isNull()) return nullptr;
    auto* h = new OreoSolid_T{std::move(ns)};
    return h;
}

// Overload for GeomResult (extracts value or returns nullptr)
OreoSolid wrapSolid(oreo::GeomResult&& result) {
    if (!result.ok()) return nullptr;
    return wrapSolid(std::move(result).value());
}

OreoError errorFromContext(const oreo::KernelContext& ctx) {
    static thread_local std::string msgBuf, entBuf, sugBuf;

    auto* lastErr = ctx.diag.lastError();
    if (!lastErr) {
        return {OREO_OK, "", "", ""};
    }

    msgBuf = lastErr->message;
    entBuf = lastErr->elementRef;
    sugBuf = lastErr->suggestion;

    return {
        static_cast<OreoErrorCode>(static_cast<int>(lastErr->code)),
        msgBuf.c_str(),
        entBuf.c_str(),
        sugBuf.c_str()
    };
}

} // anonymous namespace

extern "C" {

// ============================================================
// Lifecycle
// ============================================================

void oreo_init(void) {
    oreo::KernelContext::initOCCT();
    (void)internalDefaultCtx();
}

void oreo_shutdown(void) {
    internalDefaultCtx().reset();
    internalDefaultCtx() = oreo::KernelContext::create();
}

OreoError oreo_last_error(void) {
    return errorFromContext(*internalDefaultCtx());
}

// ============================================================
// Context
// ============================================================

OreoContext oreo_context_create(void) {
    return new OreoContext_T{oreo::KernelContext::create()};
}

void oreo_context_free(OreoContext ctx) {
    delete ctx;
}

int oreo_context_has_errors(OreoContext ctx) {
    if (!ctx) return 0;
    return ctx->ctx->diag.hasErrors() ? 1 : 0;
}

int oreo_context_has_warnings(OreoContext ctx) {
    if (!ctx) return 0;
    return ctx->ctx->diag.hasWarnings() ? 1 : 0;
}

int oreo_context_error_count(OreoContext ctx) {
    if (!ctx) return 0;
    return ctx->ctx->diag.errorCount();
}

const char* oreo_context_last_error_message(OreoContext ctx) {
    if (!ctx) return "";
    auto* last = ctx->ctx->diag.lastError();
    if (!last) return "";
    tl_nameBuffer = last->message;
    return tl_nameBuffer.c_str();
}

// Context-aware primitives
OreoSolid oreo_ctx_make_box(OreoContext ctx, double dx, double dy, double dz) {
    if (!ctx) return nullptr;
    return wrapSolid(oreo::makeBox(*ctx->ctx, dx, dy, dz));
}

OreoSolid oreo_ctx_make_cylinder(OreoContext ctx, double radius, double height) {
    if (!ctx) return nullptr;
    return wrapSolid(oreo::makeCylinder(*ctx->ctx, radius, height));
}

OreoSolid oreo_ctx_make_sphere(OreoContext ctx, double radius) {
    if (!ctx) return nullptr;
    return wrapSolid(oreo::makeSphere(*ctx->ctx, radius));
}

// Context-aware geometry operations
OreoSolid oreo_ctx_extrude(OreoContext ctx, OreoSolid base, double dx, double dy, double dz) {
    if (!ctx || !base) return nullptr;
    return wrapSolid(oreo::extrude(*ctx->ctx, base->ns, gp_Vec(dx, dy, dz)));
}

OreoSolid oreo_ctx_boolean_union(OreoContext ctx, OreoSolid a, OreoSolid b) {
    if (!ctx || !a || !b) return nullptr;
    return wrapSolid(oreo::booleanUnion(*ctx->ctx, a->ns, b->ns));
}

OreoSolid oreo_ctx_boolean_subtract(OreoContext ctx, OreoSolid target, OreoSolid tool) {
    if (!ctx || !target || !tool) return nullptr;
    return wrapSolid(oreo::booleanSubtract(*ctx->ctx, target->ns, tool->ns));
}

OreoSolid oreo_ctx_fillet(OreoContext ctx, OreoSolid solid, OreoEdge edges[], int n, double radius) {
    if (!ctx || !solid) return nullptr;
    std::vector<oreo::NamedEdge> edgeVec;
    for (int i = 0; i < n; ++i) {
        if (edges[i]) edgeVec.push_back({oreo::IndexedName("Edge", i+1), edges[i]->ns.shape()});
    }
    return wrapSolid(oreo::fillet(*ctx->ctx, solid->ns, edgeVec, radius));
}

// ============================================================
// Primitives (legacy — uses default context)
// ============================================================

OreoSolid oreo_make_box(double dx, double dy, double dz) {
    return wrapSolid(oreo::makeBox(*internalDefaultCtx(), dx, dy, dz));
}

OreoSolid oreo_make_cylinder(double radius, double height) {
    return wrapSolid(oreo::makeCylinder(*internalDefaultCtx(), radius, height));
}

OreoSolid oreo_make_sphere(double radius) {
    return wrapSolid(oreo::makeSphere(*internalDefaultCtx(), radius));
}

OreoSolid oreo_make_cone(double r1, double r2, double height) {
    return wrapSolid(oreo::makeCone(*internalDefaultCtx(), r1, r2, height));
}

OreoSolid oreo_make_torus(double major_r, double minor_r) {
    return wrapSolid(oreo::makeTorus(*internalDefaultCtx(), major_r, minor_r));
}

OreoSolid oreo_make_wedge(double dx, double dy, double dz, double ltx) {
    return wrapSolid(oreo::makeWedge(*internalDefaultCtx(), dx, dy, dz, ltx));
}

// ============================================================
// Geometry operations
// ============================================================

OreoSolid oreo_extrude(OreoSolid base, double dx, double dy, double dz) {
    if (!base) { internalDefaultCtx()->diag.error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::extrude(*internalDefaultCtx(), base->ns, gp_Vec(dx, dy, dz)));
}

OreoSolid oreo_revolve(OreoSolid base,
                       double ax, double ay, double az,
                       double dx, double dy, double dz,
                       double angle_rad) {
    if (!base) { internalDefaultCtx()->diag.error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    gp_Ax1 axis(gp_Pnt(ax, ay, az), gp_Dir(dx, dy, dz));
    return wrapSolid(oreo::revolve(*internalDefaultCtx(), base->ns, axis, angle_rad));
}

OreoSolid oreo_fillet(OreoSolid solid, OreoEdge edges[], int n, double radius) {
    if (!solid) { internalDefaultCtx()->diag.error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    std::vector<oreo::NamedEdge> edgeVec;
    for (int i = 0; i < n; ++i) {
        if (edges[i]) {
            edgeVec.push_back({oreo::IndexedName("Edge", i+1), edges[i]->ns.shape()});
        }
    }
    return wrapSolid(oreo::fillet(*internalDefaultCtx(), solid->ns, edgeVec, radius));
}

OreoSolid oreo_chamfer(OreoSolid solid, OreoEdge edges[], int n, double distance) {
    if (!solid) { internalDefaultCtx()->diag.error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    std::vector<oreo::NamedEdge> edgeVec;
    for (int i = 0; i < n; ++i) {
        if (edges[i]) {
            edgeVec.push_back({oreo::IndexedName("Edge", i+1), edges[i]->ns.shape()});
        }
    }
    return wrapSolid(oreo::chamfer(*internalDefaultCtx(), solid->ns, edgeVec, distance));
}

OreoSolid oreo_boolean_union(OreoSolid a, OreoSolid b) {
    if (!a || !b) { internalDefaultCtx()->diag.error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::booleanUnion(*internalDefaultCtx(), a->ns, b->ns));
}

OreoSolid oreo_boolean_subtract(OreoSolid target, OreoSolid tool) {
    if (!target || !tool) { internalDefaultCtx()->diag.error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::booleanSubtract(*internalDefaultCtx(), target->ns, tool->ns));
}

OreoSolid oreo_boolean_intersect(OreoSolid a, OreoSolid b) {
    if (!a || !b) { internalDefaultCtx()->diag.error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::booleanIntersect(*internalDefaultCtx(), a->ns, b->ns));
}

OreoSolid oreo_shell(OreoSolid solid, OreoFace faces[], int n, double thickness) {
    if (!solid) { internalDefaultCtx()->diag.error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    std::vector<oreo::NamedFace> faceVec;
    for (int i = 0; i < n; ++i) {
        if (faces[i]) {
            faceVec.push_back({oreo::IndexedName("Face", i+1), faces[i]->ns.shape()});
        }
    }
    return wrapSolid(oreo::shell(*internalDefaultCtx(), solid->ns, faceVec, thickness));
}

OreoSolid oreo_loft(OreoWire profiles[], int n, int make_solid) {
    std::vector<oreo::NamedShape> pVec;
    for (int i = 0; i < n; ++i) {
        if (profiles[i]) pVec.push_back(profiles[i]->ns);
    }
    return wrapSolid(oreo::loft(*internalDefaultCtx(), pVec, make_solid != 0));
}

OreoSolid oreo_sweep(OreoWire profile, OreoWire path) {
    if (!profile || !path) { internalDefaultCtx()->diag.error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::sweep(*internalDefaultCtx(), profile->ns, path->ns));
}

OreoSolid oreo_mirror(OreoSolid solid,
                      double px, double py, double pz,
                      double nx, double ny, double nz) {
    if (!solid) { internalDefaultCtx()->diag.error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    gp_Ax2 plane(gp_Pnt(px, py, pz), gp_Dir(nx, ny, nz));
    return wrapSolid(oreo::mirror(*internalDefaultCtx(), solid->ns, plane));
}

OreoSolid oreo_pattern_linear(OreoSolid solid,
                              double dx, double dy, double dz,
                              int count, double spacing) {
    if (!solid) { internalDefaultCtx()->diag.error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::patternLinear(*internalDefaultCtx(), solid->ns, gp_Vec(dx, dy, dz), count, spacing));
}

OreoSolid oreo_pattern_circular(OreoSolid solid,
                                double ax, double ay, double az,
                                double dx, double dy, double dz,
                                int count, double total_angle_rad) {
    if (!solid) { internalDefaultCtx()->diag.error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    gp_Ax1 axis(gp_Pnt(ax, ay, az), gp_Dir(dx, dy, dz));
    return wrapSolid(oreo::patternCircular(*internalDefaultCtx(), solid->ns, axis, count, total_angle_rad));
}

// ============================================================
// Manufacturing operations
// ============================================================

OreoSolid oreo_draft(OreoSolid solid, OreoFace faces[], int n,
                     double angle_deg,
                     double pull_dx, double pull_dy, double pull_dz) {
    if (!solid) { internalDefaultCtx()->diag.error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    std::vector<oreo::NamedFace> faceVec;
    for (int i = 0; i < n; ++i) {
        if (faces[i]) faceVec.push_back({oreo::IndexedName("Face", i+1), faces[i]->ns.shape()});
    }
    return wrapSolid(oreo::draft(*internalDefaultCtx(), solid->ns, faceVec, angle_deg, gp_Dir(pull_dx, pull_dy, pull_dz)));
}

OreoSolid oreo_hole(OreoSolid solid, OreoFace face,
                    double cx, double cy, double cz,
                    double diameter, double depth) {
    if (!solid) { internalDefaultCtx()->diag.error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    oreo::NamedFace nf = {oreo::IndexedName("Face", 1), face ? face->ns.shape() : TopoDS_Shape()};
    return wrapSolid(oreo::hole(*internalDefaultCtx(), solid->ns, nf, gp_Pnt(cx, cy, cz), diameter, depth));
}

OreoSolid oreo_pocket(OreoSolid solid, OreoSolid profile, double depth) {
    if (!solid || !profile) { internalDefaultCtx()->diag.error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::pocket(*internalDefaultCtx(), solid->ns, profile->ns, depth));
}

OreoSolid oreo_rib(OreoSolid solid, OreoSolid profile,
                   double dx, double dy, double dz, double thickness) {
    if (!solid || !profile) { internalDefaultCtx()->diag.error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::rib(*internalDefaultCtx(), solid->ns, profile->ns, gp_Dir(dx, dy, dz), thickness));
}

OreoSolid oreo_offset(OreoSolid solid, double distance) {
    if (!solid) { internalDefaultCtx()->diag.error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::offset(*internalDefaultCtx(), solid->ns, distance));
}

OreoSolid oreo_thicken(OreoSolid shell, double thickness) {
    if (!shell) { internalDefaultCtx()->diag.error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::thicken(*internalDefaultCtx(), shell->ns, thickness));
}

OreoSolid oreo_split_body(OreoSolid solid,
                          double px, double py, double pz,
                          double nx, double ny, double nz) {
    if (!solid) { internalDefaultCtx()->diag.error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::splitBody(*internalDefaultCtx(), solid->ns, gp_Pln(gp_Pnt(px, py, pz), gp_Dir(nx, ny, nz))));
}

OreoSolid oreo_fillet_variable(OreoSolid solid, OreoEdge edge,
                               double start_radius, double end_radius) {
    if (!solid || !edge) { internalDefaultCtx()->diag.error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    oreo::NamedEdge ne = {oreo::IndexedName("Edge", 1), edge->ns.shape()};
    return wrapSolid(oreo::filletVariable(*internalDefaultCtx(), solid->ns, ne, start_radius, end_radius));
}

OreoSolid oreo_make_face_from_wire(OreoWire wire) {
    if (!wire) { internalDefaultCtx()->diag.error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::makeFaceFromWire(*internalDefaultCtx(), wire->ns));
}

OreoSolid oreo_combine(OreoSolid shapes[], int n) {
    std::vector<oreo::NamedShape> vec;
    for (int i = 0; i < n; ++i) {
        if (shapes[i]) vec.push_back(shapes[i]->ns);
    }
    return wrapSolid(oreo::combine(*internalDefaultCtx(), vec));
}

// ============================================================
// Queries
// ============================================================

OreoBBox oreo_aabb(OreoSolid solid) {
    if (!solid) return {0,0,0,0,0,0};
    auto b = oreo::aabb(*internalDefaultCtx(), solid->ns);
    return {b.xmin, b.ymin, b.zmin, b.xmax, b.ymax, b.zmax};
}

OreoBBox oreo_footprint(OreoSolid before, OreoSolid after) {
    if (!before || !after) return {0,0,0,0,0,0};
    auto b = oreo::footprint(*internalDefaultCtx(), before->ns, after->ns);
    return {b.xmin, b.ymin, b.zmin, b.xmax, b.ymax, b.zmax};
}

int oreo_face_count(OreoSolid solid) {
    if (!solid) return 0;
    return solid->ns.countSubShapes(TopAbs_FACE);
}

int oreo_edge_count(OreoSolid solid) {
    if (!solid) return 0;
    return solid->ns.countSubShapes(TopAbs_EDGE);
}

OreoFace oreo_get_face(OreoSolid solid, int index) {
    if (!solid || index < 1) return nullptr;
    TopoDS_Shape face = solid->ns.getSubShape(TopAbs_FACE, index);
    if (face.IsNull()) return nullptr;
    auto* h = new OreoFace_T{oreo::NamedShape(face, 0)};
    return h;
}

OreoEdge oreo_get_edge(OreoSolid solid, int index) {
    if (!solid || index < 1) return nullptr;
    TopoDS_Shape edge = solid->ns.getSubShape(TopAbs_EDGE, index);
    if (edge.IsNull()) return nullptr;
    auto* h = new OreoEdge_T{oreo::NamedShape(edge, 0)};
    return h;
}

double oreo_measure_distance(OreoSolid a, OreoSolid b) {
    if (!a || !b) return -1.0;
    return oreo::measureDistance(*internalDefaultCtx(), a->ns, b->ns);
}

OreoMassProps oreo_mass_properties(OreoSolid solid) {
    if (!solid) return {};
    auto m = oreo::massProperties(*internalDefaultCtx(), solid->ns);
    return {m.volume, m.surfaceArea,
            m.centerOfMassX, m.centerOfMassY, m.centerOfMassZ,
            m.ixx, m.iyy, m.izz, m.ixy, m.ixz, m.iyz};
}

// ============================================================
// Element naming
// ============================================================

const char* oreo_face_name(OreoSolid solid, int index) {
    if (!solid || index < 1) return "";
    auto name = solid->ns.getElementName(oreo::IndexedName("Face", index));
    tl_nameBuffer = name.data();
    return tl_nameBuffer.c_str();
}

const char* oreo_edge_name(OreoSolid solid, int index) {
    if (!solid || index < 1) return "";
    auto name = solid->ns.getElementName(oreo::IndexedName("Edge", index));
    tl_nameBuffer = name.data();
    return tl_nameBuffer.c_str();
}

// ============================================================
// I/O
// ============================================================

OreoSolid oreo_import_step(const uint8_t* data, size_t len) {
    auto result = oreo::importStep(*internalDefaultCtx(), data, len);
    return wrapSolid(std::move(result.shape));
}

OreoSolid oreo_import_step_file(const char* path) {
    auto result = oreo::importStepFile(*internalDefaultCtx(), path ? path : "");
    return wrapSolid(std::move(result.shape));
}

int oreo_export_step_file(OreoSolid solids[], int n, const char* path) {
    std::vector<oreo::NamedShape> shapes;
    for (int i = 0; i < n; ++i) {
        if (solids[i]) shapes.push_back(solids[i]->ns);
    }
    return oreo::exportStepFile(*internalDefaultCtx(), shapes, path ? path : "") ? 1 : 0;
}

// ============================================================
// Serialization
// ============================================================

uint8_t* oreo_serialize(OreoSolid solid, size_t* out_len) {
    if (!solid || !out_len) return nullptr;
    auto buf = oreo::serialize(*internalDefaultCtx(), solid->ns);
    if (buf.empty()) { *out_len = 0; return nullptr; }
    *out_len = buf.size();
    auto* data = new uint8_t[buf.size()];
    std::memcpy(data, buf.data(), buf.size());
    return data;
}

OreoSolid oreo_deserialize(const uint8_t* data, size_t len) {
    return wrapSolid(oreo::deserialize(*internalDefaultCtx(), data, len));
}

void oreo_free_buffer(uint8_t* buf) {
    delete[] buf;
}

// ============================================================
// Sketch
// ============================================================

OreoSketch oreo_sketch_create(void) {
    return new OreoSketch_T{};
}

void oreo_sketch_free(OreoSketch sketch) {
    delete sketch;
}

int oreo_sketch_add_point(OreoSketch sketch, double x, double y) {
    if (!sketch) { internalDefaultCtx()->diag.error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return -1; }
    sketch->points.push_back({x, y});
    return static_cast<int>(sketch->points.size()) - 1;
}

int oreo_sketch_add_line(OreoSketch sketch,
                         double x1, double y1,
                         double x2, double y2) {
    if (!sketch) { internalDefaultCtx()->diag.error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return -1; }
    sketch->lines.push_back({{x1, y1}, {x2, y2}});
    return static_cast<int>(sketch->lines.size()) - 1;
}

int oreo_sketch_add_circle(OreoSketch sketch,
                           double cx, double cy, double radius) {
    if (!sketch) { internalDefaultCtx()->diag.error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return -1; }
    sketch->circles.push_back({{cx, cy}, radius});
    return static_cast<int>(sketch->circles.size()) - 1;
}

int oreo_sketch_add_arc(OreoSketch sketch,
                        double cx, double cy,
                        double sx, double sy,
                        double ex, double ey,
                        double radius) {
    if (!sketch) { internalDefaultCtx()->diag.error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return -1; }
    sketch->arcs.push_back({{cx, cy}, {sx, sy}, {ex, ey}, radius});
    return static_cast<int>(sketch->arcs.size()) - 1;
}

int oreo_sketch_add_constraint(OreoSketch sketch,
                               int type, int entity1,
                               int entity2, int entity3,
                               double value) {
    if (!sketch) { internalDefaultCtx()->diag.error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return -1; }
    oreo::SketchConstraint c;
    c.type = static_cast<oreo::ConstraintType>(type);
    c.entity1 = entity1;
    c.entity2 = entity2;
    c.entity3 = entity3;
    c.value = value;
    sketch->constraints.push_back(c);
    return static_cast<int>(sketch->constraints.size()) - 1;
}

int oreo_sketch_solve(OreoSketch sketch) {
    if (!sketch) { internalDefaultCtx()->diag.error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return OREO_INVALID_INPUT; }
    sketch->lastResult = oreo::solveSketch(
        *internalDefaultCtx(),
        sketch->points, sketch->lines, sketch->circles,
        sketch->arcs, sketch->constraints);

    switch (sketch->lastResult.status) {
        case oreo::SolveStatus::OK:
        case oreo::SolveStatus::Underconstrained:
            return OREO_OK;
        case oreo::SolveStatus::Redundant:
            return OREO_SKETCH_REDUNDANT;
        case oreo::SolveStatus::Conflicting:
            return OREO_SKETCH_CONFLICTING;
        case oreo::SolveStatus::Failed:
        default:
            return OREO_SKETCH_SOLVE_FAILED;
    }
}

int oreo_sketch_dof(OreoSketch sketch) {
    if (!sketch) return -1;
    return sketch->lastResult.degreesOfFreedom;
}

void oreo_sketch_get_point(OreoSketch sketch, int index,
                           double* x, double* y) {
    if (!sketch || index < 0 || index >= static_cast<int>(sketch->points.size())) {
        if (x) *x = 0.0;
        if (y) *y = 0.0;
        return;
    }
    if (x) *x = sketch->points[index].x;
    if (y) *y = sketch->points[index].y;
}

void oreo_sketch_get_line(OreoSketch sketch, int index,
                          double* x1, double* y1,
                          double* x2, double* y2) {
    if (!sketch || index < 0 || index >= static_cast<int>(sketch->lines.size())) {
        if (x1) *x1 = 0.0;
        if (y1) *y1 = 0.0;
        if (x2) *x2 = 0.0;
        if (y2) *y2 = 0.0;
        return;
    }
    if (x1) *x1 = sketch->lines[index].p1.x;
    if (y1) *y1 = sketch->lines[index].p1.y;
    if (x2) *x2 = sketch->lines[index].p2.x;
    if (y2) *y2 = sketch->lines[index].p2.y;
}

OreoWire oreo_sketch_to_wire(OreoSketch sketch) {
    if (!sketch) { internalDefaultCtx()->diag.error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    oreo::NamedShape ns = oreo::sketchToWire(*internalDefaultCtx(), sketch->lines, sketch->circles, sketch->arcs);
    if (ns.isNull()) return nullptr;
    return new OreoWire_T{std::move(ns)};
}

// ============================================================
// Tessellation / Meshing
// ============================================================

struct OreoMesh_T { oreo::MeshResult mesh; };

OreoMesh oreo_tessellate(OreoSolid solid, double linear_deflection, double angular_deflection_deg) {
    if (!solid) return nullptr;
    auto result = oreo::tessellate(*internalDefaultCtx(), solid->ns, {linear_deflection, angular_deflection_deg});
    if (result.empty()) return nullptr;
    return new OreoMesh_T{std::move(result)};
}

OreoMesh oreo_ctx_tessellate(OreoContext ctx, OreoSolid solid,
                             double linear_deflection, double angular_deflection_deg) {
    if (!ctx || !solid) return nullptr;
    auto result = oreo::tessellate(*ctx->ctx, solid->ns, {linear_deflection, angular_deflection_deg});
    if (result.empty()) return nullptr;
    return new OreoMesh_T{std::move(result)};
}

void oreo_mesh_free(OreoMesh mesh) { delete mesh; }

int oreo_mesh_vertex_count(OreoMesh mesh) {
    return mesh ? mesh->mesh.totalVertices : 0;
}
int oreo_mesh_triangle_count(OreoMesh mesh) {
    return mesh ? mesh->mesh.totalTriangles : 0;
}
int oreo_mesh_face_group_count(OreoMesh mesh) {
    return mesh ? static_cast<int>(mesh->mesh.faceGroups.size()) : 0;
}
int oreo_mesh_edge_count(OreoMesh mesh) {
    return mesh ? mesh->mesh.totalEdges : 0;
}

const float* oreo_mesh_positions(OreoMesh mesh) {
    return (mesh && !mesh->mesh.positions.empty()) ? mesh->mesh.positions.data() : nullptr;
}
const float* oreo_mesh_normals(OreoMesh mesh) {
    return (mesh && !mesh->mesh.normals.empty()) ? mesh->mesh.normals.data() : nullptr;
}
const uint32_t* oreo_mesh_indices(OreoMesh mesh) {
    return (mesh && !mesh->mesh.indices.empty()) ? mesh->mesh.indices.data() : nullptr;
}

int oreo_mesh_face_group_id(OreoMesh mesh, int group) {
    if (!mesh || group < 0 || group >= (int)mesh->mesh.faceGroups.size()) return 0;
    return mesh->mesh.faceGroups[group].faceId;
}
const char* oreo_mesh_face_group_name(OreoMesh mesh, int group) {
    if (!mesh || group < 0 || group >= (int)mesh->mesh.faceGroups.size()) return "";
    return mesh->mesh.faceGroups[group].faceName.c_str();
}
int oreo_mesh_face_group_index_start(OreoMesh mesh, int group) {
    if (!mesh || group < 0 || group >= (int)mesh->mesh.faceGroups.size()) return 0;
    return static_cast<int>(mesh->mesh.faceGroups[group].indexStart);
}
int oreo_mesh_face_group_index_count(OreoMesh mesh, int group) {
    if (!mesh || group < 0 || group >= (int)mesh->mesh.faceGroups.size()) return 0;
    return static_cast<int>(mesh->mesh.faceGroups[group].indexCount);
}

// ============================================================
// Memory management
// ============================================================

void oreo_free_solid(OreoSolid solid) { delete solid; }
void oreo_free_wire(OreoWire wire)   { delete wire; }
void oreo_free_edge(OreoEdge edge)   { delete edge; }
void oreo_free_face(OreoFace face)   { delete face; }

} // extern "C"
