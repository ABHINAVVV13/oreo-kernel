// SPDX-License-Identifier: LGPL-2.1-or-later

// oreo_capi.cpp — C API wrapper over C++ internals.

#include "oreo_kernel.h"

#include "core/kernel_context.h"
#include "core/diagnostic.h"
#include "core/schema.h"
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
#include "feature/feature.h"
#include "feature/feature_schema.h"
#include "feature/feature_tree.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS.hxx>

#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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

#define OREO_C_CATCH_RETURN_CTX(ctx_handle, default_val) \
    } catch (const Standard_Failure& e) { \
        reportCxxBoundaryException(ctx_handle, oreo::ErrorCode::OCCT_FAILURE, \
            std::string("OCCT exception: ") + e.GetMessageString()); \
        return default_val; \
    } catch (const std::exception& e) { \
        reportCxxBoundaryException(ctx_handle, oreo::ErrorCode::INTERNAL_ERROR, \
            std::string("C++ exception: ") + e.what()); \
        return default_val; \
    } catch (...) { \
        reportCxxBoundaryException(ctx_handle, oreo::ErrorCode::INTERNAL_ERROR, \
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

// ============================================================
// Public/internal error code parity
// ============================================================
//
// The public C ABI (OreoErrorCode in include/oreo_kernel.h) and the
// internal C++ enum (oreo::ErrorCode in src/core/diagnostic.h) MUST
// share identical numeric values for every code in either enum. The
// static_asserts below trap any drift at compile time so a future
// addition to one side without the other becomes an immediate build
// break, not a silent ABI mismatch.
//
// Adding a new error code:
//   1. Add it to both OreoErrorCode (include/oreo_kernel.h) and
//      oreo::ErrorCode (src/core/diagnostic.h) with the same number.
//   2. Add a static_assert below.
//   3. Document the code in docs/error-codes.md.
//
// The ctx-aware C API returns ints; we cast oreo::ErrorCode to int
// directly because the values are guaranteed to match.
static_assert(static_cast<int>(oreo::ErrorCode::OK)                          == OREO_OK,                          "ErrorCode/OreoErrorCode drift: OK");
static_assert(static_cast<int>(oreo::ErrorCode::INVALID_INPUT)               == OREO_INVALID_INPUT,               "ErrorCode/OreoErrorCode drift: INVALID_INPUT");
static_assert(static_cast<int>(oreo::ErrorCode::OCCT_FAILURE)                == OREO_OCCT_FAILURE,                "ErrorCode/OreoErrorCode drift: OCCT_FAILURE");
static_assert(static_cast<int>(oreo::ErrorCode::BOOLEAN_FAILED)              == OREO_BOOLEAN_FAILED,              "ErrorCode/OreoErrorCode drift: BOOLEAN_FAILED");
static_assert(static_cast<int>(oreo::ErrorCode::SHAPE_INVALID)               == OREO_SHAPE_INVALID,               "ErrorCode/OreoErrorCode drift: SHAPE_INVALID");
static_assert(static_cast<int>(oreo::ErrorCode::SHAPE_FIX_FAILED)            == OREO_SHAPE_FIX_FAILED,            "ErrorCode/OreoErrorCode drift: SHAPE_FIX_FAILED");
static_assert(static_cast<int>(oreo::ErrorCode::SKETCH_SOLVE_FAILED)         == OREO_SKETCH_SOLVE_FAILED,         "ErrorCode/OreoErrorCode drift: SKETCH_SOLVE_FAILED");
static_assert(static_cast<int>(oreo::ErrorCode::SKETCH_REDUNDANT)            == OREO_SKETCH_REDUNDANT,            "ErrorCode/OreoErrorCode drift: SKETCH_REDUNDANT");
static_assert(static_cast<int>(oreo::ErrorCode::SKETCH_CONFLICTING)          == OREO_SKETCH_CONFLICTING,          "ErrorCode/OreoErrorCode drift: SKETCH_CONFLICTING");
static_assert(static_cast<int>(oreo::ErrorCode::STEP_IMPORT_FAILED)          == OREO_STEP_IMPORT_FAILED,          "ErrorCode/OreoErrorCode drift: STEP_IMPORT_FAILED");
static_assert(static_cast<int>(oreo::ErrorCode::STEP_EXPORT_FAILED)          == OREO_STEP_EXPORT_FAILED,          "ErrorCode/OreoErrorCode drift: STEP_EXPORT_FAILED");
static_assert(static_cast<int>(oreo::ErrorCode::SERIALIZE_FAILED)            == OREO_SERIALIZE_FAILED,            "ErrorCode/OreoErrorCode drift: SERIALIZE_FAILED");
static_assert(static_cast<int>(oreo::ErrorCode::DESERIALIZE_FAILED)          == OREO_DESERIALIZE_FAILED,          "ErrorCode/OreoErrorCode drift: DESERIALIZE_FAILED");
static_assert(static_cast<int>(oreo::ErrorCode::LEGACY_IDENTITY_DOWNGRADE)   == OREO_LEGACY_IDENTITY_DOWNGRADE,   "ErrorCode/OreoErrorCode drift: LEGACY_IDENTITY_DOWNGRADE");
static_assert(static_cast<int>(oreo::ErrorCode::V2_IDENTITY_NOT_REPRESENTABLE) == OREO_V2_IDENTITY_NOT_REPRESENTABLE, "ErrorCode/OreoErrorCode drift: V2_IDENTITY_NOT_REPRESENTABLE");
static_assert(static_cast<int>(oreo::ErrorCode::MALFORMED_ELEMENT_NAME)      == OREO_MALFORMED_ELEMENT_NAME,      "ErrorCode/OreoErrorCode drift: MALFORMED_ELEMENT_NAME");
static_assert(static_cast<int>(oreo::ErrorCode::BUFFER_TOO_SMALL)            == OREO_BUFFER_TOO_SMALL,            "ErrorCode/OreoErrorCode drift: BUFFER_TOO_SMALL");
static_assert(static_cast<int>(oreo::ErrorCode::NOT_INITIALIZED)             == OREO_NOT_INITIALIZED,             "ErrorCode/OreoErrorCode drift: NOT_INITIALIZED");
static_assert(static_cast<int>(oreo::ErrorCode::INTERNAL_ERROR)              == OREO_INTERNAL_ERROR,              "ErrorCode/OreoErrorCode drift: INTERNAL_ERROR");
static_assert(static_cast<int>(oreo::ErrorCode::NOT_SUPPORTED)               == OREO_NOT_SUPPORTED,               "ErrorCode/OreoErrorCode drift: NOT_SUPPORTED");
static_assert(static_cast<int>(oreo::ErrorCode::INVALID_STATE)               == OREO_INVALID_STATE,               "ErrorCode/OreoErrorCode drift: INVALID_STATE");
static_assert(static_cast<int>(oreo::ErrorCode::OUT_OF_RANGE)                == OREO_OUT_OF_RANGE,                "ErrorCode/OreoErrorCode drift: OUT_OF_RANGE");
static_assert(static_cast<int>(oreo::ErrorCode::TIMEOUT)                     == OREO_TIMEOUT,                     "ErrorCode/OreoErrorCode drift: TIMEOUT");
static_assert(static_cast<int>(oreo::ErrorCode::CANCELLED)                   == OREO_CANCELLED,                   "ErrorCode/OreoErrorCode drift: CANCELLED");
static_assert(static_cast<int>(oreo::ErrorCode::RESOURCE_EXHAUSTED)          == OREO_RESOURCE_EXHAUSTED,          "ErrorCode/OreoErrorCode drift: RESOURCE_EXHAUSTED");
static_assert(static_cast<int>(oreo::ErrorCode::DIAG_TRUNCATED)              == OREO_DIAG_TRUNCATED,              "ErrorCode/OreoErrorCode drift: DIAG_TRUNCATED");

// Slot-based sketch storage with stable entity IDs.
//
// Each entity (point / line / circle / arc / constraint) lives in a
// slot vector. The PUBLIC ID is the slot index — assigned at add time,
// stable across edits, and never reused. Removed entities are marked
// `removed = true` and stay in their slot as a tombstone so subsequent
// add_*() calls produce strictly-increasing IDs.
//
// Translation between PUBLIC ID and the LIVE solver index happens at
// solve time in solveCompacted(), based on the constraint family
// schema in oreo::constraintSchemaFor.
//
// ABI: OreoSketch_T is opaque to consumers, so adding the slot wrapper
// is source- and binary-compatible with handles produced by older
// builds (the previous `vector<SketchPoint>` and the new
// `vector<Slot<SketchPoint>>` simply have a different memory layout
// inside the opaque struct).
template <typename T>
struct OreoSketchSlot {
    T value{};
    bool removed = false;
};

struct OreoSketch_T {
    std::vector<OreoSketchSlot<oreo::SketchPoint>>      pointSlots;
    std::vector<OreoSketchSlot<oreo::SketchLine>>       lineSlots;
    std::vector<OreoSketchSlot<oreo::SketchCircle>>     circleSlots;
    std::vector<OreoSketchSlot<oreo::SketchArc>>        arcSlots;
    std::vector<OreoSketchSlot<oreo::SketchConstraint>> constraintSlots;
    oreo::SolveResult                   lastResult = {};
    // Non-null iff the sketch was created via oreo_ctx_sketch_create —
    // routes diagnostics through the bound context. Legacy
    // oreo_sketch_create leaves this null and uses internalDefaultCtx.
    std::shared_ptr<oreo::KernelContext> ctxBinding;
};

// ── Slot helpers ─────────────────────────────────────────────
//
// Wrapped in extern "C++" because they appear inside the public
// extern "C" block opened later in this file but use std::vector and
// other C++-only types.
extern "C++" {
namespace {

// Family check — does the public ID `pid` reference a live entity in
// the requested family? Out-of-range, removed, or wrong-family IDs
// return false. AnyCurve / CircleOrArc unions accept membership in
// any of their constituent families.
bool sketchEntityAlive(const OreoSketch_T* s,
                       oreo::ConstraintEntityFamily fam, int pid) {
    using F = oreo::ConstraintEntityFamily;
    if (!s) return false;
    auto inSlot = [](const auto& slots, int pid) {
        return pid >= 0 && pid < static_cast<int>(slots.size())
               && !slots[pid].removed;
    };
    switch (fam) {
        case F::None:        return pid == -1;
        case F::Point:       return inSlot(s->pointSlots,  pid);
        case F::Line:        return inSlot(s->lineSlots,   pid);
        case F::Circle:      return inSlot(s->circleSlots, pid);
        case F::Arc:         return inSlot(s->arcSlots,    pid);
        case F::AnyCurve:    return inSlot(s->lineSlots,  pid)
                                  || inSlot(s->circleSlots, pid)
                                  || inSlot(s->arcSlots,   pid);
        case F::CircleOrArc: return inSlot(s->circleSlots, pid)
                                  || inSlot(s->arcSlots,   pid);
    }
    return false;
}

// Build per-family compact-vectors-from-slots PLUS a publicId →
// liveIndex map (vector indexed by publicId, -1 = not live in this
// family). Used by both the solver and sketchToWire.
struct SketchLiveView {
    std::vector<oreo::SketchPoint>  points;
    std::vector<oreo::SketchLine>   lines;
    std::vector<oreo::SketchCircle> circles;
    std::vector<oreo::SketchArc>    arcs;
    // Public ID -> live index (or -1 if removed / oob).
    std::vector<int> pointToLive;
    std::vector<int> lineToLive;
    std::vector<int> circleToLive;
    std::vector<int> arcToLive;
    // Live index -> public ID (== slot index).
    std::vector<int> livePointToPublic;
    std::vector<int> liveLineToPublic;
    std::vector<int> liveCircleToPublic;
    std::vector<int> liveArcToPublic;
};

template <typename Slot, typename Vec, typename Map, typename Inv>
void compactFamily(const std::vector<Slot>& slots, Vec& outLive,
                   Map& outPublicToLive, Inv& outLiveToPublic) {
    outPublicToLive.assign(slots.size(), -1);
    outLive.reserve(slots.size());
    for (int i = 0; i < static_cast<int>(slots.size()); ++i) {
        if (slots[i].removed) continue;
        outPublicToLive[i] = static_cast<int>(outLive.size());
        outLiveToPublic.push_back(i);
        outLive.push_back(slots[i].value);
    }
}

SketchLiveView buildLiveView(const OreoSketch_T* s) {
    SketchLiveView v;
    compactFamily(s->pointSlots,  v.points,  v.pointToLive,  v.livePointToPublic);
    compactFamily(s->lineSlots,   v.lines,   v.lineToLive,   v.liveLineToPublic);
    compactFamily(s->circleSlots, v.circles, v.circleToLive, v.liveCircleToPublic);
    compactFamily(s->arcSlots,    v.arcs,    v.arcToLive,    v.liveArcToPublic);
    return v;
}

// Resolve a single entity slot (Point/Line/Circle/Arc/AnyCurve/...)
// from publicId to liveIndex. Returns -1 when the family is None or
// the publicId does not name a live entity in that family.
int resolveEntity(const SketchLiveView& v,
                  oreo::ConstraintEntityFamily fam, int pid) {
    using F = oreo::ConstraintEntityFamily;
    auto get = [&](const std::vector<int>& m) -> int {
        if (pid < 0 || pid >= static_cast<int>(m.size())) return -1;
        return m[pid];
    };
    switch (fam) {
        case F::None:        return -1;
        case F::Point:       return get(v.pointToLive);
        case F::Line:        return get(v.lineToLive);
        case F::Circle:      return get(v.circleToLive);
        case F::Arc:         return get(v.arcToLive);
        case F::AnyCurve: {
            int li = get(v.lineToLive);   if (li >= 0) return li;
            int ci = get(v.circleToLive); if (ci >= 0) return ci;
            return get(v.arcToLive);
        }
        case F::CircleOrArc: {
            int ci = get(v.circleToLive); if (ci >= 0) return ci;
            return get(v.arcToLive);
        }
    }
    return -1;
}

// Translate an in-tree SketchConstraint (entity IDs are PUBLIC) into
// a copy whose entity1/2/3 are LIVE indices ready for the solver.
// Returns std::nullopt if any required entity is dead — the constraint
// is silently dropped from the solve, but its public slot stays alive
// so the user can re-target it.
std::optional<oreo::SketchConstraint>
translateConstraint(const SketchLiveView& v, const oreo::SketchConstraint& c) {
    using F = oreo::ConstraintEntityFamily;
    const auto sch = oreo::constraintSchemaFor(c.type);
    oreo::SketchConstraint live = c;
    auto fix = [&](F fam, int pid, int& outLive) -> bool {
        if (fam == F::None) { outLive = -1; return true; }
        int li = resolveEntity(v, fam, pid);
        if (li < 0) return false;
        outLive = li;
        return true;
    };
    if (!fix(sch.e1, c.entity1, live.entity1)) return std::nullopt;
    if (!fix(sch.e2, c.entity2, live.entity2)) return std::nullopt;
    if (!fix(sch.e3, c.entity3, live.entity3)) return std::nullopt;
    return live;
}

// Cascade-remove every constraint slot referencing the freshly killed
// entity. Called from the public oreo_ctx_sketch_remove_*() handlers.
void cascadeConstraintsForRemovedEntity(OreoSketch_T* s,
                                        oreo::ConstraintEntityFamily fam,
                                        int pid) {
    using F = oreo::ConstraintEntityFamily;
    if (!s) return;
    for (auto& slot : s->constraintSlots) {
        if (slot.removed) continue;
        const auto& c = slot.value;
        const auto sch = oreo::constraintSchemaFor(c.type);
        auto matches = [&](F slotFam, int slotPid) -> bool {
            if (slotFam == F::None) return false;
            // The constraint's family must include the killed entity's
            // family (Point in Point; Line in AnyCurve; etc.).
            if (slotFam != fam
                && !(slotFam == F::AnyCurve   && (fam == F::Line || fam == F::Circle || fam == F::Arc))
                && !(slotFam == F::CircleOrArc && (fam == F::Circle || fam == F::Arc))) {
                return false;
            }
            return slotPid == pid;
        };
        if (matches(sch.e1, c.entity1) ||
            matches(sch.e2, c.entity2) ||
            matches(sch.e3, c.entity3)) {
            slot.removed = true;
        }
    }
}

// Shared solve worker: builds a live view of the sketch, translates
// constraints from public IDs to live indices, solves, copies solved
// values back to the original slots, and remaps SolveResult indices
// from live-constraint space to public-constraint IDs.
int solveSketchSlots(OreoSketch sketch, oreo::KernelContext& ctx) {
    SketchLiveView v = buildLiveView(sketch);

    std::vector<oreo::SketchConstraint> liveConstraints;
    std::vector<int> liveConstraintToPublic;
    liveConstraints.reserve(sketch->constraintSlots.size());
    liveConstraintToPublic.reserve(sketch->constraintSlots.size());
    for (int i = 0; i < static_cast<int>(sketch->constraintSlots.size()); ++i) {
        const auto& slot = sketch->constraintSlots[i];
        if (slot.removed) continue;
        auto translated = translateConstraint(v, slot.value);
        if (!translated) {
            ctx.diag().warning(oreo::ErrorCode::INVALID_INPUT,
                "constraint[" + std::to_string(i)
                + "] references a removed entity; skipping this solve");
            continue;
        }
        liveConstraints.push_back(*translated);
        liveConstraintToPublic.push_back(i);
    }

    auto r = oreo::solveSketch(ctx, v.points, v.lines, v.circles, v.arcs,
                               liveConstraints);
    if (!r.ok()) {
        sketch->lastResult = oreo::SolveResult{};
        sketch->lastResult.status = oreo::SolveStatus::Failed;
        return OREO_SKETCH_SOLVE_FAILED;
    }
    auto solved = std::move(r).value();

    for (int li = 0; li < static_cast<int>(v.points.size()); ++li) {
        sketch->pointSlots[v.livePointToPublic[li]].value = v.points[li];
    }
    for (int li = 0; li < static_cast<int>(v.lines.size()); ++li) {
        sketch->lineSlots[v.liveLineToPublic[li]].value = v.lines[li];
    }
    for (int li = 0; li < static_cast<int>(v.circles.size()); ++li) {
        sketch->circleSlots[v.liveCircleToPublic[li]].value = v.circles[li];
    }
    for (int li = 0; li < static_cast<int>(v.arcs.size()); ++li) {
        sketch->arcSlots[v.liveArcToPublic[li]].value = v.arcs[li];
    }

    auto remap = [&](std::vector<int>& ids) {
        for (auto& idx : ids) {
            if (idx >= 0 && idx < static_cast<int>(liveConstraintToPublic.size())) {
                idx = liveConstraintToPublic[idx];
            }
        }
    };
    remap(solved.conflictingConstraints);
    remap(solved.redundantConstraints);
    sketch->lastResult = std::move(solved);

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

OreoWire sketchToWireSlots(OreoSketch sketch, oreo::KernelContext& ctx) {
    SketchLiveView v = buildLiveView(sketch);
    auto r = oreo::sketchToWire(ctx, v.lines, v.circles, v.arcs);
    if (!r.ok()) return nullptr;
    auto ns = std::move(r).value();
    if (ns.isNull()) return nullptr;
    return new OreoWire_T{std::move(ns)};
}

} // anonymous namespace
} // extern "C++"

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

oreo::DiagnosticCollector& diagFor(OreoContext ctx) {
    return (ctx && ctx->ctx) ? ctx->ctx->diag() : internalDefaultCtx()->diag();
}

void reportCxxBoundaryException(OreoContext ctx,
                                oreo::ErrorCode code,
                                const std::string& message) {
    diagFor(ctx).error(code, message);
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

OreoErrorCode publicErrorFromInternal(oreo::ErrorCode code) {
    switch (code) {
        case oreo::ErrorCode::OK: return OREO_OK;
        case oreo::ErrorCode::INVALID_INPUT: return OREO_INVALID_INPUT;
        case oreo::ErrorCode::OCCT_FAILURE: return OREO_OCCT_FAILURE;
        case oreo::ErrorCode::BOOLEAN_FAILED: return OREO_BOOLEAN_FAILED;
        case oreo::ErrorCode::SHAPE_INVALID: return OREO_SHAPE_INVALID;
        case oreo::ErrorCode::SHAPE_FIX_FAILED: return OREO_SHAPE_FIX_FAILED;
        case oreo::ErrorCode::SKETCH_SOLVE_FAILED: return OREO_SKETCH_SOLVE_FAILED;
        case oreo::ErrorCode::SKETCH_REDUNDANT: return OREO_SKETCH_REDUNDANT;
        case oreo::ErrorCode::SKETCH_CONFLICTING: return OREO_SKETCH_CONFLICTING;
        case oreo::ErrorCode::STEP_IMPORT_FAILED: return OREO_STEP_IMPORT_FAILED;
        case oreo::ErrorCode::STEP_EXPORT_FAILED: return OREO_STEP_EXPORT_FAILED;
        case oreo::ErrorCode::SERIALIZE_FAILED: return OREO_SERIALIZE_FAILED;
        case oreo::ErrorCode::DESERIALIZE_FAILED: return OREO_DESERIALIZE_FAILED;
        case oreo::ErrorCode::LEGACY_IDENTITY_DOWNGRADE: return OREO_LEGACY_IDENTITY_DOWNGRADE;
        case oreo::ErrorCode::V2_IDENTITY_NOT_REPRESENTABLE: return OREO_V2_IDENTITY_NOT_REPRESENTABLE;
        case oreo::ErrorCode::MALFORMED_ELEMENT_NAME: return OREO_MALFORMED_ELEMENT_NAME;
        case oreo::ErrorCode::BUFFER_TOO_SMALL: return OREO_BUFFER_TOO_SMALL;
        case oreo::ErrorCode::NOT_INITIALIZED: return OREO_NOT_INITIALIZED;
        case oreo::ErrorCode::INTERNAL_ERROR: return OREO_INTERNAL_ERROR;
        case oreo::ErrorCode::NOT_SUPPORTED: return OREO_NOT_SUPPORTED;
        case oreo::ErrorCode::INVALID_STATE: return OREO_INVALID_STATE;
        case oreo::ErrorCode::OUT_OF_RANGE: return OREO_OUT_OF_RANGE;
        case oreo::ErrorCode::TIMEOUT: return OREO_TIMEOUT;
        case oreo::ErrorCode::CANCELLED: return OREO_CANCELLED;
        case oreo::ErrorCode::RESOURCE_EXHAUSTED: return OREO_RESOURCE_EXHAUSTED;
        case oreo::ErrorCode::DIAG_TRUNCATED: return OREO_DIAG_TRUNCATED;
    }
    return OREO_INTERNAL_ERROR;
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
        publicErrorFromInternal(lastErr->code),
        msgBuf.c_str(),
        entBuf.c_str(),
        sugBuf.c_str()
    };
}

} // anonymous namespace

extern "C" {

// ============================================================
// Library identity
// ============================================================

const char* oreo_kernel_version(void) {
    // KERNEL_VERSION_FULL is a constexpr const char* injected from the
    // CMake project version + OREO_KERNEL_PRE_RELEASE — see schema.h.
    return oreo::schema::KERNEL_VERSION_FULL;
}

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
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return 0; }
    return ctx->ctx->diag().hasErrors() ? 1 : 0;
    OREO_C_CATCH_RETURN_CTX(ctx, 0)
}

int oreo_context_has_warnings(OreoContext ctx) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return 0; }
    return ctx->ctx->diag().hasWarnings() ? 1 : 0;
    OREO_C_CATCH_RETURN_CTX(ctx, 0)
}

