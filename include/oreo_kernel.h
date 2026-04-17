// SPDX-License-Identifier: LGPL-2.1-or-later

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
//
// Three linkage modes are supported:
//   OREO_KERNEL_STATIC  — define when consuming oreo-kernel as a static
//                         archive (e.g. the internal test binaries linking
//                         against oreo-kernel-internal). OREO_API expands
//                         to nothing — no dllexport/dllimport mismatch.
//   OREO_KERNEL_EXPORTS — define when COMPILING the SHARED oreo-kernel.dll.
//                         OREO_API expands to dllexport.
//   (neither defined)   — default: consuming the SHARED oreo-kernel.dll
//                         from a downstream binary. OREO_API expands to
//                         dllimport.
#if defined(_WIN32)
    #if defined(OREO_KERNEL_STATIC)
        #define OREO_API
    #elif defined(OREO_KERNEL_EXPORTS)
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
    OREO_INVALID_INPUT = 1,
    OREO_OCCT_FAILURE = 2,
    OREO_BOOLEAN_FAILED = 3,
    OREO_SHAPE_INVALID = 4,
    OREO_SHAPE_FIX_FAILED = 5,
    OREO_SKETCH_SOLVE_FAILED = 6,
    OREO_SKETCH_REDUNDANT = 7,
    OREO_SKETCH_CONFLICTING = 8,
    OREO_STEP_IMPORT_FAILED = 10,
    OREO_STEP_EXPORT_FAILED = 11,
    OREO_SERIALIZE_FAILED = 12,
    OREO_DESERIALIZE_FAILED = 13,
    // Identity v2 additions (docs/identity-model.md §8 Q2).
    OREO_LEGACY_IDENTITY_DOWNGRADE = 14,     // Warning: v1 tag read, high docId bits inferred/lost
    OREO_V2_IDENTITY_NOT_REPRESENTABLE = 15, // v2 identity can't fit v1 scalar format
    OREO_MALFORMED_ELEMENT_NAME = 16,        // ;:P / ;:H payload could not be parsed
    OREO_BUFFER_TOO_SMALL = 17,              // Caller-supplied output buffer too small
    OREO_NOT_INITIALIZED = 20,
    OREO_INTERNAL_ERROR = 21,
    OREO_NOT_SUPPORTED = 100,
    OREO_INVALID_STATE = 101,
    OREO_OUT_OF_RANGE = 102,
    OREO_TIMEOUT = 103,
    OREO_CANCELLED = 104,
    OREO_RESOURCE_EXHAUSTED = 105,
    OREO_DIAG_TRUNCATED = 200,
} OreoErrorCode;

// --- Structured error ---
typedef struct {
    OreoErrorCode code;
    const char* message;
    const char* entity;
    const char* suggestion;
} OreoError;

// --- Shape identity (v2 hardening; see docs/identity-model.md §6.1) ---
// A full 64+64 identity value: {documentId, counter}. Replaces the
// squeezed int64 tag on the FFI boundary.
typedef struct {
    uint64_t document_id;
    uint64_t counter;
} OreoShapeId;

// ABI layout checks. Guarded for C89/C99 consumers — the kernel's own
// TUs compile as C11+, so the invariant is still enforced inside the
// library. See docs/identity-model.md §6.1.
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(OreoShapeId) == 16, "OreoShapeId ABI: size 16");
_Static_assert(_Alignof(OreoShapeId) == 8, "OreoShapeId ABI: align 8");
#endif

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
// Library identity
// ============================================================

// Returns the kernel version string. Format is SemVer with optional
// pre-release suffix, e.g. "0.9.0-rc1" or "1.2.3". Returned pointer
// is owned by the kernel — do not free. Always non-null.
//
// For programmatic SemVer comparison, prefer parsing into major/minor/
// patch yourself; the suffix (everything after the first '-') marks
// pre-release builds and should be treated as "less than" the same
// X.Y.Z without a suffix.
OREO_API const char* oreo_kernel_version(void);

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
OREO_API int        oreo_context_warning_count(OreoContext ctx);
OREO_API int        oreo_context_diagnostic_count(OreoContext ctx);
OREO_API int        oreo_context_diagnostic_code(OreoContext ctx, int index);
OREO_API const char* oreo_context_diagnostic_message(OreoContext ctx, int index);
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
// v2 identity-aware C API (Phase 5 — always available, no legacy gate)
// ============================================================
//
// Thread-safety: one OreoContext per thread. Concurrent calls with the
// same ctx are undefined. Concurrent calls with DIFFERENT ctx arguments
// are safe.

