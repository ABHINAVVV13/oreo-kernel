// oreo_capi.cpp — C API wrapper over C++ internals.

#include "oreo_kernel.h"

#include "core/kernel_context.h"
#include "core/diagnostic.h"
#include "core/shape_identity.h"
#include "core/shape_identity_v1.h"
#include "geometry/oreo_geometry.h"
#include "io/oreo_step.h"
#include "io/oreo_serialize.h"
#include "naming/element_map.h"
#include "naming/named_shape.h"
#include "query/oreo_query.h"
#include "sketch/oreo_sketch.h"
#include "mesh/oreo_mesh.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS.hxx>

#include <cstring>
#include <memory>
#include <string>

// ============================================================
// Exception-boundary macros for extern "C" functions.
// Every extern "C" function body MUST be wrapped with these
// to prevent C++ exceptions from escaping to C callers (UB).
// ============================================================

#define OREO_C_TRY try {

#define OREO_C_CATCH_RETURN(default_val) \
    } catch (const Standard_Failure& e) { \
        internalDefaultCtx()->diag().error(oreo::ErrorCode::OCCT_FAILURE, \
            std::string("OCCT exception: ") + e.GetMessageString()); \
        return default_val; \
    } catch (const std::exception& e) { \
        internalDefaultCtx()->diag().error(oreo::ErrorCode::INTERNAL_ERROR, \
            std::string("C++ exception: ") + e.what()); \
        return default_val; \
    } catch (...) { \
        internalDefaultCtx()->diag().error(oreo::ErrorCode::INTERNAL_ERROR, \
            "Unknown exception in C API"); \
        return default_val; \
    }

// For void functions:
#define OREO_C_CATCH_VOID \
    } catch (const Standard_Failure& e) { \
        internalDefaultCtx()->diag().error(oreo::ErrorCode::OCCT_FAILURE, \
            std::string("OCCT exception: ") + e.GetMessageString()); \
    } catch (const std::exception& e) { \
        internalDefaultCtx()->diag().error(oreo::ErrorCode::INTERNAL_ERROR, \
            std::string("C++ exception: ") + e.what()); \
    } catch (...) { \
        internalDefaultCtx()->diag().error(oreo::ErrorCode::INTERNAL_ERROR, \
            "Unknown exception in C API"); \
    }

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
//
// Thread-safety: NOT thread-safe. This is by design — the legacy C API
// (functions without an explicit OreoContext parameter) is intended for
// single-threaded use only. New code should use the explicit OreoContext
// API (oreo_context_create / oreo_ctx_* functions) instead, with one
// context per thread.
std::shared_ptr<oreo::KernelContext>& internalDefaultCtx() {
    static auto ctx = oreo::KernelContext::create();
    return ctx;
}

// Thread-local storage for error message strings returned by
// oreo_context_last_error_message (separate from name buffers to avoid
// one function clobbering another's return value).
thread_local std::string tl_errorMsgBuffer;

// Thread-local storage for name strings returned by oreo_face_name / oreo_edge_name.
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

    auto* lastErr = ctx.diag().lastError();
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
// Lifecycle (legacy — gated by OREO_ENABLE_LEGACY_API)
// ============================================================

#ifdef OREO_ENABLE_LEGACY_API
void oreo_init(void) {
    OREO_C_TRY
    oreo::KernelContext::initOCCT();
    (void)internalDefaultCtx();
    OREO_C_CATCH_VOID
}

void oreo_shutdown(void) {
    OREO_C_TRY
    internalDefaultCtx().reset();
    internalDefaultCtx() = oreo::KernelContext::create();
    OREO_C_CATCH_VOID
}

OreoError oreo_last_error(void) {
    OREO_C_TRY
    return errorFromContext(*internalDefaultCtx());
    OREO_C_CATCH_RETURN((OreoError{OREO_INTERNAL_ERROR, "Exception querying error state", "", ""}))
}
#endif // OREO_ENABLE_LEGACY_API

// ============================================================
// Context
// ============================================================

OreoContext oreo_context_create(void) {
    OREO_C_TRY
    return new OreoContext_T{oreo::KernelContext::create()};
    OREO_C_CATCH_RETURN(nullptr)
}

OreoContext oreo_context_create_with_doc(uint64_t documentId, const char* documentUUID) {
    // If documentUUID is non-empty, the KernelContext constructor derives
    // documentId from it via SipHash-2-4 — the caller's numeric documentId
    // is ignored in that case, matching the header contract.
    OREO_C_TRY
    oreo::KernelConfig cfg;
    cfg.documentId = documentId;
    if (documentUUID && *documentUUID) {
        cfg.documentUUID = documentUUID;
    }
    // create() throws std::logic_error on low-32-bit documentId collision
    // (see TagAllocator::registerDocumentId). Let the OREO_C_CATCH wrapper
    // surface that via diag() and return nullptr.
    return new OreoContext_T{oreo::KernelContext::create(cfg)};
    OREO_C_CATCH_RETURN(nullptr)
}

uint64_t oreo_context_document_id(OreoContext ctx) {
    OREO_C_TRY
    if (!ctx) return 0;
    return ctx->ctx->tags().documentId();
    OREO_C_CATCH_RETURN(0)
}