int oreo_context_error_count(OreoContext ctx) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return 0; }
    return ctx->ctx->diag().errorCount();
    OREO_C_CATCH_RETURN_CTX(ctx, 0)
}

int oreo_context_warning_count(OreoContext ctx) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return 0; }
    return ctx->ctx->diag().warningCount();
    OREO_C_CATCH_RETURN_CTX(ctx, 0)
}

int oreo_context_diagnostic_count(OreoContext ctx) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return 0; }
    return static_cast<int>(ctx->ctx->diag().all().size());
    OREO_C_CATCH_RETURN_CTX(ctx, 0)
}

int oreo_context_diagnostic_code(OreoContext ctx, int index) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return OREO_INVALID_INPUT; }
    const auto& all = ctx->ctx->diag().all();
    if (index < 0 || index >= static_cast<int>(all.size())) {
        ctx->ctx->diag().error(oreo::ErrorCode::OUT_OF_RANGE, "diagnostic index out of range");
        return OREO_OUT_OF_RANGE;
    }
    return publicErrorFromInternal(all[static_cast<std::size_t>(index)].code);
    OREO_C_CATCH_RETURN_CTX(ctx, OREO_INTERNAL_ERROR)
}

const char* oreo_context_diagnostic_message(OreoContext ctx, int index) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return ""; }
    const auto& all = ctx->ctx->diag().all();
    if (index < 0 || index >= static_cast<int>(all.size())) {
        ctx->ctx->diag().error(oreo::ErrorCode::OUT_OF_RANGE, "diagnostic index out of range");
        return "";
    }
    tl_errorMsgBuffer = all[static_cast<std::size_t>(index)].message;
    return tl_errorMsgBuffer.c_str();
    OREO_C_CATCH_RETURN_CTX(ctx, "")
}

