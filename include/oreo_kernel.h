// oreo_kernel.h — Public C API for liboreo-kernel.
// This is the ONLY header a consumer needs. Flat C interface, FFI-friendly.
//
// Usage:
//   oreo_init();
//   OreoSolid box = oreo_make_box(10, 20, 30);
//   OreoSolid extruded = oreo_extrude(box, 0, 0, 1, 50);
//   OreoSolid filleted = oreo_fillet(extruded, edges, n, 2.0);
//   oreo_export_step_file(&filleted, 1, "output.step");
//   oreo_free_solid(filleted);
//   oreo_free_solid(extruded);
//   oreo_free_solid(box);
//   oreo_shutdown();

#ifndef OREO_KERNEL_H
#define OREO_KERNEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- DLL export ---
#if defined(_WIN32)
    #ifdef OREO_KERNEL_EXPORTS
        #define OREO_API __declspec(dllexport)
    #else
        #define OREO_API __declspec(dllimport)
    #endif
#elif __GNUC__ >= 4
    #define OREO_API __attribute__((visibility("default")))
#else
    #define OREO_API
#endif

// --- Opaque handle types ---
typedef struct OreoContext_T* OreoContext;
typedef struct OreoSolid_T*   OreoSolid;
typedef struct OreoWire_T*    OreoWire;
typedef struct OreoEdge_T*    OreoEdge;
typedef struct OreoFace_T*    OreoFace;
typedef struct OreoSketch_T*  OreoSketch;

// --- Error codes ---
typedef enum {
    OREO_OK = 0,
    OREO_INVALID_INPUT,
    OREO_OCCT_FAILURE,
    OREO_BOOLEAN_FAILED,
    OREO_SHAPE_INVALID,
    OREO_SHAPE_FIX_FAILED,
    OREO_SKETCH_SOLVE_FAILED,
    OREO_SKETCH_REDUNDANT,
    OREO_SKETCH_CONFLICTING,
    OREO_STEP_IMPORT_FAILED,
    OREO_STEP_EXPORT_FAILED,
    OREO_SERIALIZE_FAILED,
    OREO_DESERIALIZE_FAILED,
    OREO_NOT_INITIALIZED,
    OREO_INTERNAL_ERROR,
} OreoErrorCode;

// --- Structured error ---
typedef struct {
    OreoErrorCode code;
    const char* message;
    const char* entity;
    const char* suggestion;
} OreoError;

// --- Bounding box ---
typedef struct {
    double xmin, ymin, zmin;
    double xmax, ymax, zmax;
} OreoBBox;

// --- Mass properties ---
typedef struct {
    double volume;
    double surface_area;
    double com_x, com_y, com_z;
    double ixx, iyy, izz, ixy, ixz, iyz;
} OreoMassProps;

// ============================================================
// Context — the kernel's central authority
// ============================================================

// Create a new kernel context. Each context has its own:
//   - Deterministic tag allocator (no cross-document collisions)
//   - Diagnostic collector (errors + warnings per operation)
//   - Tolerance policy
//   - Unit system
// Use one context per document. Thread safety: one context per thread.
OREO_API OreoContext oreo_context_create(void);
OREO_API void       oreo_context_free(OreoContext ctx);

// Create a context bound to a specific document identity.
//
//   documentId    — 64-bit document identifier. Must be unique within the
//                   process. 0 means "single-document / default mode" (same
//                   as oreo_context_create).
//   documentUUID  — optional; if non-NULL and non-empty, documentId is
//                   derived from it via SipHash-2-4 and the `documentId`
//                   argument is ignored.
//
// Returns NULL on failure (including a collision on the low-32 bits with
// an existing full-64-bit documentId — query oreo_last_error via a temp
// context for details).
OREO_API OreoContext oreo_context_create_with_doc(
    uint64_t documentId,
    const char* documentUUID);

// Query the full 64-bit documentId of an existing context. Returns 0 for a
// null handle or a context created in single-document mode.
OREO_API uint64_t oreo_context_document_id(OreoContext ctx);