// Query a face or edge's ShapeIdentity by 1-based index. On success
// returns OREO_OK and writes the identity to *out. On failure (null
// handle, out-of-range index, missing element map, or docId/scalar
// mismatch in a legacy-format read) returns the relevant OreoErrorCode
// and sets *out to {0, 0}.
OREO_API int oreo_face_shape_id(OreoSolid solid, int index, OreoShapeId* out);
OREO_API int oreo_edge_shape_id(OreoSolid solid, int index, OreoShapeId* out);

// Retrieve a face's or edge's stable name into a caller-owned buffer.
// Size-probe protocol: pass (buf=NULL, buflen=0, needed=&n). On return,
// *needed holds the number of bytes required (excluding NUL terminator).
// Then allocate buflen >= *needed + 1 and call again.
//
// Returns:
//   OREO_OK                — name fully written, NUL-terminated.
//   OREO_BUFFER_TOO_SMALL  — buflen < *needed + 1; buf contains the
//                            truncated prefix + NUL if buflen > 0.
//   OREO_INVALID_INPUT     — null handle or negative/zero index.
OREO_API int oreo_ctx_face_name(OreoContext ctx, OreoSolid solid, int index,
                                 char* buf, size_t buflen, size_t* needed);
OREO_API int oreo_ctx_edge_name(OreoContext ctx, OreoSolid solid, int index,
                                 char* buf, size_t buflen, size_t* needed);

// Serialize a shape to a caller-owned buffer using the v3 on-disk
// format. Size-probe protocol as above: pass (buf=NULL, buflen=0,
// needed=&n) to measure, then allocate and re-call. No thread-local
// state, safe for concurrent use on distinct contexts.
OREO_API int oreo_ctx_serialize(OreoContext ctx, OreoSolid solid,
                                 uint8_t* buf, size_t buflen,
                                 size_t* needed);

// Deserialize a shape from a byte buffer. Accepts v3 (current) and
// v1 (legacy) wire formats. Returns NULL on failure; call
// oreo_context_last_error_message for details.
OREO_API OreoSolid oreo_ctx_deserialize(OreoContext ctx,
                                         const uint8_t* data, size_t len);

// Context-aware query APIs for server/no-legacy builds.
OREO_API OreoBBox oreo_ctx_aabb(OreoContext ctx, OreoSolid solid);
OREO_API int      oreo_ctx_face_count(OreoContext ctx, OreoSolid solid);
OREO_API int      oreo_ctx_edge_count(OreoContext ctx, OreoSolid solid);
OREO_API OreoMassProps oreo_ctx_mass_properties(OreoContext ctx, OreoSolid solid);

// Context-aware STEP APIs for server/no-legacy builds.
OREO_API OreoSolid oreo_ctx_import_step(OreoContext ctx, const uint8_t* data, size_t len);
OREO_API OreoSolid oreo_ctx_import_step_file(OreoContext ctx, const char* path);
OREO_API int       oreo_ctx_export_step_file(OreoContext ctx, OreoSolid solids[], int n, const char* path);

// ============================================================
// Context-aware sketch API (always available, no legacy gate)
// ============================================================
//
// A sketch handle created via oreo_ctx_sketch_create is bound to the
// context passed in, so subsequent solve / to_wire calls route their
// diagnostics through that context. The handle itself is plain data —
// add_point / add_line / get_* are pure mutators on the sketch's
// vectors and never block on the ctx (errors still route via the
// stored binding).
//
// Lifetime: the bound context must outlive the sketch handle.
OREO_API OreoSketch oreo_ctx_sketch_create(OreoContext ctx);
OREO_API void       oreo_ctx_sketch_free(OreoSketch sketch);
OREO_API int oreo_ctx_sketch_add_point(OreoSketch sketch, double x, double y);
OREO_API int oreo_ctx_sketch_add_line(OreoSketch sketch,
                                       double x1, double y1,
                                       double x2, double y2);