const char* oreo_context_last_error_message(OreoContext ctx) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return ""; }
    auto* last = ctx->ctx->diag().lastError();
    if (!last) return "";
    tl_errorMsgBuffer = last->message;
    return tl_errorMsgBuffer.c_str();
    OREO_C_CATCH_RETURN_CTX(ctx, "")
}

// Context-aware primitives
OreoSolid oreo_ctx_make_box(OreoContext ctx, double dx, double dy, double dz) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::makeBox(*ctx->ctx, dx, dy, dz));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoSolid oreo_ctx_make_cylinder(OreoContext ctx, double radius, double height) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::makeCylinder(*ctx->ctx, radius, height));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoSolid oreo_ctx_make_sphere(OreoContext ctx, double radius) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::makeSphere(*ctx->ctx, radius));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

// Context-aware geometry operations
OreoSolid oreo_ctx_extrude(OreoContext ctx, OreoSolid base, double dx, double dy, double dz) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    if (!base) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::extrude(*ctx->ctx, base->ns, gp_Vec(dx, dy, dz)));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoSolid oreo_ctx_boolean_union(OreoContext ctx, OreoSolid a, OreoSolid b) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    if (!a || !b) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::booleanUnion(*ctx->ctx, a->ns, b->ns));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoSolid oreo_ctx_boolean_subtract(OreoContext ctx, OreoSolid target, OreoSolid tool) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    if (!target || !tool) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return wrapSolid(oreo::booleanSubtract(*ctx->ctx, target->ns, tool->ns));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoSolid oreo_ctx_fillet(OreoContext ctx, OreoSolid solid, OreoEdge edges[], int n, double radius) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
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
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
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
        diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT,
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
    OREO_C_CATCH_RETURN_CTX(ctx, OREO_INTERNAL_ERROR)
}