// Get diagnostics from the context (replaces oreo_last_error for new code)
OREO_API int        oreo_context_has_errors(OreoContext ctx);
OREO_API int        oreo_context_has_warnings(OreoContext ctx);
OREO_API int        oreo_context_error_count(OreoContext ctx);
OREO_API const char* oreo_context_last_error_message(OreoContext ctx);

// Context-aware primitive creation
OREO_API OreoSolid oreo_ctx_make_box(OreoContext ctx, double dx, double dy, double dz);
OREO_API OreoSolid oreo_ctx_make_cylinder(OreoContext ctx, double radius, double height);
OREO_API OreoSolid oreo_ctx_make_sphere(OreoContext ctx, double radius);

// Context-aware geometry operations
OREO_API OreoSolid oreo_ctx_extrude(OreoContext ctx, OreoSolid base, double dx, double dy, double dz);
OREO_API OreoSolid oreo_ctx_boolean_union(OreoContext ctx, OreoSolid a, OreoSolid b);
OREO_API OreoSolid oreo_ctx_boolean_subtract(OreoContext ctx, OreoSolid target, OreoSolid tool);
OREO_API OreoSolid oreo_ctx_fillet(OreoContext ctx, OreoSolid solid, OreoEdge edges[], int n, double radius);

// ============================================================
// Legacy singleton-context C API
// ============================================================
//
// Everything from here until the "Memory management" section at the bottom
// uses a PROCESS-GLOBAL KernelContext. It is NOT safe for multi-tenant
// server processes — concurrent requests would share diagnostic state,
// tolerance config, and the tag allocator.
//
// These declarations are only exposed when the build was configured with
// -DOREO_ENABLE_LEGACY_API=ON (the default). Server builds should set the
// option OFF; production code should use the oreo_ctx_* functions above
// with an explicit OreoContext per document/request.
//
#ifdef OREO_ENABLE_LEGACY_API

// ─── Lifecycle (legacy — uses an internal default context) ─────────

// oreo_init() initializes OCCT and creates the internal context.
// oreo_shutdown() resets and re-creates the internal context.
// oreo_last_error() queries the internal context's diagnostic collector.
OREO_API void oreo_init(void);
OREO_API void oreo_shutdown(void);
OREO_API OreoError oreo_last_error(void);

// ─── Primitive creation (convenience) ──────────────────────────────

OREO_API OreoSolid oreo_make_box(double dx, double dy, double dz);
OREO_API OreoSolid oreo_make_cylinder(double radius, double height);
OREO_API OreoSolid oreo_make_sphere(double radius);
OREO_API OreoSolid oreo_make_cone(double r1, double r2, double height);
OREO_API OreoSolid oreo_make_torus(double major_r, double minor_r);
OREO_API OreoSolid oreo_make_wedge(double dx, double dy, double dz, double ltx);

// ============================================================
// Geometry operations
// ============================================================

OREO_API OreoSolid oreo_extrude(OreoSolid base, double dx, double dy, double dz);
OREO_API OreoSolid oreo_revolve(OreoSolid base,
                                 double ax, double ay, double az,  // axis point
                                 double dx, double dy, double dz,  // axis direction
                                 double angle_rad);
OREO_API OreoSolid oreo_fillet(OreoSolid solid, OreoEdge edges[], int n, double radius);
OREO_API OreoSolid oreo_chamfer(OreoSolid solid, OreoEdge edges[], int n, double distance);
OREO_API OreoSolid oreo_boolean_union(OreoSolid a, OreoSolid b);
OREO_API OreoSolid oreo_boolean_subtract(OreoSolid target, OreoSolid tool);
OREO_API OreoSolid oreo_boolean_intersect(OreoSolid a, OreoSolid b);
OREO_API OreoSolid oreo_shell(OreoSolid solid, OreoFace faces[], int n, double thickness);
OREO_API OreoSolid oreo_loft(OreoWire profiles[], int n, int make_solid);
OREO_API OreoSolid oreo_sweep(OreoWire profile, OreoWire path);
OREO_API OreoSolid oreo_mirror(OreoSolid solid,
                                double px, double py, double pz,  // plane point
                                double nx, double ny, double nz); // plane normal