void oreo_context_free(OreoContext ctx) {
    OREO_C_TRY
    if (!ctx) return;
    delete ctx;
    OREO_C_CATCH_VOID
}

int oreo_context_has_errors(OreoContext ctx) {
    OREO_C_TRY
    if (!ctx) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return 0; }
    return ctx->ctx->diag().hasErrors() ? 1 : 0;
    OREO_C_CATCH_RETURN(0)
}

int oreo_context_has_warnings(OreoContext ctx) {
    OREO_C_TRY
    if (!ctx) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return 0; }
    return ctx->ctx->diag().hasWarnings() ? 1 : 0;
    OREO_C_CATCH_RETURN(0)
}

int oreo_context_error_count(OreoContext ctx) {
    OREO_C_TRY
    if (!ctx) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return 0; }
    return ctx->ctx->diag().errorCount();
    OREO_C_CATCH_RETURN(0)
}

const char* oreo_context_last_error_message(OreoContext ctx) {
    OREO_C_TRY
    if (!ctx) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return ""; }
    auto* last = ctx->ctx->diag().lastError();
    if (!last) return "";
    tl_errorMsgBuffer = last->message;
    return tl_errorMsgBuffer.c_str();
    OREO_C_CATCH_RETURN("")
}

// Context-aware primitives
OreoSolid oreo_ctx_make_box(OreoContext ctx, double dx, double dy, double dz) {
    OREO_C_TRY
    if (!ctx) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::makeBox(*ctx->ctx, dx, dy, dz));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_ctx_make_cylinder(OreoContext ctx, double radius, double height) {
    OREO_C_TRY
    if (!ctx) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::makeCylinder(*ctx->ctx, radius, height));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_ctx_make_sphere(OreoContext ctx, double radius) {
    OREO_C_TRY
    if (!ctx) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::makeSphere(*ctx->ctx, radius));
    OREO_C_CATCH_RETURN(nullptr)
}

// Context-aware geometry operations
OreoSolid oreo_ctx_extrude(OreoContext ctx, OreoSolid base, double dx, double dy, double dz) {
    OREO_C_TRY
    if (!ctx) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    if (!base) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::extrude(*ctx->ctx, base->ns, gp_Vec(dx, dy, dz)));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_ctx_boolean_union(OreoContext ctx, OreoSolid a, OreoSolid b) {
    OREO_C_TRY
    if (!ctx) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    if (!a || !b) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::booleanUnion(*ctx->ctx, a->ns, b->ns));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_ctx_boolean_subtract(OreoContext ctx, OreoSolid target, OreoSolid tool) {
    OREO_C_TRY
    if (!ctx) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    if (!target || !tool) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::booleanSubtract(*ctx->ctx, target->ns, tool->ns));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_ctx_fillet(OreoContext ctx, OreoSolid solid, OreoEdge edges[], int n, double radius) {
    OREO_C_TRY
    if (!ctx) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    if (!solid) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    if (n < 0) {
        ctx->ctx->diag().error(oreo::ErrorCode::OUT_OF_RANGE, "n must be >= 0");
        return nullptr;
    }
    if (n > 0 && !edges) {
        ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT,
            "edges pointer is NULL but n > 0");
        return nullptr;
    }
    std::vector<oreo::NamedEdge> edgeVec;
    for (int i = 0; i < n; ++i) {
        if (edges[i]) edgeVec.push_back({oreo::IndexedName("Edge", i+1), edges[i]->ns.shape()});
    }
    return wrapSolid(oreo::fillet(*ctx->ctx, solid->ns, edgeVec, radius));
    OREO_C_CATCH_RETURN(nullptr)
}

// ============================================================
// v2 identity-aware C API (Phase 5)
// ============================================================

namespace {

// Shared helper for oreo_face_shape_id / oreo_edge_shape_id.
int queryShapeId(OreoSolid solid, const char* elementType, int index,
                 OreoShapeId* out) {
    if (out) *out = OreoShapeId{0, 0};
    if (!solid) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT,
            "Null handle");
        return OREO_INVALID_INPUT;
    }
    if (!out) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT,
            "out pointer is NULL");
        return OREO_INVALID_INPUT;
    }
    if (index < 1) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::OUT_OF_RANGE,
            "index must be >= 1");
        return OREO_INVALID_INPUT;
    }
    auto name = solid->ns.getElementName(oreo::IndexedName(elementType, index));
    auto id   = oreo::ElementMap::extractShapeIdentity(name);
    out->document_id = id.documentId;
    out->counter     = id.counter;
    return OREO_OK;
}