int oreo_ctx_edge_name(OreoContext ctx, OreoSolid solid, int index,
                       char* buf, size_t buflen, size_t* needed) {
    OREO_C_TRY
    return queryName(ctx, solid, "Edge", index, buf, buflen, needed);
    OREO_C_CATCH_RETURN_CTX(ctx, OREO_INTERNAL_ERROR)
}

int oreo_ctx_serialize(OreoContext ctx, OreoSolid solid,
                       uint8_t* buf, size_t buflen, size_t* needed) {
    OREO_C_TRY
    if (!ctx) {
        diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT,
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
    OREO_C_CATCH_RETURN_CTX(ctx, OREO_INTERNAL_ERROR)
}

OreoSolid oreo_ctx_deserialize(OreoContext ctx,
                               const uint8_t* data, size_t len) {
    OREO_C_TRY
    if (!ctx) {
        diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT,
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
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoBBox oreo_ctx_aabb(OreoContext ctx, OreoSolid solid) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return {}; }
    if (!solid) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null solid handle"); return {}; }
    auto r = oreo::aabb(*ctx->ctx, solid->ns);
    if (!r.ok()) return {};
    auto b = r.value();
    return {b.xmin, b.ymin, b.zmin, b.xmax, b.ymax, b.zmax};
    OREO_C_CATCH_RETURN_CTX(ctx, OreoBBox{})
}

int oreo_ctx_face_count(OreoContext ctx, OreoSolid solid) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return 0; }
    if (!solid) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null solid handle"); return 0; }
    return solid->ns.countSubShapes(TopAbs_FACE);
    OREO_C_CATCH_RETURN_CTX(ctx, 0)
}

int oreo_ctx_edge_count(OreoContext ctx, OreoSolid solid) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return 0; }
    if (!solid) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null solid handle"); return 0; }
    return solid->ns.countSubShapes(TopAbs_EDGE);
    OREO_C_CATCH_RETURN_CTX(ctx, 0)
}

OreoMassProps oreo_ctx_mass_properties(OreoContext ctx, OreoSolid solid) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return {}; }
    if (!solid) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null solid handle"); return {}; }
    auto r = oreo::massProperties(*ctx->ctx, solid->ns);
    if (!r.ok()) return {};
    auto m = r.value();
    return {m.volume, m.surfaceArea,
            m.centerOfMassX, m.centerOfMassY, m.centerOfMassZ,
            m.ixx, m.iyy, m.izz, m.ixy, m.ixz, m.iyz};
    OREO_C_CATCH_RETURN_CTX(ctx, OreoMassProps{})
}

OreoSolid oreo_ctx_import_step(OreoContext ctx, const uint8_t* data, size_t len) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    if (!data) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null data pointer"); return nullptr; }
    auto r = oreo::importStep(*ctx->ctx, data, len);
    if (!r.ok()) return nullptr;
    return wrapSolid(std::move(r.value().shape));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoSolid oreo_ctx_import_step_file(OreoContext ctx, const char* path) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    if (!path) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null path pointer"); return nullptr; }
    auto r = oreo::importStepFile(*ctx->ctx, path);
    if (!r.ok()) return nullptr;
    return wrapSolid(std::move(r.value().shape));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

int oreo_ctx_export_step_file(OreoContext ctx, OreoSolid solids[], int n, const char* path) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return 0; }
    if (!path) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null path pointer"); return 0; }
    if (n < 0) {
        ctx->ctx->diag().error(oreo::ErrorCode::OUT_OF_RANGE, "n must be >= 0");
        return 0;
    }
    if (n > 0 && !solids) {
        ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT,
            "solids pointer is NULL but n > 0");
        return 0;
    }
    std::vector<oreo::NamedShape> shapes;
    shapes.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        if (solids[i]) shapes.push_back(solids[i]->ns);
    }
    auto r = oreo::exportStepFile(*ctx->ctx, shapes, path);
    if (!r.ok()) return 0;
    return r.value() ? 1 : 0;
    OREO_C_CATCH_RETURN_CTX(ctx, 0)
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
    sketch->pointSlots.push_back({{x, y}, false});
    return static_cast<int>(sketch->pointSlots.size()) - 1;
    OREO_C_CATCH_RETURN(-1)
}

int oreo_sketch_add_line(OreoSketch sketch,
                         double x1, double y1,
                         double x2, double y2) {
    OREO_C_TRY
    if (!sketch) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return -1; }
    sketch->lineSlots.push_back({{{x1, y1}, {x2, y2}}, false});
    return static_cast<int>(sketch->lineSlots.size()) - 1;
    OREO_C_CATCH_RETURN(-1)
}

int oreo_sketch_add_circle(OreoSketch sketch,
                           double cx, double cy, double radius) {
    OREO_C_TRY
    if (!sketch) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return -1; }
    sketch->circleSlots.push_back({{{cx, cy}, radius}, false});
    return static_cast<int>(sketch->circleSlots.size()) - 1;
    OREO_C_CATCH_RETURN(-1)
}

int oreo_sketch_add_arc(OreoSketch sketch,
                        double cx, double cy,
                        double sx, double sy,
                        double ex, double ey,
                        double radius) {
    OREO_C_TRY
    if (!sketch) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return -1; }
    sketch->arcSlots.push_back({{{cx, cy}, {sx, sy}, {ex, ey}, radius}, false});
    return static_cast<int>(sketch->arcSlots.size()) - 1;
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
    sketch->constraintSlots.push_back({c, false});
    return static_cast<int>(sketch->constraintSlots.size()) - 1;
    OREO_C_CATCH_RETURN(-1)
}