OREO_API OreoSolid oreo_pattern_linear(OreoSolid solid,
                                        double dx, double dy, double dz,
                                        int count, double spacing);
OREO_API OreoSolid oreo_pattern_circular(OreoSolid solid,
                                          double ax, double ay, double az,
                                          double dx, double dy, double dz,
                                          int count, double total_angle_rad);

// ============================================================
// Manufacturing operations
// ============================================================

OREO_API OreoSolid oreo_draft(OreoSolid solid, OreoFace faces[], int n,
                               double angle_deg,
                               double pull_dx, double pull_dy, double pull_dz);
OREO_API OreoSolid oreo_hole(OreoSolid solid, OreoFace face,
                              double cx, double cy, double cz,
                              double diameter, double depth);
OREO_API OreoSolid oreo_pocket(OreoSolid solid, OreoSolid profile, double depth);
OREO_API OreoSolid oreo_rib(OreoSolid solid, OreoSolid profile,
                             double dx, double dy, double dz, double thickness);

// ============================================================
// Surface / face operations
// ============================================================

OREO_API OreoSolid oreo_offset(OreoSolid solid, double distance);
OREO_API OreoSolid oreo_thicken(OreoSolid shell, double thickness);
OREO_API OreoSolid oreo_split_body(OreoSolid solid,
                                    double px, double py, double pz,
                                    double nx, double ny, double nz);
OREO_API OreoSolid oreo_fillet_variable(OreoSolid solid, OreoEdge edge,
                                         double start_radius, double end_radius);
OREO_API OreoSolid oreo_make_face_from_wire(OreoWire wire);
OREO_API OreoSolid oreo_combine(OreoSolid shapes[], int n);

// ============================================================
// Queries
// ============================================================

OREO_API OreoBBox oreo_aabb(OreoSolid solid);
OREO_API OreoBBox oreo_footprint(OreoSolid before, OreoSolid after);
OREO_API int      oreo_face_count(OreoSolid solid);
OREO_API int      oreo_edge_count(OreoSolid solid);
OREO_API OreoFace oreo_get_face(OreoSolid solid, int index);  // 1-based
OREO_API OreoEdge oreo_get_edge(OreoSolid solid, int index);  // 1-based
OREO_API double   oreo_measure_distance(OreoSolid a, OreoSolid b);
OREO_API OreoMassProps oreo_mass_properties(OreoSolid solid);

// ============================================================
// Element naming
// ============================================================

// Get the stable name for a face/edge by index (1-based).
// Returns a string owned by the kernel — do not free.
OREO_API const char* oreo_face_name(OreoSolid solid, int index);
OREO_API const char* oreo_edge_name(OreoSolid solid, int index);

// ============================================================
// I/O
// ============================================================

OREO_API OreoSolid oreo_import_step(const uint8_t* data, size_t len);
OREO_API OreoSolid oreo_import_step_file(const char* path);
OREO_API int       oreo_export_step_file(OreoSolid solids[], int n, const char* path);

// ============================================================
// Serialization
// ============================================================

// Serialize returns a buffer. Caller must free with oreo_free_buffer.
OREO_API uint8_t* oreo_serialize(OreoSolid solid, size_t* out_len);
OREO_API OreoSolid oreo_deserialize(const uint8_t* data, size_t len);
OREO_API void oreo_free_buffer(uint8_t* buf);

// ============================================================
// Sketch
// ============================================================

// Sketch creation / destruction
OREO_API OreoSketch oreo_sketch_create(void);
OREO_API void       oreo_sketch_free(OreoSketch sketch);

// Add entities — each returns the 0-based index of the added entity
OREO_API int oreo_sketch_add_point(OreoSketch sketch, double x, double y);
OREO_API int oreo_sketch_add_line(OreoSketch sketch,
                                  double x1, double y1,
                                  double x2, double y2);