// Shared helper for oreo_ctx_face_name / oreo_ctx_edge_name.
int queryName(OreoContext ctx, OreoSolid solid, const char* elementType,
              int index, char* buf, size_t buflen, size_t* needed) {
    if (!ctx) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT,
            "Null context handle");
        return OREO_INVALID_INPUT;
    }
    if (!solid) {
        ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT,
            "Null solid handle");
        return OREO_INVALID_INPUT;
    }
    if (!needed) {
        ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT,
            "needed pointer is NULL");
        return OREO_INVALID_INPUT;
    }
    if (index < 1) {
        ctx->ctx->diag().error(oreo::ErrorCode::OUT_OF_RANGE,
            "index must be >= 1");
        return OREO_INVALID_INPUT;
    }
    auto name = solid->ns.getElementName(oreo::IndexedName(elementType, index));
    const auto& s = name.data();
    *needed = s.size();  // excludes NUL

    // Size-probe with NULL buffer always returns OREO_OK so callers can
    // measure without triggering an error diagnostic.
    if (buf == nullptr && buflen == 0) return OREO_OK;
    if (buf == nullptr) {
        ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT,
            "buf is NULL but buflen > 0");
        return OREO_INVALID_INPUT;
    }
    if (buflen < s.size() + 1) {
        // Write as much as fits plus NUL so partial reads are safe.
        if (buflen > 0) {
            std::memcpy(buf, s.data(), buflen - 1);
            buf[buflen - 1] = '\0';
        }
        return OREO_BUFFER_TOO_SMALL;
    }
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    return OREO_OK;
}

}  // anonymous namespace

int oreo_face_shape_id(OreoSolid solid, int index, OreoShapeId* out) {
    OREO_C_TRY
    return queryShapeId(solid, "Face", index, out);
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_edge_shape_id(OreoSolid solid, int index, OreoShapeId* out) {
    OREO_C_TRY
    return queryShapeId(solid, "Edge", index, out);
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_face_name(OreoContext ctx, OreoSolid solid, int index,
                       char* buf, size_t buflen, size_t* needed) {
    OREO_C_TRY
    return queryName(ctx, solid, "Face", index, buf, buflen, needed);
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_edge_name(OreoContext ctx, OreoSolid solid, int index,
                       char* buf, size_t buflen, size_t* needed) {
    OREO_C_TRY
    return queryName(ctx, solid, "Edge", index, buf, buflen, needed);
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_serialize(OreoContext ctx, OreoSolid solid,
                       uint8_t* buf, size_t buflen, size_t* needed) {
    OREO_C_TRY
    if (!ctx) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT,
            "Null context handle");
        return OREO_INVALID_INPUT;
    }
    if (!solid) {
        ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT,
            "Null solid handle");
        return OREO_INVALID_INPUT;
    }
    if (!needed) {
        ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT,
            "needed pointer is NULL");
        return OREO_INVALID_INPUT;
    }
    auto r = oreo::serialize(*ctx->ctx, solid->ns);
    if (!r.ok()) return OREO_SERIALIZE_FAILED;
    const auto& payload = r.value();
    *needed = payload.size();

    if (buf == nullptr && buflen == 0) return OREO_OK;  // size probe
    if (buf == nullptr) {
        ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT,
            "buf is NULL but buflen > 0");
        return OREO_INVALID_INPUT;
    }
    if (buflen < payload.size()) {
        return OREO_BUFFER_TOO_SMALL;
    }
    std::memcpy(buf, payload.data(), payload.size());
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

OreoSolid oreo_ctx_deserialize(OreoContext ctx,
                               const uint8_t* data, size_t len) {
    OREO_C_TRY
    if (!ctx) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT,
            "Null context handle");
        return nullptr;
    }
    if (!data) {
        ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT,
            "Null data pointer");
        return nullptr;
    }
    auto r = oreo::deserialize(*ctx->ctx, data, len);
    if (!r.ok()) return nullptr;
    return wrapSolid(std::move(r).value());
    OREO_C_CATCH_RETURN(nullptr)
}

// ============================================================
// Primitives through Sketch are the legacy singleton surface —
// gated as one block on OREO_ENABLE_LEGACY_API.
// ============================================================
#ifdef OREO_ENABLE_LEGACY_API

// ============================================================
// Primitives (legacy — uses default context)
// ============================================================