OREO_API int oreo_ctx_sketch_add_circle(OreoSketch sketch,
                                         double cx, double cy, double radius);
OREO_API int oreo_ctx_sketch_add_arc(OreoSketch sketch,
                                      double cx, double cy,
                                      double sx, double sy,
                                      double ex, double ey,
                                      double radius);
OREO_API int oreo_ctx_sketch_add_constraint(OreoSketch sketch,
                                             int type, int entity1,
                                             int entity2, int entity3,
                                             double value);
OREO_API int oreo_ctx_sketch_solve(OreoSketch sketch);
OREO_API int oreo_ctx_sketch_dof(OreoSketch sketch);
OREO_API int oreo_ctx_sketch_get_point(OreoSketch sketch, int index,
                                        double* x, double* y);
OREO_API int oreo_ctx_sketch_get_line(OreoSketch sketch, int index,
                                       double* x1, double* y1,
                                       double* x2, double* y2);
OREO_API OreoWire oreo_ctx_sketch_to_wire(OreoSketch sketch);

// ─── Persistent sketch entity IDs ──────────────────────────────
//
// Every oreo_ctx_sketch_add_* function returns a stable, integer
// entity ID — specifically, the slot index assigned at add time.
// IDs are NEVER reused, even after the entity is removed. A removed
// entity becomes a tombstone in its slot so subsequent add_* calls
// return strictly increasing IDs and existing references in cached
// constraints, feature trees, or external storage stay valid as
// "this used to point to a thing that no longer exists".
//
// The cloud-collab edit model (see docs/part-studio-model.md and
// docs/server-safe-api.md) depends on this contract: a server must
// be able to apply (insert / delete / update) operations to a sketch
// stored on disk and replay them later in any order without renaming.
//
// Removal cascades: when an entity is removed, every constraint that
// referenced it is automatically removed too. The constraint slot
// stays alive (as a tombstone) so the user can re-target it.

// Remove an entity by ID. Cascades to dependent constraints.
// Returns OREO_OK on success, OREO_INVALID_INPUT on null handle /
// out-of-range / already-removed.
OREO_API int oreo_ctx_sketch_remove_point(OreoSketch sketch, int pointId);
OREO_API int oreo_ctx_sketch_remove_line(OreoSketch sketch, int lineId);
OREO_API int oreo_ctx_sketch_remove_circle(OreoSketch sketch, int circleId);
OREO_API int oreo_ctx_sketch_remove_arc(OreoSketch sketch, int arcId);

// Remove a constraint by ID (no cascade — constraints don't own
// entities). Returns OREO_OK / OREO_INVALID_INPUT.
OREO_API int oreo_ctx_sketch_remove_constraint(OreoSketch sketch,
                                                int constraintId);

// Per-family liveness probe. Returns 1 if the slot exists and is
// not a tombstone, 0 otherwise (out-of-range or removed).
OREO_API int oreo_ctx_sketch_point_alive     (OreoSketch sketch, int id);
OREO_API int oreo_ctx_sketch_line_alive      (OreoSketch sketch, int id);
OREO_API int oreo_ctx_sketch_circle_alive    (OreoSketch sketch, int id);
OREO_API int oreo_ctx_sketch_arc_alive       (OreoSketch sketch, int id);
OREO_API int oreo_ctx_sketch_constraint_alive(OreoSketch sketch, int id);

// Slot count = number of slots ever allocated (live + tombstone).
// Use for `for (id = 0; id < slot_count; ++id)` iteration with an
// _alive() check. The slot count never decreases.
OREO_API int oreo_ctx_sketch_point_slot_count     (OreoSketch sketch);
OREO_API int oreo_ctx_sketch_line_slot_count      (OreoSketch sketch);
OREO_API int oreo_ctx_sketch_circle_slot_count    (OreoSketch sketch);
OREO_API int oreo_ctx_sketch_arc_slot_count       (OreoSketch sketch);
OREO_API int oreo_ctx_sketch_constraint_slot_count(OreoSketch sketch);