#if 0
namespace {

// Shared solve worker: builds a live view of the sketch, translates
// constraints from public IDs to live indices, solves, copies solved
// values back to the original slots, and remaps SolveResult indices
// from live-constraint space to public-constraint IDs.
int solveSketchSlots(OreoSketch sketch, oreo::KernelContext& ctx) {
    // Build compact live vectors + per-family ID maps.
    SketchLiveView v = buildLiveView(sketch);

    // Translate constraints. Live-constraint k -> public ID
    // liveConstraintToPublic[k] so we can remap SolveResult.
    std::vector<oreo::SketchConstraint> liveConstraints;
    std::vector<int> liveConstraintToPublic;
    liveConstraints.reserve(sketch->constraintSlots.size());
    liveConstraintToPublic.reserve(sketch->constraintSlots.size());
    for (int i = 0; i < static_cast<int>(sketch->constraintSlots.size()); ++i) {
        const auto& slot = sketch->constraintSlots[i];
        if (slot.removed) continue;
        auto translated = translateConstraint(v, slot.value);
        if (!translated) {
            // Constraint references a removed entity. Drop from this
            // solve — keep the slot alive so the user can re-target.
            // Emit a one-line warning so debugging is possible.
            ctx.diag().warning(oreo::ErrorCode::INVALID_INPUT,
                "constraint[" + std::to_string(i)
                + "] references a removed entity; skipping this solve");
            continue;
        }
        liveConstraints.push_back(*translated);
        liveConstraintToPublic.push_back(i);
    }

    auto r = oreo::solveSketch(ctx, v.points, v.lines, v.circles, v.arcs,
                               liveConstraints);
    if (!r.ok()) {
        sketch->lastResult = oreo::SolveResult{};
        sketch->lastResult.status = oreo::SolveStatus::Failed;
        return OREO_SKETCH_SOLVE_FAILED;
    }
    auto solved = std::move(r).value();

    // Copy solved values back to the original slots via the inverse
    // live -> public map. We never wrote to removed slots.
    for (int li = 0; li < static_cast<int>(v.points.size()); ++li) {
        sketch->pointSlots[v.livePointToPublic[li]].value = v.points[li];
    }
    for (int li = 0; li < static_cast<int>(v.lines.size()); ++li) {
        sketch->lineSlots[v.liveLineToPublic[li]].value = v.lines[li];
    }
    for (int li = 0; li < static_cast<int>(v.circles.size()); ++li) {
        sketch->circleSlots[v.liveCircleToPublic[li]].value = v.circles[li];
    }
    for (int li = 0; li < static_cast<int>(v.arcs.size()); ++li) {
        sketch->arcSlots[v.liveArcToPublic[li]].value = v.arcs[li];
    }

    // Remap conflicting/redundant indices from live-constraint space
    // back to public IDs so the caller can correlate against
    // oreo_sketch_add_constraint return values.
    auto remap = [&](std::vector<int>& v) {
        for (auto& idx : v) {
            if (idx >= 0 && idx < static_cast<int>(liveConstraintToPublic.size())) {
                idx = liveConstraintToPublic[idx];
            }
        }
    };
    remap(solved.conflictingConstraints);
    remap(solved.redundantConstraints);
    sketch->lastResult = std::move(solved);

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

OreoWire sketchToWireSlots(OreoSketch sketch, oreo::KernelContext& ctx) {
    SketchLiveView v = buildLiveView(sketch);
    auto r = oreo::sketchToWire(ctx, v.lines, v.circles, v.arcs);
    if (!r.ok()) return nullptr;
    auto ns = std::move(r).value();
    if (ns.isNull()) return nullptr;
    return new OreoWire_T{std::move(ns)};
}

} // anonymous namespace
#endif

int oreo_sketch_solve(OreoSketch sketch) {
    OREO_C_TRY
    if (!sketch) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return OREO_INVALID_INPUT; }
    return solveSketchSlots(sketch, *internalDefaultCtx());
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
    if (!sketch || index < 0
        || index >= static_cast<int>(sketch->pointSlots.size())
        || sketch->pointSlots[index].removed) {
        if (x) *x = 0.0;
        if (y) *y = 0.0;
        return;
    }
    if (x) *x = sketch->pointSlots[index].value.x;
    if (y) *y = sketch->pointSlots[index].value.y;
    OREO_C_CATCH_VOID
}

void oreo_sketch_get_line(OreoSketch sketch, int index,
                          double* x1, double* y1,
                          double* x2, double* y2) {
    OREO_C_TRY
    if (!sketch || index < 0
        || index >= static_cast<int>(sketch->lineSlots.size())
        || sketch->lineSlots[index].removed) {
        if (x1) *x1 = 0.0;
        if (y1) *y1 = 0.0;
        if (x2) *x2 = 0.0;
        if (y2) *y2 = 0.0;
        return;
    }
    const auto& l = sketch->lineSlots[index].value;
    if (x1) *x1 = l.p1.x;
    if (y1) *y1 = l.p1.y;
    if (x2) *x2 = l.p2.x;
    if (y2) *y2 = l.p2.y;
    OREO_C_CATCH_VOID
}

OreoWire oreo_sketch_to_wire(OreoSketch sketch) {
    OREO_C_TRY
    if (!sketch) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    return sketchToWireSlots(sketch, *internalDefaultCtx());
    OREO_C_CATCH_RETURN(nullptr)
}

#endif // OREO_ENABLE_LEGACY_API (Primitives through Sketch)

// ============================================================
// Context-aware sketch API (always available, no legacy gate)
// ============================================================

namespace {

oreo::KernelContext& sketchCtx(OreoSketch s) {
    return (s && s->ctxBinding) ? *s->ctxBinding : *internalDefaultCtx();
}

oreo::DiagnosticCollector& sketchDiag(OreoSketch s) {
    return sketchCtx(s).diag();
}

} // anonymous namespace

OreoSketch oreo_ctx_sketch_create(OreoContext ctx) {
    OREO_C_TRY
    if (!ctx || !ctx->ctx) {
        diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context");
        return nullptr;
    }
    auto* s = new OreoSketch_T{};
    s->ctxBinding = ctx->ctx;
    return s;
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

void oreo_ctx_sketch_free(OreoSketch sketch) {
    OREO_C_TRY
    delete sketch;
    OREO_C_CATCH_VOID
}

int oreo_ctx_sketch_add_point(OreoSketch sketch, double x, double y) {
    OREO_C_TRY
    if (!sketch) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null sketch handle");
        return -1;
    }
    sketch->pointSlots.push_back({{x, y}, false});
    return static_cast<int>(sketch->pointSlots.size()) - 1;
    OREO_C_CATCH_RETURN(-1)
}

int oreo_ctx_sketch_add_line(OreoSketch sketch,
                              double x1, double y1,
                              double x2, double y2) {
    OREO_C_TRY
    if (!sketch) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null sketch handle");
        return -1;
    }
    sketch->lineSlots.push_back({{{x1, y1}, {x2, y2}}, false});
    return static_cast<int>(sketch->lineSlots.size()) - 1;
    OREO_C_CATCH_RETURN(-1)
}

int oreo_ctx_sketch_add_circle(OreoSketch sketch,
                                double cx, double cy, double radius) {
    OREO_C_TRY
    if (!sketch) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null sketch handle");
        return -1;
    }
    sketch->circleSlots.push_back({{{cx, cy}, radius}, false});
    return static_cast<int>(sketch->circleSlots.size()) - 1;
    OREO_C_CATCH_RETURN(-1)
}

int oreo_ctx_sketch_add_arc(OreoSketch sketch,
                             double cx, double cy,
                             double sx, double sy,
                             double ex, double ey,
                             double radius) {
    OREO_C_TRY
    if (!sketch) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null sketch handle");
        return -1;
    }
    sketch->arcSlots.push_back({{{cx, cy}, {sx, sy}, {ex, ey}, radius}, false});
    return static_cast<int>(sketch->arcSlots.size()) - 1;
    OREO_C_CATCH_RETURN(-1)
}