OreoSolid oreo_make_box(double dx, double dy, double dz) {
    OREO_C_TRY
    return wrapSolid(oreo::makeBox(*internalDefaultCtx(), dx, dy, dz));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_make_cylinder(double radius, double height) {
    OREO_C_TRY
    return wrapSolid(oreo::makeCylinder(*internalDefaultCtx(), radius, height));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_make_sphere(double radius) {
    OREO_C_TRY
    return wrapSolid(oreo::makeSphere(*internalDefaultCtx(), radius));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_make_cone(double r1, double r2, double height) {
    OREO_C_TRY
    return wrapSolid(oreo::makeCone(*internalDefaultCtx(), r1, r2, height));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_make_torus(double major_r, double minor_r) {
    OREO_C_TRY
    return wrapSolid(oreo::makeTorus(*internalDefaultCtx(), major_r, minor_r));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_make_wedge(double dx, double dy, double dz, double ltx) {
    OREO_C_TRY
    return wrapSolid(oreo::makeWedge(*internalDefaultCtx(), dx, dy, dz, ltx));
    OREO_C_CATCH_RETURN(nullptr)
}

// ============================================================
// Geometry operations
// ============================================================

OreoSolid oreo_extrude(OreoSolid base, double dx, double dy, double dz) {
    OREO_C_TRY
    if (!base) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::extrude(*internalDefaultCtx(), base->ns, gp_Vec(dx, dy, dz)));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_revolve(OreoSolid base,
                       double ax, double ay, double az,
                       double dx, double dy, double dz,
                       double angle_rad) {
    OREO_C_TRY
    if (!base) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    gp_Ax1 axis(gp_Pnt(ax, ay, az), gp_Dir(dx, dy, dz));
    return wrapSolid(oreo::revolve(*internalDefaultCtx(), base->ns, axis, angle_rad));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_fillet(OreoSolid solid, OreoEdge edges[], int n, double radius) {
    OREO_C_TRY
    if (!solid) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    // Array-bounds gate (audit fix): a negative `n` or a null `edges` with
    // n > 0 would have dereferenced junk / walked off the heap. Reject both
    // before we touch `edges[i]`.
    if (n < 0) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::OUT_OF_RANGE, "n must be >= 0");
        return nullptr;
    }
    if (n > 0 && !edges) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT,
            "edges pointer is NULL but n > 0");
        return nullptr;
    }
    std::vector<oreo::NamedEdge> edgeVec;
    for (int i = 0; i < n; ++i) {
        if (edges[i]) {
            edgeVec.push_back({oreo::IndexedName("Edge", i+1), edges[i]->ns.shape()});
        }
    }
    return wrapSolid(oreo::fillet(*internalDefaultCtx(), solid->ns, edgeVec, radius));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_chamfer(OreoSolid solid, OreoEdge edges[], int n, double distance) {
    OREO_C_TRY
    if (!solid) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    if (n < 0) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::OUT_OF_RANGE, "n must be >= 0");
        return nullptr;
    }
    if (n > 0 && !edges) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT,
            "edges pointer is NULL but n > 0");
        return nullptr;
    }
    std::vector<oreo::NamedEdge> edgeVec;
    for (int i = 0; i < n; ++i) {
        if (edges[i]) {
            edgeVec.push_back({oreo::IndexedName("Edge", i+1), edges[i]->ns.shape()});
        }
    }
    return wrapSolid(oreo::chamfer(*internalDefaultCtx(), solid->ns, edgeVec, distance));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_boolean_union(OreoSolid a, OreoSolid b) {
    OREO_C_TRY
    if (!a || !b) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::booleanUnion(*internalDefaultCtx(), a->ns, b->ns));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_boolean_subtract(OreoSolid target, OreoSolid tool) {
    OREO_C_TRY
    if (!target || !tool) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::booleanSubtract(*internalDefaultCtx(), target->ns, tool->ns));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_boolean_intersect(OreoSolid a, OreoSolid b) {
    OREO_C_TRY
    if (!a || !b) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::booleanIntersect(*internalDefaultCtx(), a->ns, b->ns));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_shell(OreoSolid solid, OreoFace faces[], int n, double thickness) {
    OREO_C_TRY
    if (!solid) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    if (n < 0) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::OUT_OF_RANGE, "n must be >= 0");
        return nullptr;
    }
    if (n > 0 && !faces) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT,
            "faces pointer is NULL but n > 0");
        return nullptr;
    }
    std::vector<oreo::NamedFace> faceVec;
    for (int i = 0; i < n; ++i) {
        if (faces[i]) {
            faceVec.push_back({oreo::IndexedName("Face", i+1), faces[i]->ns.shape()});
        }
    }
    return wrapSolid(oreo::shell(*internalDefaultCtx(), solid->ns, faceVec, thickness));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_loft(OreoWire profiles[], int n, int make_solid) {
    OREO_C_TRY
    if (n < 0) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::OUT_OF_RANGE, "n must be >= 0");
        return nullptr;
    }
    if (n > 0 && !profiles) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT,
            "profiles pointer is NULL but n > 0");
        return nullptr;
    }
    std::vector<oreo::NamedShape> pVec;
    for (int i = 0; i < n; ++i) {
        if (profiles[i]) pVec.push_back(profiles[i]->ns);
    }
    return wrapSolid(oreo::loft(*internalDefaultCtx(), pVec, make_solid != 0));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_sweep(OreoWire profile, OreoWire path) {
    OREO_C_TRY
    if (!profile || !path) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::sweep(*internalDefaultCtx(), profile->ns, path->ns));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_mirror(OreoSolid solid,
                      double px, double py, double pz,
                      double nx, double ny, double nz) {
    OREO_C_TRY
    if (!solid) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    gp_Ax2 plane(gp_Pnt(px, py, pz), gp_Dir(nx, ny, nz));
    return wrapSolid(oreo::mirror(*internalDefaultCtx(), solid->ns, plane));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_pattern_linear(OreoSolid solid,
                              double dx, double dy, double dz,
                              int count, double spacing) {
    OREO_C_TRY
    if (!solid) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::patternLinear(*internalDefaultCtx(), solid->ns, gp_Vec(dx, dy, dz), count, spacing));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_pattern_circular(OreoSolid solid,
                                double ax, double ay, double az,
                                double dx, double dy, double dz,
                                int count, double total_angle_rad) {
    OREO_C_TRY
    if (!solid) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    gp_Ax1 axis(gp_Pnt(ax, ay, az), gp_Dir(dx, dy, dz));
    return wrapSolid(oreo::patternCircular(*internalDefaultCtx(), solid->ns, axis, count, total_angle_rad));
    OREO_C_CATCH_RETURN(nullptr)
}

// ============================================================
// Manufacturing operations
// ============================================================

OreoSolid oreo_draft(OreoSolid solid, OreoFace faces[], int n,
                     double angle_deg,
                     double pull_dx, double pull_dy, double pull_dz) {
    OREO_C_TRY
    if (!solid) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    if (n < 0) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::OUT_OF_RANGE, "n must be >= 0");
        return nullptr;
    }
    if (n > 0 && !faces) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT,
            "faces pointer is NULL but n > 0");
        return nullptr;
    }
    std::vector<oreo::NamedFace> faceVec;
    for (int i = 0; i < n; ++i) {
        if (faces[i]) faceVec.push_back({oreo::IndexedName("Face", i+1), faces[i]->ns.shape()});
    }
    return wrapSolid(oreo::draft(*internalDefaultCtx(), solid->ns, faceVec, angle_deg, gp_Dir(pull_dx, pull_dy, pull_dz)));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_hole(OreoSolid solid, OreoFace face,
                    double cx, double cy, double cz,
                    double diameter, double depth) {
    OREO_C_TRY
    if (!solid) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    oreo::NamedFace nf = {oreo::IndexedName("Face", 1), face ? face->ns.shape() : TopoDS_Shape()};
    return wrapSolid(oreo::hole(*internalDefaultCtx(), solid->ns, nf, gp_Pnt(cx, cy, cz), diameter, depth));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_pocket(OreoSolid solid, OreoSolid profile, double depth) {
    OREO_C_TRY
    if (!solid || !profile) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::pocket(*internalDefaultCtx(), solid->ns, profile->ns, depth));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_rib(OreoSolid solid, OreoSolid profile,
                   double dx, double dy, double dz, double thickness) {
    OREO_C_TRY
    if (!solid || !profile) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::rib(*internalDefaultCtx(), solid->ns, profile->ns, gp_Dir(dx, dy, dz), thickness));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_offset(OreoSolid solid, double distance) {
    OREO_C_TRY
    if (!solid) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::offset(*internalDefaultCtx(), solid->ns, distance));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_thicken(OreoSolid shell, double thickness) {
    OREO_C_TRY
    if (!shell) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::thicken(*internalDefaultCtx(), shell->ns, thickness));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_split_body(OreoSolid solid,
                          double px, double py, double pz,
                          double nx, double ny, double nz) {
    OREO_C_TRY
    if (!solid) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::splitBody(*internalDefaultCtx(), solid->ns, gp_Pln(gp_Pnt(px, py, pz), gp_Dir(nx, ny, nz))));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_fillet_variable(OreoSolid solid, OreoEdge edge,
                               double start_radius, double end_radius) {
    OREO_C_TRY
    if (!solid || !edge) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    oreo::NamedEdge ne = {oreo::IndexedName("Edge", 1), edge->ns.shape()};
    return wrapSolid(oreo::filletVariable(*internalDefaultCtx(), solid->ns, ne, start_radius, end_radius));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_make_face_from_wire(OreoWire wire) {
    OREO_C_TRY
    if (!wire) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::makeFaceFromWire(*internalDefaultCtx(), wire->ns));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_combine(OreoSolid shapes[], int n) {
    OREO_C_TRY
    if (n < 0) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::OUT_OF_RANGE, "n must be >= 0");
        return nullptr;
    }
    if (n > 0 && !shapes) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT,
            "shapes pointer is NULL but n > 0");
        return nullptr;
    }
    std::vector<oreo::NamedShape> vec;
    for (int i = 0; i < n; ++i) {
        if (shapes[i]) vec.push_back(shapes[i]->ns);
    }
    return wrapSolid(oreo::combine(*internalDefaultCtx(), vec));
    OREO_C_CATCH_RETURN(nullptr)
}