// Live count = slot count minus tombstones. Useful for UI badges
// and quota checks.
OREO_API int oreo_ctx_sketch_point_live_count     (OreoSketch sketch);
OREO_API int oreo_ctx_sketch_line_live_count      (OreoSketch sketch);
OREO_API int oreo_ctx_sketch_circle_live_count    (OreoSketch sketch);
OREO_API int oreo_ctx_sketch_arc_live_count       (OreoSketch sketch);
OREO_API int oreo_ctx_sketch_constraint_live_count(OreoSketch sketch);

// ============================================================
// Context-aware feature-edit API (always available, no legacy gate)
// ============================================================
//
// Two-handle model:
//   OreoFeatureBuilder accumulates {id, type, params} in a fluent API,
//   OreoFeatureTree owns an ordered sequence of features bound to a ctx.
//
// To add a feature: build it via the setters, then hand it to
// oreo_ctx_feature_tree_add. The tree takes ownership of the resulting
// Feature; the builder remains usable (it can be reset and reused).
//
// All param setters return OREO_OK on success or OREO_INVALID_INPUT on
// a null handle / null name. Schema-level validation runs at replay
// time (executeFeature) and at the dedicated _validate entry point.

typedef struct OreoFeatureBuilder_T* OreoFeatureBuilder;
typedef struct OreoFeatureTree_T*    OreoFeatureTree;

OREO_API OreoFeatureBuilder oreo_feature_builder_create(const char* id, const char* type);
OREO_API void               oreo_feature_builder_free(OreoFeatureBuilder b);
OREO_API void               oreo_feature_builder_reset(OreoFeatureBuilder b,
                                                        const char* id, const char* type);

OREO_API int oreo_feature_builder_set_double(OreoFeatureBuilder b, const char* name, double v);
OREO_API int oreo_feature_builder_set_int   (OreoFeatureBuilder b, const char* name, int v);
OREO_API int oreo_feature_builder_set_bool  (OreoFeatureBuilder b, const char* name, int v);
OREO_API int oreo_feature_builder_set_string(OreoFeatureBuilder b, const char* name, const char* v);
OREO_API int oreo_feature_builder_set_vec   (OreoFeatureBuilder b, const char* name,
                                              double x, double y, double z);
OREO_API int oreo_feature_builder_set_pnt   (OreoFeatureBuilder b, const char* name,
                                              double x, double y, double z);
OREO_API int oreo_feature_builder_set_dir   (OreoFeatureBuilder b, const char* name,
                                              double x, double y, double z);
OREO_API int oreo_feature_builder_set_ax1   (OreoFeatureBuilder b, const char* name,
                                              double px, double py, double pz,
                                              double dx, double dy, double dz);
OREO_API int oreo_feature_builder_set_ax2   (OreoFeatureBuilder b, const char* name,
                                              double px, double py, double pz,
                                              double nx, double ny, double nz);
OREO_API int oreo_feature_builder_set_pln   (OreoFeatureBuilder b, const char* name,
                                              double px, double py, double pz,
                                              double nx, double ny, double nz);

// Set a single ElementRef parameter (overwrites). Use for params like
// "tool" or "edge" / "face" that take a single sub-shape reference.
OREO_API int oreo_feature_builder_set_ref(OreoFeatureBuilder b, const char* name,
                                           const char* featureId,
                                           const char* elementName,
                                           const char* elementType);

// Append an ElementRef to a list-typed parameter (creates the list on
// first call). Use for params like "edges" / "faces" / "shapes".
OREO_API int oreo_feature_builder_add_ref(OreoFeatureBuilder b, const char* name,
                                           const char* featureId,
                                           const char* elementName,
                                           const char* elementType);

// ─── Tree ─────────────────────────────────────────────────────

OREO_API OreoFeatureTree oreo_ctx_feature_tree_create(OreoContext ctx);
OREO_API void            oreo_ctx_feature_tree_free(OreoFeatureTree t);

// Snapshot the builder's current state into a new feature appended at
// the end of the tree. The builder is not modified.
// Returns OREO_OK or an OreoErrorCode on failure (null args, duplicate ID).
OREO_API int oreo_ctx_feature_tree_add(OreoFeatureTree t, OreoFeatureBuilder b);

// Remove a feature by ID. Returns OREO_OK or OREO_INVALID_INPUT.
OREO_API int oreo_ctx_feature_tree_remove(OreoFeatureTree t, const char* featureId);