OREO_API int oreo_sketch_add_circle(OreoSketch sketch,
                                    double cx, double cy, double radius);
OREO_API int oreo_sketch_add_arc(OreoSketch sketch,
                                 double cx, double cy,
                                 double sx, double sy,
                                 double ex, double ey,
                                 double radius);

// Add a constraint.
//   type    — one of ConstraintType (cast to int)
//   entity1 — first entity index
//   entity2 — second entity index (or -1 if unused)
//   entity3 — third entity index  (or -1 if unused)
//   value   — dimension value (distance, angle, radius; 0 if unused)
// Returns the 0-based index of the added constraint.
OREO_API int oreo_sketch_add_constraint(OreoSketch sketch,
                                        int type, int entity1,
                                        int entity2, int entity3,
                                        double value);

// Solve the sketch. Returns an OreoErrorCode:
//   OREO_OK                  — solved successfully
//   OREO_SKETCH_REDUNDANT    — over-constrained (redundant)
//   OREO_SKETCH_CONFLICTING  — conflicting constraints
//   OREO_SKETCH_SOLVE_FAILED — solver did not converge
OREO_API int oreo_sketch_solve(OreoSketch sketch);

// Degrees of freedom remaining after the last solve.
OREO_API int oreo_sketch_dof(OreoSketch sketch);

// Get solved positions (call after oreo_sketch_solve)
OREO_API void oreo_sketch_get_point(OreoSketch sketch, int index,
                                    double* x, double* y);
OREO_API void oreo_sketch_get_line(OreoSketch sketch, int index,
                                   double* x1, double* y1,
                                   double* x2, double* y2);

// Convert the solved sketch to a wire (lines + circles + arcs)
OREO_API OreoWire oreo_sketch_to_wire(OreoSketch sketch);

#endif // OREO_ENABLE_LEGACY_API

// ============================================================
// Tessellation / Meshing
// ============================================================

typedef struct OreoMesh_T* OreoMesh;

// Tessellate a shape into a triangle mesh.
// linear_deflection: chord tolerance in mm (typical: 0.1)
// angular_deflection_deg: angular tolerance in degrees (typical: 20)
#ifdef OREO_ENABLE_LEGACY_API
OREO_API OreoMesh oreo_tessellate(OreoSolid solid,
                                   double linear_deflection,
                                   double angular_deflection_deg);
#endif
OREO_API OreoMesh oreo_ctx_tessellate(OreoContext ctx, OreoSolid solid,
                                       double linear_deflection,
                                       double angular_deflection_deg);
OREO_API void oreo_mesh_free(OreoMesh mesh);

// Mesh data access (buffers owned by OreoMesh — do not free)
OREO_API int            oreo_mesh_vertex_count(OreoMesh mesh);
OREO_API int            oreo_mesh_triangle_count(OreoMesh mesh);
OREO_API int            oreo_mesh_face_group_count(OreoMesh mesh);
OREO_API int            oreo_mesh_edge_count(OreoMesh mesh);
OREO_API const float*   oreo_mesh_positions(OreoMesh mesh);
OREO_API const float*   oreo_mesh_normals(OreoMesh mesh);
OREO_API const uint32_t* oreo_mesh_indices(OreoMesh mesh);

// Per-face group access
OREO_API int         oreo_mesh_face_group_id(OreoMesh mesh, int group);
OREO_API const char* oreo_mesh_face_group_name(OreoMesh mesh, int group);
OREO_API int         oreo_mesh_face_group_index_start(OreoMesh mesh, int group);
OREO_API int         oreo_mesh_face_group_index_count(OreoMesh mesh, int group);

// ============================================================
// Memory management
// ============================================================

OREO_API void oreo_free_solid(OreoSolid solid);
OREO_API void oreo_free_wire(OreoWire wire);
OREO_API void oreo_free_edge(OreoEdge edge);
OREO_API void oreo_free_face(OreoFace face);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // OREO_KERNEL_H