// ============================================================
// Queries
// ============================================================

OreoBBox oreo_aabb(OreoSolid solid) {
    OREO_C_TRY
    if (!solid) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return {0,0,0,0,0,0}; }
    auto r = oreo::aabb(*internalDefaultCtx(), solid->ns);
    if (!r.ok()) return {0,0,0,0,0,0};
    auto b = r.value();
    return {b.xmin, b.ymin, b.zmin, b.xmax, b.ymax, b.zmax};
    OREO_C_CATCH_RETURN(OreoBBox{})
}

OreoBBox oreo_footprint(OreoSolid before, OreoSolid after) {
    OREO_C_TRY
    if (!before || !after) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return {0,0,0,0,0,0}; }
    auto r = oreo::footprint(*internalDefaultCtx(), before->ns, after->ns);
    if (!r.ok()) return {0,0,0,0,0,0};
    auto b = r.value();
    return {b.xmin, b.ymin, b.zmin, b.xmax, b.ymax, b.zmax};
    OREO_C_CATCH_RETURN(OreoBBox{})
}

int oreo_face_count(OreoSolid solid) {
    OREO_C_TRY
    if (!solid) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return 0; }
    return solid->ns.countSubShapes(TopAbs_FACE);
    OREO_C_CATCH_RETURN(0)
}