// Suppress / unsuppress a feature by ID. suppress != 0 means suppressed.
OREO_API int oreo_ctx_feature_tree_suppress(OreoFeatureTree t,
                                             const char* featureId, int suppress);

// Update a single double parameter on an existing feature. Use for
// fast parametric edits ("change radius from 2.0 to 2.5").
OREO_API int oreo_ctx_feature_tree_set_param_double(OreoFeatureTree t,
                                                     const char* featureId,
                                                     const char* paramName,
                                                     double v);

// Per-type parameter updates for the remaining ParamValue alternatives.
// Mirrors oreo_feature_builder_set_*. All return OREO_OK on success or
// OreoErrorCode on a missing feature / null arg.
OREO_API int oreo_ctx_feature_tree_set_param_int   (OreoFeatureTree t,
                                                     const char* featureId,
                                                     const char* paramName,
                                                     int v);
OREO_API int oreo_ctx_feature_tree_set_param_bool  (OreoFeatureTree t,
                                                     const char* featureId,
                                                     const char* paramName,
                                                     int v);
OREO_API int oreo_ctx_feature_tree_set_param_string(OreoFeatureTree t,
                                                     const char* featureId,
                                                     const char* paramName,
                                                     const char* v);
OREO_API int oreo_ctx_feature_tree_set_param_vec   (OreoFeatureTree t,
                                                     const char* featureId,
                                                     const char* paramName,
                                                     double x, double y, double z);

OREO_API int oreo_ctx_feature_tree_count(OreoFeatureTree t);

// ─── Topology-change query ─────────────────────────────────────
//
// After replay, callers (especially cloud collaboration servers)
// often need to know which features failed or whose topology changed
// — typically because a downstream feature's element references no
// longer resolve. The "broken" query returns the ordered list of
// feature IDs with a non-OK status (BrokenReference, ExecutionFailed).

// Number of features whose status is not OK after the most recent
// replay. Returns 0 if the tree has never been replayed or all
// features succeeded.
OREO_API int oreo_ctx_feature_tree_broken_count(OreoFeatureTree t);

// Read the featureId of the i-th broken feature (0-based index).
// Uses the size-probe protocol: pass (buf=NULL, buflen=0, needed=&n)
// to measure, then allocate buflen >= *needed + 1 and call again.
OREO_API int oreo_ctx_feature_tree_broken_id(OreoFeatureTree t, int index,
                                              char* buf, size_t buflen,
                                              size_t* needed);

// Read the human-readable error message of the i-th broken feature
// (the same string available via Feature::errorMessage). Size-probe
// protocol identical to broken_id.
OREO_API int oreo_ctx_feature_tree_broken_message(OreoFeatureTree t, int index,
                                                   char* buf, size_t buflen,
                                                   size_t* needed);

// ─── Reorder / move ─────────────────────────────────────────────
//
// Move a feature to a new position in the ordered list. Negative
// values clamp to 0; values past the end clamp to the end. Marks the
// moved feature and everything downstream of EITHER the old or new
// position as dirty so the next replay rebuilds them.
OREO_API int oreo_ctx_feature_tree_move(OreoFeatureTree t,
                                         const char* featureId,
                                         int newIndex);

// Replace every ElementRef in every feature whose featureId matches
// `oldFeatureId` with the same ref pointing at `newFeatureId`. Useful
// for "swap base feature" edits where downstream references should
// follow the swap. Returns the number of refs updated.
OREO_API int oreo_ctx_feature_tree_replace_reference(OreoFeatureTree t,
                                                      const char* oldFeatureId,
                                                      const char* newFeatureId);

// Run schema-level parameter validation on every feature in the tree
// without executing geometry. Returns OREO_OK if all features validate,
// or the first error code encountered. Diagnostics for every failing
// feature are written to the bound ctx.
OREO_API int oreo_ctx_feature_tree_validate(OreoFeatureTree t);

// Replay the entire tree and return the final solid (caller owns the
// returned handle and must oreo_free_solid it). Returns NULL on
// failure; query the bound ctx for diagnostics.
OREO_API OreoSolid oreo_ctx_feature_tree_replay(OreoFeatureTree t);

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