int oreo_ctx_sketch_add_constraint(OreoSketch sketch,
                                    int type, int entity1,
                                    int entity2, int entity3,
                                    double value) {
    OREO_C_TRY
    if (!sketch) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null sketch handle");
        return -1;
    }
    if (type < 0 || type > static_cast<int>(oreo::ConstraintType::Concentric)) {
        sketchDiag(sketch).error(oreo::ErrorCode::INVALID_INPUT,
            "Invalid constraint type: " + std::to_string(type));
        return -1;
    }
    oreo::SketchConstraint c;
    c.type = static_cast<oreo::ConstraintType>(type);
    c.entity1 = entity1;
    c.entity2 = entity2;
    c.entity3 = entity3;
    c.value = value;
    sketch->constraintSlots.push_back({c, false});
    return static_cast<int>(sketch->constraintSlots.size()) - 1;
    OREO_C_CATCH_RETURN(-1)
}

int oreo_ctx_sketch_solve(OreoSketch sketch) {
    OREO_C_TRY
    if (!sketch) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null sketch handle");
        return OREO_INVALID_INPUT;
    }
    return solveSketchSlots(sketch, sketchCtx(sketch));
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_sketch_dof(OreoSketch sketch) {
    OREO_C_TRY
    if (!sketch) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null sketch handle");
        return -1;
    }
    return sketch->lastResult.degreesOfFreedom;
    OREO_C_CATCH_RETURN(-1)
}