int oreo_edge_count(OreoSolid solid) {
    OREO_C_TRY
    if (!solid) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return 0; }
    return solid->ns.countSubShapes(TopAbs_EDGE);
    OREO_C_CATCH_RETURN(0)
}

OreoFace oreo_get_face(OreoSolid solid, int index) {
    OREO_C_TRY
    if (!solid) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    if (index < 1) return nullptr;
    TopoDS_Shape face = solid->ns.getSubShape(TopAbs_FACE, index);
    if (face.IsNull()) return nullptr;
    auto* h = new OreoFace_T{oreo::NamedShape(face, oreo::ShapeIdentity{})};
    return h;
    OREO_C_CATCH_RETURN(nullptr)
}

OreoEdge oreo_get_edge(OreoSolid solid, int index) {
    OREO_C_TRY
    if (!solid) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    if (index < 1) return nullptr;
    TopoDS_Shape edge = solid->ns.getSubShape(TopAbs_EDGE, index);
    if (edge.IsNull()) return nullptr;
    auto* h = new OreoEdge_T{oreo::NamedShape(edge, oreo::ShapeIdentity{})};
    return h;
    OREO_C_CATCH_RETURN(nullptr)
}

double oreo_measure_distance(OreoSolid a, OreoSolid b) {
    OREO_C_TRY
    if (!a || !b) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return -1.0; }
    auto r = oreo::measureDistance(*internalDefaultCtx(), a->ns, b->ns);
    if (!r.ok()) return -1.0;
    return r.value();
    OREO_C_CATCH_RETURN(-1.0)
}

OreoMassProps oreo_mass_properties(OreoSolid solid) {
    OREO_C_TRY
    if (!solid) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return {}; }
    auto r = oreo::massProperties(*internalDefaultCtx(), solid->ns);
    if (!r.ok()) return {};
    auto m = r.value();
    return {m.volume, m.surfaceArea,
            m.centerOfMassX, m.centerOfMassY, m.centerOfMassZ,
            m.ixx, m.iyy, m.izz, m.ixy, m.ixz, m.iyz};
    OREO_C_CATCH_RETURN({})
}

// ============================================================
// Element naming
// ============================================================

const char* oreo_face_name(OreoSolid solid, int index) {
    OREO_C_TRY
    if (!solid) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return ""; }
    if (index < 1) return "";
    auto name = solid->ns.getElementName(oreo::IndexedName("Face", index));
    tl_nameBuffer = name.data();
    return tl_nameBuffer.c_str();
    OREO_C_CATCH_RETURN("")
}

const char* oreo_edge_name(OreoSolid solid, int index) {
    OREO_C_TRY
    if (!solid) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return ""; }
    if (index < 1) return "";
    auto name = solid->ns.getElementName(oreo::IndexedName("Edge", index));
    tl_nameBuffer = name.data();
    return tl_nameBuffer.c_str();
    OREO_C_CATCH_RETURN("")
}

// ============================================================
// I/O
// ============================================================

OreoSolid oreo_import_step(const uint8_t* data, size_t len) {
    OREO_C_TRY
    if (!data) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    auto r = oreo::importStep(*internalDefaultCtx(), data, len);
    if (!r.ok()) return nullptr;
    return wrapSolid(std::move(r.value().shape));
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_import_step_file(const char* path) {
    OREO_C_TRY
    if (!path) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    auto r = oreo::importStepFile(*internalDefaultCtx(), path);
    if (!r.ok()) return nullptr;
    return wrapSolid(std::move(r.value().shape));
    OREO_C_CATCH_RETURN(nullptr)
}

int oreo_export_step_file(OreoSolid solids[], int n, const char* path) {
    OREO_C_TRY
    if (!path) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return 0; }
    if (n < 0) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::OUT_OF_RANGE, "n must be >= 0");
        return 0;
    }
    if (n > 0 && !solids) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT,
            "solids pointer is NULL but n > 0");
        return 0;
    }
    std::vector<oreo::NamedShape> shapes;
    for (int i = 0; i < n; ++i) {
        if (solids[i]) shapes.push_back(solids[i]->ns);
    }
    auto r = oreo::exportStepFile(*internalDefaultCtx(), shapes, path);
    if (!r.ok()) return 0;
    return r.value() ? 1 : 0;
    OREO_C_CATCH_RETURN(0)
}

// ============================================================
// Serialization
// ============================================================

uint8_t* oreo_serialize(OreoSolid solid, size_t* out_len) {
    OREO_C_TRY
    if (!solid) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    if (!out_len) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    auto r = oreo::serialize(*internalDefaultCtx(), solid->ns);
    if (!r.ok()) { *out_len = 0; return nullptr; }
    auto& buf = r.value();
    if (buf.empty()) { *out_len = 0; return nullptr; }
    *out_len = buf.size();
    auto* data = new uint8_t[buf.size()];
    std::memcpy(data, buf.data(), buf.size());
    return data;
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_deserialize(const uint8_t* data, size_t len) {
    OREO_C_TRY
    if (!data) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    auto r = oreo::deserialize(*internalDefaultCtx(), data, len);
    if (!r.ok()) return nullptr;
    return wrapSolid(std::move(r).value());
    OREO_C_CATCH_RETURN(nullptr)
}

void oreo_free_buffer(uint8_t* buf) {
    OREO_C_TRY
    delete[] buf;
    OREO_C_CATCH_VOID
}

// ============================================================
// Sketch
// ============================================================

OreoSketch oreo_sketch_create(void) {
    OREO_C_TRY
    return new OreoSketch_T{};
    OREO_C_CATCH_RETURN(nullptr)
}

void oreo_sketch_free(OreoSketch sketch) {
    OREO_C_TRY
    delete sketch;
    OREO_C_CATCH_VOID
}

int oreo_sketch_add_point(OreoSketch sketch, double x, double y) {
    OREO_C_TRY
    if (!sketch) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return -1; }
    sketch->points.push_back({x, y});
    return static_cast<int>(sketch->points.size()) - 1;
    OREO_C_CATCH_RETURN(-1)
}

int oreo_sketch_add_line(OreoSketch sketch,
                         double x1, double y1,
                         double x2, double y2) {
    OREO_C_TRY
    if (!sketch) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return -1; }
    sketch->lines.push_back({{x1, y1}, {x2, y2}});
    return static_cast<int>(sketch->lines.size()) - 1;
    OREO_C_CATCH_RETURN(-1)
}

int oreo_sketch_add_circle(OreoSketch sketch,
                           double cx, double cy, double radius) {
    OREO_C_TRY
    if (!sketch) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return -1; }
    sketch->circles.push_back({{cx, cy}, radius});
    return static_cast<int>(sketch->circles.size()) - 1;
    OREO_C_CATCH_RETURN(-1)
}

int oreo_sketch_add_arc(OreoSketch sketch,
                        double cx, double cy,
                        double sx, double sy,
                        double ex, double ey,
                        double radius) {
    OREO_C_TRY
    if (!sketch) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return -1; }
    sketch->arcs.push_back({{cx, cy}, {sx, sy}, {ex, ey}, radius});
    return static_cast<int>(sketch->arcs.size()) - 1;
    OREO_C_CATCH_RETURN(-1)
}

int oreo_sketch_add_constraint(OreoSketch sketch,
                               int type, int entity1,
                               int entity2, int entity3,
                               double value) {
    OREO_C_TRY
    if (!sketch) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return -1; }
    // Validate constraint type enum
    if (type < 0 || type > static_cast<int>(oreo::ConstraintType::Concentric)) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT,
            "Invalid constraint type: " + std::to_string(type));
        return -1;
    }
    oreo::SketchConstraint c;
    c.type = static_cast<oreo::ConstraintType>(type);
    c.entity1 = entity1;
    c.entity2 = entity2;
    c.entity3 = entity3;
    c.value = value;
    sketch->constraints.push_back(c);
    return static_cast<int>(sketch->constraints.size()) - 1;
    OREO_C_CATCH_RETURN(-1)
}

int oreo_sketch_solve(OreoSketch sketch) {
    OREO_C_TRY
    if (!sketch) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return OREO_INVALID_INPUT; }
    auto r = oreo::solveSketch(
        *internalDefaultCtx(),
        sketch->points, sketch->lines, sketch->circles,
        sketch->arcs, sketch->constraints);
    if (!r.ok()) return OREO_SKETCH_SOLVE_FAILED;
    sketch->lastResult = std::move(r).value();

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
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_sketch_dof(OreoSketch sketch) {
    OREO_C_TRY
    if (!sketch) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return -1; }
    return sketch->lastResult.degreesOfFreedom;
    OREO_C_CATCH_RETURN(-1)
}

void oreo_sketch_get_point(OreoSketch sketch, int index,
                           double* x, double* y) {
    OREO_C_TRY
    if (!sketch || index < 0 || index >= static_cast<int>(sketch->points.size())) {
        if (x) *x = 0.0;
        if (y) *y = 0.0;
        return;
    }
    if (x) *x = sketch->points[index].x;
    if (y) *y = sketch->points[index].y;
    OREO_C_CATCH_VOID
}

void oreo_sketch_get_line(OreoSketch sketch, int index,
                          double* x1, double* y1,
                          double* x2, double* y2) {
    OREO_C_TRY
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
    OREO_C_CATCH_VOID
}

OreoWire oreo_sketch_to_wire(OreoSketch sketch) {
    OREO_C_TRY
    if (!sketch) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    auto r = oreo::sketchToWire(*internalDefaultCtx(), sketch->lines, sketch->circles, sketch->arcs);
    if (!r.ok()) return nullptr;
    auto ns = std::move(r).value();
    if (ns.isNull()) return nullptr;
    return new OreoWire_T{std::move(ns)};
    OREO_C_CATCH_RETURN(nullptr)
}

#endif // OREO_ENABLE_LEGACY_API (Primitives through Sketch)

// ============================================================
// Tessellation / Meshing
// ============================================================

struct OreoMesh_T { oreo::MeshResult mesh; };