int oreo_ctx_sketch_get_point(OreoSketch sketch, int index,
                               double* x, double* y) {
    OREO_C_TRY
    if (!sketch || index < 0
        || index >= static_cast<int>(sketch->pointSlots.size())
        || sketch->pointSlots[index].removed) {
        if (x) *x = 0.0;
        if (y) *y = 0.0;
        return OREO_INVALID_INPUT;
    }
    if (x) *x = sketch->pointSlots[index].value.x;
    if (y) *y = sketch->pointSlots[index].value.y;
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_sketch_get_line(OreoSketch sketch, int index,
                              double* x1, double* y1,
                              double* x2, double* y2) {
    OREO_C_TRY
    if (!sketch || index < 0
        || index >= static_cast<int>(sketch->lineSlots.size())
        || sketch->lineSlots[index].removed) {
        if (x1) *x1 = 0.0;
        if (y1) *y1 = 0.0;
        if (x2) *x2 = 0.0;
        if (y2) *y2 = 0.0;
        return OREO_INVALID_INPUT;
    }
    const auto& l = sketch->lineSlots[index].value;
    if (x1) *x1 = l.p1.x;
    if (y1) *y1 = l.p1.y;
    if (x2) *x2 = l.p2.x;
    if (y2) *y2 = l.p2.y;
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

OreoWire oreo_ctx_sketch_to_wire(OreoSketch sketch) {
    OREO_C_TRY
    if (!sketch) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null sketch handle");
        return nullptr;
    }
    return sketchToWireSlots(sketch, sketchCtx(sketch));
    OREO_C_CATCH_RETURN(nullptr)
}

// ─── Slot management: remove / alive / count ────────────────
//
// Cascade-deletion guarantee: removing an entity also removes every
// constraint that referenced it, so post-remove the sketch is in a
// solver-valid state. The constraint's own public ID stays alive (as
// a tombstone) for forensic round-trips.

extern "C++" {
namespace {

// Generic remove worker. Caller has already confirmed sketch != null.
template <typename SlotVec>
int removeSlotImpl(OreoSketch sketch, SlotVec& slots, int id,
                   oreo::ConstraintEntityFamily fam) {
    if (id < 0 || id >= static_cast<int>(slots.size())) return OREO_INVALID_INPUT;
    if (slots[id].removed) return OREO_INVALID_INPUT;  // already dead
    slots[id].removed = true;
    cascadeConstraintsForRemovedEntity(sketch, fam, id);
    return OREO_OK;
}

template <typename SlotVec>
int slotAliveImpl(const SlotVec& slots, int id) {
    if (id < 0 || id >= static_cast<int>(slots.size())) return 0;
    return slots[id].removed ? 0 : 1;
}

template <typename SlotVec>
int slotCountLiveImpl(const SlotVec& slots) {
    int n = 0;
    for (const auto& s : slots) if (!s.removed) ++n;
    return n;
}

} // anonymous namespace
} // extern "C++"

int oreo_ctx_sketch_remove_point(OreoSketch sketch, int pointId) {
    OREO_C_TRY
    if (!sketch) return OREO_INVALID_INPUT;
    return removeSlotImpl(sketch, sketch->pointSlots, pointId,
                          oreo::ConstraintEntityFamily::Point);
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_sketch_remove_line(OreoSketch sketch, int lineId) {
    OREO_C_TRY
    if (!sketch) return OREO_INVALID_INPUT;
    return removeSlotImpl(sketch, sketch->lineSlots, lineId,
                          oreo::ConstraintEntityFamily::Line);
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_sketch_remove_circle(OreoSketch sketch, int circleId) {
    OREO_C_TRY
    if (!sketch) return OREO_INVALID_INPUT;
    return removeSlotImpl(sketch, sketch->circleSlots, circleId,
                          oreo::ConstraintEntityFamily::Circle);
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_sketch_remove_arc(OreoSketch sketch, int arcId) {
    OREO_C_TRY
    if (!sketch) return OREO_INVALID_INPUT;
    return removeSlotImpl(sketch, sketch->arcSlots, arcId,
                          oreo::ConstraintEntityFamily::Arc);
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_sketch_remove_constraint(OreoSketch sketch, int constraintId) {
    OREO_C_TRY
    if (!sketch) return OREO_INVALID_INPUT;
    if (constraintId < 0
        || constraintId >= static_cast<int>(sketch->constraintSlots.size()))
        return OREO_INVALID_INPUT;
    if (sketch->constraintSlots[constraintId].removed) return OREO_INVALID_INPUT;
    sketch->constraintSlots[constraintId].removed = true;
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_sketch_point_alive(OreoSketch s, int id) {
    OREO_C_TRY
    if (!s) return 0;
    return slotAliveImpl(s->pointSlots, id);
    OREO_C_CATCH_RETURN(0)
}
int oreo_ctx_sketch_line_alive(OreoSketch s, int id) {
    OREO_C_TRY
    if (!s) return 0;
    return slotAliveImpl(s->lineSlots, id);
    OREO_C_CATCH_RETURN(0)
}
int oreo_ctx_sketch_circle_alive(OreoSketch s, int id) {
    OREO_C_TRY
    if (!s) return 0;
    return slotAliveImpl(s->circleSlots, id);
    OREO_C_CATCH_RETURN(0)
}
int oreo_ctx_sketch_arc_alive(OreoSketch s, int id) {
    OREO_C_TRY
    if (!s) return 0;
    return slotAliveImpl(s->arcSlots, id);
    OREO_C_CATCH_RETURN(0)
}
int oreo_ctx_sketch_constraint_alive(OreoSketch s, int id) {
    OREO_C_TRY
    if (!s) return 0;
    return slotAliveImpl(s->constraintSlots, id);
    OREO_C_CATCH_RETURN(0)
}

int oreo_ctx_sketch_point_slot_count(OreoSketch s) {
    OREO_C_TRY return s ? static_cast<int>(s->pointSlots.size()) : 0;
    OREO_C_CATCH_RETURN(0)
}
int oreo_ctx_sketch_line_slot_count(OreoSketch s) {
    OREO_C_TRY return s ? static_cast<int>(s->lineSlots.size()) : 0;
    OREO_C_CATCH_RETURN(0)
}
int oreo_ctx_sketch_circle_slot_count(OreoSketch s) {
    OREO_C_TRY return s ? static_cast<int>(s->circleSlots.size()) : 0;
    OREO_C_CATCH_RETURN(0)
}
int oreo_ctx_sketch_arc_slot_count(OreoSketch s) {
    OREO_C_TRY return s ? static_cast<int>(s->arcSlots.size()) : 0;
    OREO_C_CATCH_RETURN(0)
}
int oreo_ctx_sketch_constraint_slot_count(OreoSketch s) {
    OREO_C_TRY return s ? static_cast<int>(s->constraintSlots.size()) : 0;
    OREO_C_CATCH_RETURN(0)
}

int oreo_ctx_sketch_point_live_count(OreoSketch s) {
    OREO_C_TRY return s ? slotCountLiveImpl(s->pointSlots) : 0;
    OREO_C_CATCH_RETURN(0)
}
int oreo_ctx_sketch_line_live_count(OreoSketch s) {
    OREO_C_TRY return s ? slotCountLiveImpl(s->lineSlots) : 0;
    OREO_C_CATCH_RETURN(0)
}
int oreo_ctx_sketch_circle_live_count(OreoSketch s) {
    OREO_C_TRY return s ? slotCountLiveImpl(s->circleSlots) : 0;
    OREO_C_CATCH_RETURN(0)
}
int oreo_ctx_sketch_arc_live_count(OreoSketch s) {
    OREO_C_TRY return s ? slotCountLiveImpl(s->arcSlots) : 0;
    OREO_C_CATCH_RETURN(0)
}
int oreo_ctx_sketch_constraint_live_count(OreoSketch s) {
    OREO_C_TRY return s ? slotCountLiveImpl(s->constraintSlots) : 0;
    OREO_C_CATCH_RETURN(0)
}

// ============================================================
// Context-aware feature-edit API
// ============================================================

struct OreoFeatureBuilder_T {
    oreo::Feature feature;
};

struct OreoFeatureTree_T {
    std::shared_ptr<oreo::KernelContext> ctxBinding;
    std::unique_ptr<oreo::FeatureTree>   tree;
};

namespace {

oreo::KernelContext& treeCtx(OreoFeatureTree t) {
    return (t && t->ctxBinding) ? *t->ctxBinding : *internalDefaultCtx();
}

oreo::DiagnosticCollector& treeDiag(OreoFeatureTree t) {
    return treeCtx(t).diag();
}

bool builderValid(OreoFeatureBuilder b, const char* name, OreoErrorCode& outErr) {
    if (!b) { outErr = OREO_INVALID_INPUT; return false; }
    if (!name) { outErr = OREO_INVALID_INPUT; return false; }
    return true;
}

} // anonymous namespace

OreoFeatureBuilder oreo_feature_builder_create(const char* id, const char* type) {
    OREO_C_TRY
    auto* b = new OreoFeatureBuilder_T{};
    if (id)   b->feature.id   = id;
    if (type) b->feature.type = type;
    return b;
    OREO_C_CATCH_RETURN(nullptr)
}

void oreo_feature_builder_free(OreoFeatureBuilder b) {
    OREO_C_TRY
    delete b;
    OREO_C_CATCH_VOID
}

void oreo_feature_builder_reset(OreoFeatureBuilder b, const char* id, const char* type) {
    OREO_C_TRY
    if (!b) return;
    b->feature = oreo::Feature{};
    if (id)   b->feature.id   = id;
    if (type) b->feature.type = type;
    OREO_C_CATCH_VOID
}

int oreo_feature_builder_set_double(OreoFeatureBuilder b, const char* name, double v) {
    OREO_C_TRY
    OreoErrorCode err = OREO_OK;
    if (!builderValid(b, name, err)) return err;
    b->feature.params[name] = v;
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_feature_builder_set_int(OreoFeatureBuilder b, const char* name, int v) {
    OREO_C_TRY
    OreoErrorCode err = OREO_OK;
    if (!builderValid(b, name, err)) return err;
    b->feature.params[name] = v;
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_feature_builder_set_bool(OreoFeatureBuilder b, const char* name, int v) {
    OREO_C_TRY
    OreoErrorCode err = OREO_OK;
    if (!builderValid(b, name, err)) return err;
    b->feature.params[name] = (v != 0);
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_feature_builder_set_string(OreoFeatureBuilder b, const char* name, const char* v) {
    OREO_C_TRY
    OreoErrorCode err = OREO_OK;
    if (!builderValid(b, name, err)) return err;
    b->feature.params[name] = std::string(v ? v : "");
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_feature_builder_set_vec(OreoFeatureBuilder b, const char* name,
                                  double x, double y, double z) {
    OREO_C_TRY
    OreoErrorCode err = OREO_OK;
    if (!builderValid(b, name, err)) return err;
    b->feature.params[name] = gp_Vec(x, y, z);
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_feature_builder_set_pnt(OreoFeatureBuilder b, const char* name,
                                  double x, double y, double z) {
    OREO_C_TRY
    OreoErrorCode err = OREO_OK;
    if (!builderValid(b, name, err)) return err;
    b->feature.params[name] = gp_Pnt(x, y, z);
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_feature_builder_set_dir(OreoFeatureBuilder b, const char* name,
                                  double x, double y, double z) {
    OREO_C_TRY
    OreoErrorCode err = OREO_OK;
    if (!builderValid(b, name, err)) return err;
    // gp_Dir constructor throws if magnitude is below tolerance.
    gp_Vec v(x, y, z);
    if (v.Magnitude() < 1e-12) {
        return OREO_INVALID_INPUT;
    }
    b->feature.params[name] = gp_Dir(x, y, z);
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_feature_builder_set_ax1(OreoFeatureBuilder b, const char* name,
                                  double px, double py, double pz,
                                  double dx, double dy, double dz) {
    OREO_C_TRY
    OreoErrorCode err = OREO_OK;
    if (!builderValid(b, name, err)) return err;
    gp_Vec v(dx, dy, dz);
    if (v.Magnitude() < 1e-12) return OREO_INVALID_INPUT;
    b->feature.params[name] = gp_Ax1(gp_Pnt(px, py, pz), gp_Dir(dx, dy, dz));
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_feature_builder_set_ax2(OreoFeatureBuilder b, const char* name,
                                  double px, double py, double pz,
                                  double nx, double ny, double nz) {
    OREO_C_TRY
    OreoErrorCode err = OREO_OK;
    if (!builderValid(b, name, err)) return err;
    gp_Vec v(nx, ny, nz);
    if (v.Magnitude() < 1e-12) return OREO_INVALID_INPUT;
    b->feature.params[name] = gp_Ax2(gp_Pnt(px, py, pz), gp_Dir(nx, ny, nz));
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_feature_builder_set_pln(OreoFeatureBuilder b, const char* name,
                                  double px, double py, double pz,
                                  double nx, double ny, double nz) {
    OREO_C_TRY
    OreoErrorCode err = OREO_OK;
    if (!builderValid(b, name, err)) return err;
    gp_Vec v(nx, ny, nz);
    if (v.Magnitude() < 1e-12) return OREO_INVALID_INPUT;
    b->feature.params[name] = gp_Pln(gp_Pnt(px, py, pz), gp_Dir(nx, ny, nz));
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_feature_builder_set_ref(OreoFeatureBuilder b, const char* name,
                                  const char* featureId,
                                  const char* elementName,
                                  const char* elementType) {
    OREO_C_TRY
    OreoErrorCode err = OREO_OK;
    if (!builderValid(b, name, err)) return err;
    oreo::ElementRef r;
    r.featureId   = featureId   ? featureId   : "";
    r.elementName = elementName ? elementName : "";
    r.elementType = elementType ? elementType : "";
    b->feature.params[name] = r;
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_feature_builder_add_ref(OreoFeatureBuilder b, const char* name,
                                  const char* featureId,
                                  const char* elementName,
                                  const char* elementType) {
    OREO_C_TRY
    OreoErrorCode err = OREO_OK;
    if (!builderValid(b, name, err)) return err;
    oreo::ElementRef r;
    r.featureId   = featureId   ? featureId   : "";
    r.elementName = elementName ? elementName : "";
    r.elementType = elementType ? elementType : "";
    auto it = b->feature.params.find(name);
    if (it == b->feature.params.end()) {
        b->feature.params[name] = std::vector<oreo::ElementRef>{r};
    } else {
        auto* lst = std::get_if<std::vector<oreo::ElementRef>>(&it->second);
        if (!lst) {
            // Wrong existing type — caller mixed set_ref and add_ref on the same name.
            return OREO_INVALID_STATE;
        }
        lst->push_back(r);
    }
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

OreoFeatureTree oreo_ctx_feature_tree_create(OreoContext ctx) {
    OREO_C_TRY
    if (!ctx || !ctx->ctx) {
        diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context");
        return nullptr;
    }
    auto* t = new OreoFeatureTree_T{};
    t->ctxBinding = ctx->ctx;
    t->tree = std::make_unique<oreo::FeatureTree>(ctx->ctx);
    return t;
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

void oreo_ctx_feature_tree_free(OreoFeatureTree t) {
    OREO_C_TRY
    delete t;
    OREO_C_CATCH_VOID
}

int oreo_ctx_feature_tree_add(OreoFeatureTree t, OreoFeatureBuilder b) {
    OREO_C_TRY
    if (!t || !t->tree || !b) return OREO_INVALID_INPUT;
    if (b->feature.id.empty() || b->feature.type.empty()) {
        treeDiag(t).error(oreo::ErrorCode::INVALID_INPUT,
            "Feature builder requires non-empty id and type");
        return OREO_INVALID_INPUT;
    }
    if (t->tree->getFeature(b->feature.id) != nullptr) {
        treeDiag(t).error(oreo::ErrorCode::INVALID_STATE,
            "Duplicate feature id: " + b->feature.id);
        return OREO_INVALID_STATE;
    }
    t->tree->addFeature(b->feature);
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_remove(OreoFeatureTree t, const char* featureId) {
    OREO_C_TRY
    if (!t || !t->tree || !featureId) return OREO_INVALID_INPUT;
    return t->tree->removeFeature(featureId) ? OREO_OK : OREO_INVALID_INPUT;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_suppress(OreoFeatureTree t,
                                    const char* featureId, int suppress) {
    OREO_C_TRY
    if (!t || !t->tree || !featureId) return OREO_INVALID_INPUT;
    if (t->tree->getFeature(featureId) == nullptr) return OREO_INVALID_INPUT;
    t->tree->suppressFeature(featureId, suppress != 0);
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_set_param_double(OreoFeatureTree t,
                                            const char* featureId,
                                            const char* paramName,
                                            double v) {
    OREO_C_TRY
    if (!t || !t->tree || !featureId || !paramName) return OREO_INVALID_INPUT;
    if (t->tree->getFeature(featureId) == nullptr) return OREO_INVALID_INPUT;
    t->tree->updateParameter(featureId, paramName, v);
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_set_param_int(OreoFeatureTree t,
                                         const char* featureId,
                                         const char* paramName,
                                         int v) {
    OREO_C_TRY
    if (!t || !t->tree || !featureId || !paramName) return OREO_INVALID_INPUT;
    if (t->tree->getFeature(featureId) == nullptr) return OREO_INVALID_INPUT;
    t->tree->updateParameter(featureId, paramName, v);
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_set_param_bool(OreoFeatureTree t,
                                          const char* featureId,
                                          const char* paramName,
                                          int v) {
    OREO_C_TRY
    if (!t || !t->tree || !featureId || !paramName) return OREO_INVALID_INPUT;
    if (t->tree->getFeature(featureId) == nullptr) return OREO_INVALID_INPUT;
    t->tree->updateParameter(featureId, paramName, v != 0);
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_set_param_string(OreoFeatureTree t,
                                            const char* featureId,
                                            const char* paramName,
                                            const char* v) {
    OREO_C_TRY
    if (!t || !t->tree || !featureId || !paramName) return OREO_INVALID_INPUT;
    if (t->tree->getFeature(featureId) == nullptr) return OREO_INVALID_INPUT;
    t->tree->updateParameter(featureId, paramName, std::string(v ? v : ""));
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_set_param_vec(OreoFeatureTree t,
                                         const char* featureId,
                                         const char* paramName,
                                         double x, double y, double z) {
    OREO_C_TRY
    if (!t || !t->tree || !featureId || !paramName) return OREO_INVALID_INPUT;
    if (t->tree->getFeature(featureId) == nullptr) return OREO_INVALID_INPUT;
    t->tree->updateParameter(featureId, paramName, gp_Vec(x, y, z));
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

// extern "C++" escapes the surrounding extern "C" block (opened at the
// top of the public-API section above). The helpers below return /
// take std::vector and std::string, which would be ill-formed under
// C linkage.
extern "C++" {
namespace {

// Walk the feature list collecting non-OK entries — the canonical
// "broken features" view shared by broken_count / broken_id / broken_message.
std::vector<const oreo::Feature*> collectBrokenFeatures(OreoFeatureTree t) {
    std::vector<const oreo::Feature*> out;
    if (!t || !t->tree) return out;
    for (const auto& f : t->tree->features()) {
        if (f.status != oreo::FeatureStatus::OK
            && f.status != oreo::FeatureStatus::Suppressed
            && f.status != oreo::FeatureStatus::NotExecuted) {
            out.push_back(&f);
        }
    }
    return out;
}

// Size-probe write helper — copies `s` into (buf, buflen) honouring the
// documented contract (returns BUFFER_TOO_SMALL when buflen is short).
int writeSizeProbe(const std::string& s, char* buf, size_t buflen, size_t* needed) {
    if (!needed) return OREO_INVALID_INPUT;
    *needed = s.size();
    if (buf == nullptr && buflen == 0) return OREO_OK;
    if (buf == nullptr) return OREO_INVALID_INPUT;
    if (buflen < s.size() + 1) {
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

} // anonymous namespace
} // extern "C++"

int oreo_ctx_feature_tree_broken_count(OreoFeatureTree t) {
    OREO_C_TRY
    return static_cast<int>(collectBrokenFeatures(t).size());
    OREO_C_CATCH_RETURN(0)
}

int oreo_ctx_feature_tree_broken_id(OreoFeatureTree t, int index,
                                     char* buf, size_t buflen, size_t* needed) {
    OREO_C_TRY
    auto broken = collectBrokenFeatures(t);
    if (index < 0 || index >= static_cast<int>(broken.size())) return OREO_OUT_OF_RANGE;
    return writeSizeProbe(broken[index]->id, buf, buflen, needed);
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_broken_message(OreoFeatureTree t, int index,
                                          char* buf, size_t buflen, size_t* needed) {
    OREO_C_TRY
    auto broken = collectBrokenFeatures(t);
    if (index < 0 || index >= static_cast<int>(broken.size())) return OREO_OUT_OF_RANGE;
    return writeSizeProbe(broken[index]->errorMessage, buf, buflen, needed);
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_move(OreoFeatureTree t,
                                const char* featureId, int newIndex) {
    OREO_C_TRY
    if (!t || !t->tree || !featureId) return OREO_INVALID_INPUT;
    return t->tree->moveFeature(featureId, newIndex) ? OREO_OK : OREO_INVALID_INPUT;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_replace_reference(OreoFeatureTree t,
                                             const char* oldFeatureId,
                                             const char* newFeatureId) {
    OREO_C_TRY
    if (!t || !t->tree || !oldFeatureId || !newFeatureId) return OREO_INVALID_INPUT;
    return t->tree->replaceReference(oldFeatureId, newFeatureId);
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_count(OreoFeatureTree t) {
    OREO_C_TRY
    if (!t || !t->tree) return 0;
    return t->tree->featureCount();
    OREO_C_CATCH_RETURN(0)
}

int oreo_ctx_feature_tree_validate(OreoFeatureTree t) {
    OREO_C_TRY
    if (!t || !t->tree) return OREO_INVALID_INPUT;
    int firstError = OREO_OK;
    // FeatureTree::features() returns const&; validateFeature mutates
    // status/errorMessage on the Feature. We snapshot a mutable copy
    // per feature so the tree's stored state is left untouched — the
    // tree's own status updates happen during replay.
    for (auto& f : t->tree->features()) {
        oreo::Feature copy = f;
        if (!oreo::validateFeature(treeCtx(t), copy)) {
            if (firstError == OREO_OK) firstError = OREO_INVALID_INPUT;
        }
    }
    return firstError;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

OreoSolid oreo_ctx_feature_tree_replay(OreoFeatureTree t) {
    OREO_C_TRY
    if (!t || !t->tree) return nullptr;
    auto result = t->tree->replay();
    if (result.isNull()) return nullptr;
    return new OreoSolid_T{std::move(result)};
    OREO_C_CATCH_RETURN(nullptr)
}

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
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    if (!solid) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    auto result = oreo::tessellate(*ctx->ctx, solid->ns, {linear_deflection, angular_deflection_deg});
    if (!result.ok()) return nullptr;
    auto mesh = std::move(result).value();
    if (mesh.empty()) return nullptr;
    return new OreoMesh_T{std::move(mesh)};
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
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