#ifdef OREO_ENABLE_LEGACY_API
OreoMesh oreo_tessellate(OreoSolid solid, double linear_deflection, double angular_deflection_deg) {
    OREO_C_TRY
    if (!solid) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    auto result = oreo::tessellate(*internalDefaultCtx(), solid->ns, {linear_deflection, angular_deflection_deg});
    if (!result.ok()) return nullptr;
    auto mesh = std::move(result).value();
    if (mesh.empty()) return nullptr;
    return new OreoMesh_T{std::move(mesh)};
    OREO_C_CATCH_RETURN(nullptr)
}
#endif // OREO_ENABLE_LEGACY_API

OreoMesh oreo_ctx_tessellate(OreoContext ctx, OreoSolid solid,
                             double linear_deflection, double angular_deflection_deg) {
    OREO_C_TRY
    if (!ctx) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    if (!solid) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    auto result = oreo::tessellate(*ctx->ctx, solid->ns, {linear_deflection, angular_deflection_deg});
    if (!result.ok()) return nullptr;
    auto mesh = std::move(result).value();
    if (mesh.empty()) return nullptr;
    return new OreoMesh_T{std::move(mesh)};
    OREO_C_CATCH_RETURN(nullptr)
}

void oreo_mesh_free(OreoMesh mesh) {
    OREO_C_TRY
    delete mesh;
    OREO_C_CATCH_VOID
}

int oreo_mesh_vertex_count(OreoMesh mesh) {
    OREO_C_TRY
    if (!mesh) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return 0; }
    return mesh->mesh.totalVertices;
    OREO_C_CATCH_RETURN(0)
}
int oreo_mesh_triangle_count(OreoMesh mesh) {
    OREO_C_TRY
    if (!mesh) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return 0; }
    return mesh->mesh.totalTriangles;
    OREO_C_CATCH_RETURN(0)
}
int oreo_mesh_face_group_count(OreoMesh mesh) {
    OREO_C_TRY
    if (!mesh) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return 0; }
    return static_cast<int>(mesh->mesh.faceGroups.size());
    OREO_C_CATCH_RETURN(0)
}
int oreo_mesh_edge_count(OreoMesh mesh) {
    OREO_C_TRY
    if (!mesh) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return 0; }
    return mesh->mesh.totalEdges;
    OREO_C_CATCH_RETURN(0)
}

const float* oreo_mesh_positions(OreoMesh mesh) {
    OREO_C_TRY
    if (!mesh) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return mesh->mesh.positions.empty() ? nullptr : mesh->mesh.positions.data();
    OREO_C_CATCH_RETURN(nullptr)
}
const float* oreo_mesh_normals(OreoMesh mesh) {
    OREO_C_TRY
    if (!mesh) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return mesh->mesh.normals.empty() ? nullptr : mesh->mesh.normals.data();
    OREO_C_CATCH_RETURN(nullptr)
}
const uint32_t* oreo_mesh_indices(OreoMesh mesh) {
    OREO_C_TRY
    if (!mesh) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return mesh->mesh.indices.empty() ? nullptr : mesh->mesh.indices.data();
    OREO_C_CATCH_RETURN(nullptr)
}

int oreo_mesh_face_group_id(OreoMesh mesh, int group) {
    OREO_C_TRY
    if (!mesh) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return 0; }
    if (group < 0 || group >= (int)mesh->mesh.faceGroups.size()) return 0;
    return mesh->mesh.faceGroups[group].faceId;
    OREO_C_CATCH_RETURN(0)
}
const char* oreo_mesh_face_group_name(OreoMesh mesh, int group) {
    OREO_C_TRY
    if (!mesh) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return ""; }
    if (group < 0 || group >= (int)mesh->mesh.faceGroups.size()) return "";
    return mesh->mesh.faceGroups[group].faceName.c_str();
    OREO_C_CATCH_RETURN("")
}
int oreo_mesh_face_group_index_start(OreoMesh mesh, int group) {
    OREO_C_TRY
    if (!mesh) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return 0; }
    if (group < 0 || group >= (int)mesh->mesh.faceGroups.size()) return 0;
    return static_cast<int>(mesh->mesh.faceGroups[group].indexStart);
    OREO_C_CATCH_RETURN(0)
}
int oreo_mesh_face_group_index_count(OreoMesh mesh, int group) {
    OREO_C_TRY
    if (!mesh) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return 0; }
    if (group < 0 || group >= (int)mesh->mesh.faceGroups.size()) return 0;
    return static_cast<int>(mesh->mesh.faceGroups[group].indexCount);
    OREO_C_CATCH_RETURN(0)
}

// ============================================================
// Memory management
// ============================================================

void oreo_free_solid(OreoSolid solid) {
    OREO_C_TRY
    delete solid;
    OREO_C_CATCH_VOID
}
void oreo_free_wire(OreoWire wire) {
    OREO_C_TRY
    delete wire;
    OREO_C_CATCH_VOID
}
void oreo_free_edge(OreoEdge edge) {
    OREO_C_TRY
    delete edge;
    OREO_C_CATCH_VOID
}
void oreo_free_face(OreoFace face) {
    OREO_C_TRY
    delete face;
    OREO_C_CATCH_VOID
}

} // extern "C"
