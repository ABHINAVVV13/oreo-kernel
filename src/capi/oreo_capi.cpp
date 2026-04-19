// SPDX-License-Identifier: LGPL-2.1-or-later

// oreo_capi.cpp — C API wrapper over C++ internals.

#include "oreo_kernel.h"

#include "core/kernel_context.h"
#include "core/diagnostic.h"
#include "core/schema.h"
#include "core/shape_identity.h"
#include "core/shape_identity_v1.h"
#include "geometry/oreo_geometry.h"
#include "io/oreo_iges.h"
#include "io/oreo_mesh_io.h"
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
#include "feature/config.h"
#include "feature/part_studio.h"
#include "feature/workspace.h"
#include "feature/merge.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS.hxx>
#include <gp_Vec.hxx>

#include <cmath>
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

    // Construction-geometry filter: an entity flagged `construction=true`
    // participates in the solver (already included in `v` — constraints
    // such as "point on centerline" or "tangent to construction circle"
    // rely on these entities) but is stripped before wire emission.
    // Onshape calls this "construction geometry"; SolidWorks calls it
    // "For Construction". Points are never emitted into wires by
    // oreo::sketchToWire so we only filter the curve families.
    std::vector<oreo::SketchLine>   realLines;
    std::vector<oreo::SketchCircle> realCircles;
    std::vector<oreo::SketchArc>    realArcs;
    realLines.reserve(v.lines.size());
    realCircles.reserve(v.circles.size());
    realArcs.reserve(v.arcs.size());
    for (const auto& ln : v.lines)   if (!ln.construction) realLines.push_back(ln);
    for (const auto& ci : v.circles) if (!ci.construction) realCircles.push_back(ci);
    for (const auto& ar : v.arcs)    if (!ar.construction) realArcs.push_back(ar);

    auto r = oreo::sketchToWire(ctx, realLines, realCircles, realArcs);
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

    // Use the by-value lastErrorOpt() accessor to avoid dangling-pointer
    // risk if a concurrent diagnostic report() resizes the underlying
    // vector between the read and the field accesses.
    auto lastErr = ctx.diag().lastErrorOpt();
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
    auto last = ctx->ctx->diag().lastErrorOpt();
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

// ─── STL (ctx-aware) ─────────────────────────────────────────

OreoSolid oreo_ctx_import_stl(OreoContext ctx, const uint8_t* data, size_t len) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    if (!data) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null data pointer"); return nullptr; }
    auto r = oreo::importStl(*ctx->ctx, data, len);
    if (!r.ok()) return nullptr;
    return wrapSolid(std::move(r).value());
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoSolid oreo_ctx_import_stl_file(OreoContext ctx, const char* path) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    if (!path) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null path pointer"); return nullptr; }
    auto r = oreo::importStlFile(*ctx->ctx, path);
    if (!r.ok()) return nullptr;
    return wrapSolid(std::move(r).value());
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

int oreo_ctx_export_stl_file(OreoContext ctx, OreoSolid solid, const char* path,
                              int stl_format, double linear_deflection_mm,
                              double angular_deflection_deg, int heal_before_tessellate) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return 0; }
    if (!solid || !path) {
        ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null solid or path handle");
        return 0;
    }
    oreo::StlExportOptions opts;
    opts.format               = (stl_format == 1) ? oreo::StlFormat::Ascii : oreo::StlFormat::Binary;
    opts.linearDeflection     = linear_deflection_mm;
    opts.angularDeflection    = angular_deflection_deg;
    opts.healBeforeTessellate = (heal_before_tessellate != 0);
    auto r = oreo::exportStlFile(*ctx->ctx, solid->ns, path, opts);
    if (!r.ok()) return 0;
    return r.value() ? 1 : 0;
    OREO_C_CATCH_RETURN_CTX(ctx, 0)
}

// ─── IGES (ctx-aware) ─────────────────────────────────────────

OreoSolid oreo_ctx_import_iges(OreoContext ctx, const uint8_t* data, size_t len) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    if (!data) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null data pointer"); return nullptr; }
    auto r = oreo::importIges(*ctx->ctx, data, len);
    if (!r.ok()) return nullptr;
    return wrapSolid(std::move(r).value());
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoSolid oreo_ctx_import_iges_file(OreoContext ctx, const char* path) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    if (!path) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null path pointer"); return nullptr; }
    auto r = oreo::importIgesFile(*ctx->ctx, path);
    if (!r.ok()) return nullptr;
    return wrapSolid(std::move(r).value());
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

int oreo_ctx_export_iges_file(OreoContext ctx, OreoSolid solids[], int n, const char* path) {
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
    auto r = oreo::exportIgesFile(*ctx->ctx, shapes, path);
    if (!r.ok()) return 0;
    return r.value() ? 1 : 0;
    OREO_C_CATCH_RETURN_CTX(ctx, 0)
}

// ─── 3MF export (ctx-aware) ───────────────────────────────────

int oreo_ctx_export_3mf_file(OreoContext ctx, OreoSolid solid, const char* path,
                              double linear_deflection_mm, double angular_deflection_deg,
                              const char* object_name, const char* unit) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return 0; }
    if (!solid || !path) {
        ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null solid or path handle");
        return 0;
    }
    oreo::ThreeMfExportOptions opts;
    opts.linearDeflection  = linear_deflection_mm;
    opts.angularDeflection = angular_deflection_deg;
    if (object_name) opts.objectName = object_name;
    if (unit)        opts.unit       = unit;
    auto r = oreo::exportThreeMfFile(*ctx->ctx, solid->ns, path, opts);
    if (!r.ok()) return 0;
    return r.value() ? 1 : 0;
    OREO_C_CATCH_RETURN_CTX(ctx, 0)
}

// ============================================================
// Context-aware extended primitives + geometry operations
// ============================================================
//
// These wrap the remaining 20+ ops that previously only had a legacy
// singleton surface, closing the gap documented in CHANGELOG.md's
// "Ctx-aware C API still incomplete" list. Every wrapper follows the
// same defensive pattern:
//
//   1. Null-ctx → diagFor(ctx) + return default
//   2. Null operand handles → ctx->ctx->diag().error + return default
//   3. Range checks on counts (n >= 0) + null-pointer-with-n>0
//   4. Exception boundary via OREO_C_TRY / OREO_C_CATCH_RETURN_CTX
//
// The behaviour must match the legacy singleton counterpart byte-for-byte
// apart from the context plumbing — test_capi_consumer exercises both
// surfaces to lock this invariant.

// ─── Extended primitives ──────────────────────────────────────

OreoSolid oreo_ctx_make_cone(OreoContext ctx, double r1, double r2, double height) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    return wrapSolid(oreo::makeCone(*ctx->ctx, r1, r2, height));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoSolid oreo_ctx_make_torus(OreoContext ctx, double major_r, double minor_r) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    return wrapSolid(oreo::makeTorus(*ctx->ctx, major_r, minor_r));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoSolid oreo_ctx_make_wedge(OreoContext ctx, double dx, double dy, double dz, double ltx) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    return wrapSolid(oreo::makeWedge(*ctx->ctx, dx, dy, dz, ltx));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

// ─── Extended geometry operations ─────────────────────────────

OreoSolid oreo_ctx_revolve(OreoContext ctx, OreoSolid base,
                            double ax, double ay, double az,
                            double dx, double dy, double dz,
                            double angle_rad) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    if (!base) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null base handle"); return nullptr; }
    gp_Ax1 axis(gp_Pnt(ax, ay, az), gp_Dir(dx, dy, dz));
    return wrapSolid(oreo::revolve(*ctx->ctx, base->ns, axis, angle_rad));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoSolid oreo_ctx_chamfer(OreoContext ctx, OreoSolid solid,
                            OreoEdge edges[], int n, double distance) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    if (!solid) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null solid handle"); return nullptr; }
    if (n < 0) {
        ctx->ctx->diag().error(oreo::ErrorCode::OUT_OF_RANGE, "n must be >= 0");
        return nullptr;
    }
    if (n > 0 && !edges) {
        ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "edges pointer is NULL but n > 0");
        return nullptr;
    }
    std::vector<oreo::NamedEdge> edgeVec;
    edgeVec.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        if (edges[i]) edgeVec.push_back({oreo::IndexedName("Edge", i + 1), edges[i]->ns.shape()});
    }
    return wrapSolid(oreo::chamfer(*ctx->ctx, solid->ns, edgeVec, distance));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoSolid oreo_ctx_boolean_intersect(OreoContext ctx, OreoSolid a, OreoSolid b) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    if (!a || !b) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null operand handle"); return nullptr; }
    return wrapSolid(oreo::booleanIntersect(*ctx->ctx, a->ns, b->ns));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoSolid oreo_ctx_shell(OreoContext ctx, OreoSolid solid,
                          OreoFace faces[], int n, double thickness) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    if (!solid) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null solid handle"); return nullptr; }
    if (n < 0) {
        ctx->ctx->diag().error(oreo::ErrorCode::OUT_OF_RANGE, "n must be >= 0");
        return nullptr;
    }
    if (n > 0 && !faces) {
        ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "faces pointer is NULL but n > 0");
        return nullptr;
    }
    std::vector<oreo::NamedFace> faceVec;
    faceVec.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        if (faces[i]) faceVec.push_back({oreo::IndexedName("Face", i + 1), faces[i]->ns.shape()});
    }
    return wrapSolid(oreo::shell(*ctx->ctx, solid->ns, faceVec, thickness));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoSolid oreo_ctx_loft(OreoContext ctx, OreoWire profiles[], int n, int make_solid) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    if (n < 0) {
        ctx->ctx->diag().error(oreo::ErrorCode::OUT_OF_RANGE, "n must be >= 0");
        return nullptr;
    }
    if (n > 0 && !profiles) {
        ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "profiles pointer is NULL but n > 0");
        return nullptr;
    }
    std::vector<oreo::NamedShape> profVec;
    profVec.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        if (profiles[i]) profVec.push_back(profiles[i]->ns);
    }
    return wrapSolid(oreo::loft(*ctx->ctx, profVec, make_solid != 0));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoSolid oreo_ctx_sweep(OreoContext ctx, OreoWire profile, OreoWire path) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    if (!profile || !path) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null profile or path handle"); return nullptr; }
    return wrapSolid(oreo::sweep(*ctx->ctx, profile->ns, path->ns));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoSolid oreo_ctx_mirror(OreoContext ctx, OreoSolid solid,
                           double px, double py, double pz,
                           double nx, double ny, double nz) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    if (!solid) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null solid handle"); return nullptr; }
    gp_Ax2 plane(gp_Pnt(px, py, pz), gp_Dir(nx, ny, nz));
    return wrapSolid(oreo::mirror(*ctx->ctx, solid->ns, plane));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoSolid oreo_ctx_pattern_linear(OreoContext ctx, OreoSolid solid,
                                   double dx, double dy, double dz,
                                   int count, double spacing) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    if (!solid) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null solid handle"); return nullptr; }
    return wrapSolid(oreo::patternLinear(*ctx->ctx, solid->ns, gp_Vec(dx, dy, dz), count, spacing));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoSolid oreo_ctx_pattern_circular(OreoContext ctx, OreoSolid solid,
                                     double ax, double ay, double az,
                                     double dx, double dy, double dz,
                                     int count, double total_angle_rad) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    if (!solid) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null solid handle"); return nullptr; }
    gp_Ax1 axis(gp_Pnt(ax, ay, az), gp_Dir(dx, dy, dz));
    return wrapSolid(oreo::patternCircular(*ctx->ctx, solid->ns, axis, count, total_angle_rad));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoSolid oreo_ctx_draft(OreoContext ctx, OreoSolid solid,
                          OreoFace faces[], int n,
                          double angle_deg,
                          double pull_dx, double pull_dy, double pull_dz) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    if (!solid) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null solid handle"); return nullptr; }
    if (n < 0) {
        ctx->ctx->diag().error(oreo::ErrorCode::OUT_OF_RANGE, "n must be >= 0");
        return nullptr;
    }
    if (n > 0 && !faces) {
        ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "faces pointer is NULL but n > 0");
        return nullptr;
    }
    std::vector<oreo::NamedFace> faceVec;
    faceVec.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        if (faces[i]) faceVec.push_back({oreo::IndexedName("Face", i + 1), faces[i]->ns.shape()});
    }
    return wrapSolid(oreo::draft(*ctx->ctx, solid->ns, faceVec, angle_deg, gp_Dir(pull_dx, pull_dy, pull_dz)));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoSolid oreo_ctx_hole(OreoContext ctx, OreoSolid solid, OreoFace face,
                         double cx, double cy, double cz,
                         double diameter, double depth) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    if (!solid) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null solid handle"); return nullptr; }
    oreo::NamedFace nf = {oreo::IndexedName("Face", 1), face ? face->ns.shape() : TopoDS_Shape()};
    return wrapSolid(oreo::hole(*ctx->ctx, solid->ns, nf, gp_Pnt(cx, cy, cz), diameter, depth));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoSolid oreo_ctx_pocket(OreoContext ctx, OreoSolid solid, OreoSolid profile, double depth) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    if (!solid || !profile) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null solid or profile handle"); return nullptr; }
    return wrapSolid(oreo::pocket(*ctx->ctx, solid->ns, profile->ns, depth));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoSolid oreo_ctx_rib(OreoContext ctx, OreoSolid solid, OreoSolid profile,
                        double dx, double dy, double dz, double thickness) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    if (!solid || !profile) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null solid or profile handle"); return nullptr; }
    return wrapSolid(oreo::rib(*ctx->ctx, solid->ns, profile->ns, gp_Dir(dx, dy, dz), thickness));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoSolid oreo_ctx_offset(OreoContext ctx, OreoSolid solid, double distance) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    if (!solid) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null solid handle"); return nullptr; }
    return wrapSolid(oreo::offset(*ctx->ctx, solid->ns, distance));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoSolid oreo_ctx_thicken(OreoContext ctx, OreoSolid shell, double thickness) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    if (!shell) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null shell handle"); return nullptr; }
    return wrapSolid(oreo::thicken(*ctx->ctx, shell->ns, thickness));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoSolid oreo_ctx_split_body(OreoContext ctx, OreoSolid solid,
                               double px, double py, double pz,
                               double nx, double ny, double nz) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    if (!solid) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null solid handle"); return nullptr; }
    return wrapSolid(oreo::splitBody(*ctx->ctx, solid->ns, gp_Pln(gp_Pnt(px, py, pz), gp_Dir(nx, ny, nz))));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoSolid oreo_ctx_fillet_variable(OreoContext ctx, OreoSolid solid, OreoEdge edge,
                                    double start_radius, double end_radius) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    if (!solid || !edge) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null solid or edge handle"); return nullptr; }
    oreo::NamedEdge ne = {oreo::IndexedName("Edge", 1), edge->ns.shape()};
    return wrapSolid(oreo::filletVariable(*ctx->ctx, solid->ns, ne, start_radius, end_radius));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoSolid oreo_ctx_make_face_from_wire(OreoContext ctx, OreoWire wire) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    if (!wire) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null wire handle"); return nullptr; }
    return wrapSolid(oreo::makeFaceFromWire(*ctx->ctx, wire->ns));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoSolid oreo_ctx_combine(OreoContext ctx, OreoSolid shapes[], int n) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    if (n < 0) {
        ctx->ctx->diag().error(oreo::ErrorCode::OUT_OF_RANGE, "n must be >= 0");
        return nullptr;
    }
    if (n > 0 && !shapes) {
        ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "shapes pointer is NULL but n > 0");
        return nullptr;
    }
    std::vector<oreo::NamedShape> vec;
    vec.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        if (shapes[i]) vec.push_back(shapes[i]->ns);
    }
    return wrapSolid(oreo::combine(*ctx->ctx, vec));
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

// ─── Extended ctx-aware queries ──────────────────────────────

OreoFace oreo_ctx_get_face(OreoContext ctx, OreoSolid solid, int index) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    if (!solid) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null solid handle"); return nullptr; }
    if (index < 1) {
        ctx->ctx->diag().error(oreo::ErrorCode::OUT_OF_RANGE, "index must be >= 1");
        return nullptr;
    }
    TopoDS_Shape face = solid->ns.getSubShape(TopAbs_FACE, index);
    if (face.IsNull()) return nullptr;
    return new OreoFace_T{oreo::NamedShape(face, oreo::ShapeIdentity{})};
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

OreoEdge oreo_ctx_get_edge(OreoContext ctx, OreoSolid solid, int index) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return nullptr; }
    if (!solid) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null solid handle"); return nullptr; }
    if (index < 1) {
        ctx->ctx->diag().error(oreo::ErrorCode::OUT_OF_RANGE, "index must be >= 1");
        return nullptr;
    }
    TopoDS_Shape edge = solid->ns.getSubShape(TopAbs_EDGE, index);
    if (edge.IsNull()) return nullptr;
    return new OreoEdge_T{oreo::NamedShape(edge, oreo::ShapeIdentity{})};
    OREO_C_CATCH_RETURN_CTX(ctx, nullptr)
}

double oreo_ctx_measure_distance(OreoContext ctx, OreoSolid a, OreoSolid b) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return -1.0; }
    if (!a || !b) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null operand handle"); return -1.0; }
    auto r = oreo::measureDistance(*ctx->ctx, a->ns, b->ns);
    if (!r.ok()) return -1.0;
    return r.value();
    OREO_C_CATCH_RETURN_CTX(ctx, -1.0)
}

OreoBBox oreo_ctx_footprint(OreoContext ctx, OreoSolid before, OreoSolid after) {
    OREO_C_TRY
    if (!ctx) { diagFor(ctx).error(oreo::ErrorCode::INVALID_INPUT, "Null context handle"); return {}; }
    if (!before || !after) { ctx->ctx->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null before or after handle"); return {}; }
    auto r = oreo::footprint(*ctx->ctx, before->ns, after->ns);
    if (!r.ok()) return {};
    auto bb = r.value();
    return {bb.xmin, bb.ymin, bb.zmin, bb.xmax, bb.ymax, bb.zmax};
    OREO_C_CATCH_RETURN_CTX(ctx, OreoBBox{})
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

// ─── STL / IGES / 3MF legacy wrappers ────────────────────────
//
// Mirror the STEP legacy surface. Every call routes through
// internalDefaultCtx() — NOT safe for multi-tenant server processes.
// See the CMakeLists.txt OREO_ENABLE_LEGACY_API option for the server
// build story.

OreoSolid oreo_import_stl(const uint8_t* data, size_t len) {
    OREO_C_TRY
    if (!data) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    auto r = oreo::importStl(*internalDefaultCtx(), data, len);
    if (!r.ok()) return nullptr;
    return wrapSolid(std::move(r).value());
    OREO_C_CATCH_RETURN(nullptr)
}
OreoSolid oreo_import_stl_file(const char* path) {
    OREO_C_TRY
    if (!path) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null path"); return nullptr; }
    auto r = oreo::importStlFile(*internalDefaultCtx(), path);
    if (!r.ok()) return nullptr;
    return wrapSolid(std::move(r).value());
    OREO_C_CATCH_RETURN(nullptr)
}
int oreo_export_stl_file(OreoSolid solid, const char* path,
                          int stl_format, double linear_deflection_mm,
                          double angular_deflection_deg, int heal_before_tessellate) {
    OREO_C_TRY
    if (!solid || !path) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null solid or path");
        return 0;
    }
    oreo::StlExportOptions opts;
    opts.format               = (stl_format == 1) ? oreo::StlFormat::Ascii : oreo::StlFormat::Binary;
    opts.linearDeflection     = linear_deflection_mm;
    opts.angularDeflection    = angular_deflection_deg;
    opts.healBeforeTessellate = (heal_before_tessellate != 0);
    auto r = oreo::exportStlFile(*internalDefaultCtx(), solid->ns, path, opts);
    return (r.ok() && r.value()) ? 1 : 0;
    OREO_C_CATCH_RETURN(0)
}

OreoSolid oreo_import_iges(const uint8_t* data, size_t len) {
    OREO_C_TRY
    if (!data) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null handle"); return nullptr; }
    auto r = oreo::importIges(*internalDefaultCtx(), data, len);
    if (!r.ok()) return nullptr;
    return wrapSolid(std::move(r).value());
    OREO_C_CATCH_RETURN(nullptr)
}
OreoSolid oreo_import_iges_file(const char* path) {
    OREO_C_TRY
    if (!path) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null path"); return nullptr; }
    auto r = oreo::importIgesFile(*internalDefaultCtx(), path);
    if (!r.ok()) return nullptr;
    return wrapSolid(std::move(r).value());
    OREO_C_CATCH_RETURN(nullptr)
}
int oreo_export_iges_file(OreoSolid solids[], int n, const char* path) {
    OREO_C_TRY
    if (!path) { internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null path"); return 0; }
    if (n < 0) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::OUT_OF_RANGE, "n must be >= 0");
        return 0;
    }
    if (n > 0 && !solids) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "solids pointer is NULL but n > 0");
        return 0;
    }
    std::vector<oreo::NamedShape> shapes;
    shapes.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        if (solids[i]) shapes.push_back(solids[i]->ns);
    }
    auto r = oreo::exportIgesFile(*internalDefaultCtx(), shapes, path);
    return (r.ok() && r.value()) ? 1 : 0;
    OREO_C_CATCH_RETURN(0)
}

int oreo_export_3mf_file(OreoSolid solid, const char* path,
                          double linear_deflection_mm, double angular_deflection_deg,
                          const char* object_name, const char* unit) {
    OREO_C_TRY
    if (!solid || !path) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT, "Null solid or path");
        return 0;
    }
    oreo::ThreeMfExportOptions opts;
    opts.linearDeflection  = linear_deflection_mm;
    opts.angularDeflection = angular_deflection_deg;
    if (object_name) opts.objectName = object_name;
    if (unit)        opts.unit       = unit;
    auto r = oreo::exportThreeMfFile(*internalDefaultCtx(), solid->ns, path, opts);
    return (r.ok() && r.value()) ? 1 : 0;
    OREO_C_CATCH_RETURN(0)
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

// extern "C++" escapes the surrounding extern "C" block so these
// internal helpers can return C++ references (-Wreturn-type-c-linkage
// otherwise). They are never called from C code.
extern "C++" {
namespace {

oreo::KernelContext& sketchCtx(OreoSketch s) {
    return (s && s->ctxBinding) ? *s->ctxBinding : *internalDefaultCtx();
}

oreo::DiagnosticCollector& sketchDiag(OreoSketch s) {
    return sketchCtx(s).diag();
}

} // anonymous namespace
} // extern "C++"

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

// ─── Construction-geometry setters / getters ──────────────────

// Templates inside an anonymous namespace cannot live directly inside
// `extern "C" { ... }` — GCC rejects them with "template with C
// linkage". Wrap in `extern "C++"` to toggle the linkage back for the
// helper block (same idiom as sketchToWireSlots earlier in this file).
extern "C++" {
namespace {
// Shared setter template: validates the slot, flips `construction`, and
// returns OREO_OK. The live/tombstone state of the slot is NOT altered —
// changing construction on a tombstoned slot is a no-op (nothing would
// render anyway), so we reject it as INVALID_INPUT for consistency with
// the rest of the slot API. The binding to ctxBinding for diagnostics
// mirrors every other sketch mutator.
template <typename Slots>
int setConstructionImpl(OreoSketch s, Slots& slots, int id, int flag) {
    if (!s) return OREO_INVALID_INPUT;
    if (id < 0 || id >= static_cast<int>(slots.size())) {
        auto& diag = s->ctxBinding ? s->ctxBinding->diag() : internalDefaultCtx()->diag();
        diag.error(oreo::ErrorCode::OUT_OF_RANGE,
                   std::string("construction setter: id ") + std::to_string(id)
                   + " out of range (slot count " + std::to_string(slots.size()) + ")");
        return OREO_OUT_OF_RANGE;
    }
    if (slots[id].removed) {
        auto& diag = s->ctxBinding ? s->ctxBinding->diag() : internalDefaultCtx()->diag();
        diag.error(oreo::ErrorCode::INVALID_STATE,
                   std::string("construction setter: id ") + std::to_string(id)
                   + " is a tombstoned slot");
        return OREO_INVALID_STATE;
    }
    slots[id].value.construction = (flag != 0);
    return OREO_OK;
}

template <typename Slots>
int isConstructionImpl(OreoSketch s, const Slots& slots, int id) {
    if (!s) return -1;
    if (id < 0 || id >= static_cast<int>(slots.size())) return -1;
    if (slots[id].removed) return -1;
    return slots[id].value.construction ? 1 : 0;
}
} // anonymous namespace
} // extern "C++"

int oreo_ctx_sketch_set_line_construction(OreoSketch s, int id, int is_construction) {
    OREO_C_TRY
    if (!s) return OREO_INVALID_INPUT;
    return setConstructionImpl(s, s->lineSlots, id, is_construction);
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}
int oreo_ctx_sketch_set_circle_construction(OreoSketch s, int id, int is_construction) {
    OREO_C_TRY
    if (!s) return OREO_INVALID_INPUT;
    return setConstructionImpl(s, s->circleSlots, id, is_construction);
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}
int oreo_ctx_sketch_set_arc_construction(OreoSketch s, int id, int is_construction) {
    OREO_C_TRY
    if (!s) return OREO_INVALID_INPUT;
    return setConstructionImpl(s, s->arcSlots, id, is_construction);
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_sketch_line_is_construction(OreoSketch s, int id) {
    OREO_C_TRY
    if (!s) return -1;
    return isConstructionImpl(s, s->lineSlots, id);
    OREO_C_CATCH_RETURN(-1)
}
int oreo_ctx_sketch_circle_is_construction(OreoSketch s, int id) {
    OREO_C_TRY
    if (!s) return -1;
    return isConstructionImpl(s, s->circleSlots, id);
    OREO_C_CATCH_RETURN(-1)
}
int oreo_ctx_sketch_arc_is_construction(OreoSketch s, int id) {
    OREO_C_TRY
    if (!s) return -1;
    return isConstructionImpl(s, s->arcSlots, id);
    OREO_C_CATCH_RETURN(-1)
}

// ============================================================
// Context-aware feature-edit API
// ============================================================

struct OreoFeatureBuilder_T {
    oreo::Feature feature;
};

struct OreoFeatureTree_T {
    std::shared_ptr<oreo::KernelContext> ctxBinding;
    // Owned tree. When this handle wraps a tree the C layer created
    // itself (oreo_ctx_feature_tree_create), `tree` holds the sole
    // live pointer.
    std::unique_ptr<oreo::FeatureTree>   tree;
    // Borrowed tree — used when this handle is a loan from a larger
    // aggregate that owns the tree (PartStudio, Workspace, MergeResult).
    // When non-null, `ptr()` returns this instead of `tree.get()` and
    // the handle's destructor does NOT delete the tree.
    oreo::FeatureTree* borrow = nullptr;

    oreo::FeatureTree*       ptr()       noexcept { return borrow ? borrow : tree.get(); }
    const oreo::FeatureTree* ptr() const noexcept { return borrow ? borrow : tree.get(); }
    bool                     valid() const noexcept { return borrow || tree; }
};

// extern "C++" escapes the surrounding extern "C" block so helpers
// that return C++ references (and accept a bool outparam) don't trip
// -Wreturn-type-c-linkage. They are never called from C code.
extern "C++" {
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
} // extern "C++"

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
    if (!t) return;
    // Borrowed handles are owned by their parent aggregate
    // (PartStudio / Workspace / MergeResult). Freeing one here would
    // leave a dangling unique_ptr on the parent side — detected
    // immediately via double-free when the parent itself is freed.
    // Fail-closed: emit a diagnostic to the default ctx and return
    // without deleting. (See the CONTRACT note on
    // oreo_ctx_part_studio_tree / _workspace_tree / _merge_result_tree
    // in the public header.)
    if (t->borrow != nullptr) {
        internalDefaultCtx()->diag().warning(oreo::ErrorCode::INVALID_STATE,
            "oreo_ctx_feature_tree_free: handle is borrowed from a parent "
            "aggregate; free the parent instead. Ignoring.");
        return;
    }
    delete t;
    OREO_C_CATCH_VOID
}

int oreo_ctx_feature_tree_add(OreoFeatureTree t, OreoFeatureBuilder b) {
    OREO_C_TRY
    if (!t || !t->valid() || !b) return OREO_INVALID_INPUT;
    if (b->feature.id.empty() || b->feature.type.empty()) {
        treeDiag(t).error(oreo::ErrorCode::INVALID_INPUT,
            "Feature builder requires non-empty id and type");
        return OREO_INVALID_INPUT;
    }
    // Duplicate-id check is redundant with addFeature's own guard but
    // the C ABI wants a specific OREO_INVALID_STATE code whereas
    // addFeature raises an INVALID_STATE diagnostic without returning
    // one — keep the explicit check so the caller gets a usable code.
    if (t->ptr()->getFeature(b->feature.id) != nullptr) {
        treeDiag(t).error(oreo::ErrorCode::INVALID_STATE,
            "Duplicate feature id: " + b->feature.id);
        return OREO_INVALID_STATE;
    }
    if (!t->ptr()->addFeature(b->feature)) {
        // Should never hit given the check above, but honouring the
        // [[nodiscard]] return means a future invariant break here
        // surfaces as INTERNAL_ERROR instead of being silently dropped.
        return OREO_INTERNAL_ERROR;
    }
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_remove(OreoFeatureTree t, const char* featureId) {
    OREO_C_TRY
    if (!t || !t->valid() || !featureId) return OREO_INVALID_INPUT;
    return t->ptr()->removeFeature(featureId) ? OREO_OK : OREO_INVALID_INPUT;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_suppress(OreoFeatureTree t,
                                    const char* featureId, int suppress) {
    OREO_C_TRY
    if (!t || !t->valid() || !featureId) return OREO_INVALID_INPUT;
    if (t->ptr()->getFeature(featureId) == nullptr) return OREO_INVALID_INPUT;
    t->ptr()->suppressFeature(featureId, suppress != 0);
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_set_param_double(OreoFeatureTree t,
                                            const char* featureId,
                                            const char* paramName,
                                            double v) {
    OREO_C_TRY
    if (!t || !t->valid() || !featureId || !paramName) return OREO_INVALID_INPUT;
    if (t->ptr()->getFeature(featureId) == nullptr) return OREO_INVALID_INPUT;
    t->ptr()->updateParameter(featureId, paramName, v);
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_set_param_int(OreoFeatureTree t,
                                         const char* featureId,
                                         const char* paramName,
                                         int v) {
    OREO_C_TRY
    if (!t || !t->valid() || !featureId || !paramName) return OREO_INVALID_INPUT;
    if (t->ptr()->getFeature(featureId) == nullptr) return OREO_INVALID_INPUT;
    t->ptr()->updateParameter(featureId, paramName, v);
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_set_param_bool(OreoFeatureTree t,
                                          const char* featureId,
                                          const char* paramName,
                                          int v) {
    OREO_C_TRY
    if (!t || !t->valid() || !featureId || !paramName) return OREO_INVALID_INPUT;
    if (t->ptr()->getFeature(featureId) == nullptr) return OREO_INVALID_INPUT;
    t->ptr()->updateParameter(featureId, paramName, v != 0);
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_set_param_string(OreoFeatureTree t,
                                            const char* featureId,
                                            const char* paramName,
                                            const char* v) {
    OREO_C_TRY
    if (!t || !t->valid() || !featureId || !paramName) return OREO_INVALID_INPUT;
    if (t->ptr()->getFeature(featureId) == nullptr) return OREO_INVALID_INPUT;
    t->ptr()->updateParameter(featureId, paramName, std::string(v ? v : ""));
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_set_param_vec(OreoFeatureTree t,
                                         const char* featureId,
                                         const char* paramName,
                                         double x, double y, double z) {
    OREO_C_TRY
    if (!t || !t->valid() || !featureId || !paramName) return OREO_INVALID_INPUT;
    if (t->ptr()->getFeature(featureId) == nullptr) return OREO_INVALID_INPUT;
    t->ptr()->updateParameter(featureId, paramName, gp_Vec(x, y, z));
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

// ── Geometry / ref / config-ref param updates ───────────────────
//
// Parity with oreo_feature_builder_set_* for already-inserted features,
// so app code can retarget a sketch plane, a face reference list, a
// hole face, etc. without delete-and-recreate. Prior to 2026-04-19
// the only in-place updates were double/int/bool/string/vec — the
// audit flagged this as the last missing parametric-edit surface.

int oreo_ctx_feature_tree_set_param_pnt(OreoFeatureTree t,
                                         const char* featureId,
                                         const char* paramName,
                                         double x, double y, double z) {
    OREO_C_TRY
    if (!t || !t->valid() || !featureId || !paramName) return OREO_INVALID_INPUT;
    if (t->ptr()->getFeature(featureId) == nullptr) return OREO_INVALID_INPUT;
    t->ptr()->updateParameter(featureId, paramName, gp_Pnt(x, y, z));
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_set_param_dir(OreoFeatureTree t,
                                         const char* featureId,
                                         const char* paramName,
                                         double x, double y, double z) {
    OREO_C_TRY
    if (!t || !t->valid() || !featureId || !paramName) return OREO_INVALID_INPUT;
    if (t->ptr()->getFeature(featureId) == nullptr) return OREO_INVALID_INPUT;
    // gp_Dir throws on zero-magnitude input; reject up-front so the
    // caller sees a clean OREO_INVALID_INPUT instead of INTERNAL_ERROR.
    if (gp_Vec(x, y, z).Magnitude() < 1e-12) return OREO_INVALID_INPUT;
    t->ptr()->updateParameter(featureId, paramName, gp_Dir(x, y, z));
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_set_param_ax1(OreoFeatureTree t,
                                         const char* featureId,
                                         const char* paramName,
                                         double px, double py, double pz,
                                         double dx, double dy, double dz) {
    OREO_C_TRY
    if (!t || !t->valid() || !featureId || !paramName) return OREO_INVALID_INPUT;
    if (t->ptr()->getFeature(featureId) == nullptr) return OREO_INVALID_INPUT;
    if (gp_Vec(dx, dy, dz).Magnitude() < 1e-12) return OREO_INVALID_INPUT;
    t->ptr()->updateParameter(featureId, paramName,
        gp_Ax1(gp_Pnt(px, py, pz), gp_Dir(dx, dy, dz)));
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_set_param_ax2(OreoFeatureTree t,
                                         const char* featureId,
                                         const char* paramName,
                                         double px, double py, double pz,
                                         double nx, double ny, double nz) {
    OREO_C_TRY
    if (!t || !t->valid() || !featureId || !paramName) return OREO_INVALID_INPUT;
    if (t->ptr()->getFeature(featureId) == nullptr) return OREO_INVALID_INPUT;
    if (gp_Vec(nx, ny, nz).Magnitude() < 1e-12) return OREO_INVALID_INPUT;
    t->ptr()->updateParameter(featureId, paramName,
        gp_Ax2(gp_Pnt(px, py, pz), gp_Dir(nx, ny, nz)));
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_set_param_pln(OreoFeatureTree t,
                                         const char* featureId,
                                         const char* paramName,
                                         double px, double py, double pz,
                                         double nx, double ny, double nz) {
    OREO_C_TRY
    if (!t || !t->valid() || !featureId || !paramName) return OREO_INVALID_INPUT;
    if (t->ptr()->getFeature(featureId) == nullptr) return OREO_INVALID_INPUT;
    if (gp_Vec(nx, ny, nz).Magnitude() < 1e-12) return OREO_INVALID_INPUT;
    t->ptr()->updateParameter(featureId, paramName,
        gp_Pln(gp_Pnt(px, py, pz), gp_Dir(nx, ny, nz)));
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_set_param_ref(OreoFeatureTree t,
                                         const char* featureId,
                                         const char* paramName,
                                         const char* refFeatureId,
                                         const char* refElementName,
                                         const char* refElementType) {
    OREO_C_TRY
    if (!t || !t->valid() || !featureId || !paramName) return OREO_INVALID_INPUT;
    if (t->ptr()->getFeature(featureId) == nullptr) return OREO_INVALID_INPUT;
    oreo::ElementRef r;
    r.featureId   = refFeatureId   ? refFeatureId   : "";
    r.elementName = refElementName ? refElementName : "";
    r.elementType = refElementType ? refElementType : "";
    t->ptr()->updateParameter(featureId, paramName, r);
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_set_param_ref_list(OreoFeatureTree t,
                                              const char* featureId,
                                              const char* paramName,
                                              const char* const* refFeatureIds,
                                              const char* const* refElementNames,
                                              const char* const* refElementTypes,
                                              int n) {
    OREO_C_TRY
    if (!t || !t->valid() || !featureId || !paramName) return OREO_INVALID_INPUT;
    if (n < 0) return OREO_INVALID_INPUT;
    if (t->ptr()->getFeature(featureId) == nullptr) return OREO_INVALID_INPUT;
    std::vector<oreo::ElementRef> refs;
    refs.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        oreo::ElementRef r;
        r.featureId   = (refFeatureIds   && refFeatureIds[i])   ? refFeatureIds[i]   : "";
        r.elementName = (refElementNames && refElementNames[i]) ? refElementNames[i] : "";
        r.elementType = (refElementTypes && refElementTypes[i]) ? refElementTypes[i] : "";
        refs.push_back(std::move(r));
    }
    t->ptr()->updateParameter(featureId, paramName, std::move(refs));
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_add_param_ref_list(OreoFeatureTree t,
                                              const char* featureId,
                                              const char* paramName,
                                              const char* refFeatureId,
                                              const char* refElementName,
                                              const char* refElementType) {
    OREO_C_TRY
    if (!t || !t->valid() || !featureId || !paramName) return OREO_INVALID_INPUT;
    const oreo::Feature* existing = t->ptr()->getFeature(featureId);
    if (!existing) return OREO_INVALID_INPUT;
    oreo::ElementRef r;
    r.featureId   = refFeatureId   ? refFeatureId   : "";
    r.elementName = refElementName ? refElementName : "";
    r.elementType = refElementType ? refElementType : "";
    // Start from the existing list (if any), append, replay through
    // updateParameter so dirty tracking / downstream replay kick in.
    std::vector<oreo::ElementRef> refs;
    auto it = existing->params.find(paramName);
    if (it != existing->params.end()) {
        if (auto* lst = std::get_if<std::vector<oreo::ElementRef>>(&it->second)) {
            refs = *lst;
        } else if (std::holds_alternative<oreo::ElementRef>(it->second)) {
            // Caller mixed set_ref (scalar) and add_param_ref_list on
            // the same name — same error-shape as the builder API.
            return OREO_INVALID_STATE;
        }
        // Any other existing type is treated as "replaced by a new list".
    }
    refs.push_back(std::move(r));
    t->ptr()->updateParameter(featureId, paramName, std::move(refs));
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_set_param_config_ref(OreoFeatureTree t,
                                                const char* featureId,
                                                const char* paramName,
                                                const char* configInputName) {
    OREO_C_TRY
    if (!t || !t->valid() || !featureId || !paramName) return OREO_INVALID_INPUT;
    if (!configInputName || !*configInputName) return OREO_INVALID_INPUT;
    if (t->ptr()->getFeature(featureId) == nullptr) return OREO_INVALID_INPUT;
    oreo::ConfigRef cr;
    cr.name = configInputName;
    t->ptr()->updateParameter(featureId, paramName, cr);
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_unset_param(OreoFeatureTree t,
                                       const char* featureId,
                                       const char* paramName) {
    OREO_C_TRY
    if (!t || !t->valid() || !featureId || !paramName) return OREO_INVALID_INPUT;
    if (t->ptr()->getFeature(featureId) == nullptr) return OREO_INVALID_INPUT;
    // FeatureTree has no public "erase param" helper; we reach through
    // the non-const accessor by finding the feature's index and then
    // erasing from params directly. Dirty-marking still needs to run
    // so downstream features replay — we mirror updateParameter's
    // contract by calling it with an empty update (delete → mark dirty).
    // The simplest path is to re-use the tree's internal map surface:
    // remove the key and re-set rollback so markDirtyFrom fires.
    //
    // We do not have a direct erase helper, so use the public const
    // vector + index + the non-const tree to mutate safely.
    auto& tree = *t->ptr();
    const auto& feats = tree.features();
    int idx = -1;
    for (int i = 0; i < static_cast<int>(feats.size()); ++i) {
        if (feats[i].id == featureId) { idx = i; break; }
    }
    if (idx < 0) return OREO_INVALID_INPUT;
    // The public tree API is read-only for per-feature param maps
    // except through updateParameter. The closest semantically correct
    // move is to re-add with the param missing: copy the feature, erase
    // the key, remove + re-insert at the same index.
    oreo::Feature copy = feats[idx];
    auto pit = copy.params.find(paramName);
    if (pit == copy.params.end()) return OREO_OK;  // no-op; param wasn't set
    copy.params.erase(pit);
    (void)tree.removeFeature(featureId);
    if (!tree.insertFeature(idx, copy)) {
        treeDiag(t).error(oreo::ErrorCode::INTERNAL_ERROR,
            "oreo_ctx_feature_tree_unset_param: re-insertion failed for '"
            + std::string(featureId) + "'");
        return OREO_INTERNAL_ERROR;
    }
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
    if (!t || !t->valid()) return out;
    for (const auto& f : t->ptr()->features()) {
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
    if (!t || !t->valid() || !featureId) return OREO_INVALID_INPUT;
    return t->ptr()->moveFeature(featureId, newIndex) ? OREO_OK : OREO_INVALID_INPUT;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_replace_reference(OreoFeatureTree t,
                                             const char* oldFeatureId,
                                             const char* newFeatureId) {
    OREO_C_TRY
    if (!t || !t->valid() || !oldFeatureId || !newFeatureId) return OREO_INVALID_INPUT;
    return t->ptr()->replaceReference(oldFeatureId, newFeatureId);
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_count(OreoFeatureTree t) {
    OREO_C_TRY
    if (!t || !t->valid()) return 0;
    return t->ptr()->featureCount();
    OREO_C_CATCH_RETURN(0)
}

// ── Introspection: structured C-ABI read API ────────────────────
//
// Lets the app walk the tree without round-tripping through toJSON/
// parse. Each accessor takes a 0-based index in replay order; the
// typed getters return OREO_INVALID_INPUT for missing params,
// OREO_INVALID_STATE for a present-but-wrong-type param, and
// OREO_OUT_OF_RANGE for a bad feature index.

extern "C++" {
namespace {

const oreo::Feature* featureAtIndex(OreoFeatureTree t, int index) {
    if (!t || !t->valid()) return nullptr;
    const auto& feats = t->ptr()->features();
    if (index < 0 || index >= static_cast<int>(feats.size())) return nullptr;
    return &feats[static_cast<std::size_t>(index)];
}

// Read a Param by (featureIndex, paramName). Writes *outFeature and
// *outValue on success. Returns OREO_OK / OREO_OUT_OF_RANGE /
// OREO_INVALID_INPUT consistent with the rest of the introspection API.
int lookupParam(OreoFeatureTree t, int featureIndex, const char* paramName,
                const oreo::Feature*& outFeature,
                const oreo::ParamValue*& outValue) {
    if (!paramName) return OREO_INVALID_INPUT;
    const oreo::Feature* f = featureAtIndex(t, featureIndex);
    if (!f) return OREO_OUT_OF_RANGE;
    auto it = f->params.find(paramName);
    if (it == f->params.end()) return OREO_INVALID_INPUT;
    outFeature = f;
    outValue   = &it->second;
    return OREO_OK;
}

// Pick a single string out of an ElementRef by field selector.
// field 0 = featureId, 1 = elementName, 2 = elementType.
int refFieldOut(const oreo::ElementRef& r, int field,
                char* buf, size_t buflen, size_t* needed) {
    const std::string* s = nullptr;
    switch (field) {
        case 0: s = &r.featureId; break;
        case 1: s = &r.elementName; break;
        case 2: s = &r.elementType; break;
        default: return OREO_INVALID_INPUT;
    }
    return writeSizeProbe(*s, buf, buflen, needed);
}

} // anonymous
} // extern "C++"

int oreo_ctx_feature_tree_index_of(OreoFeatureTree t, const char* featureId) {
    OREO_C_TRY
    if (!t || !t->valid() || !featureId) return -1;
    const auto& feats = t->ptr()->features();
    for (int i = 0; i < static_cast<int>(feats.size()); ++i) {
        if (feats[i].id == featureId) return i;
    }
    return -1;
    OREO_C_CATCH_RETURN(-1)
}

int oreo_ctx_feature_tree_feature_id(OreoFeatureTree t, int index,
                                      char* buf, size_t buflen, size_t* needed) {
    OREO_C_TRY
    const oreo::Feature* f = featureAtIndex(t, index);
    if (!f) return OREO_OUT_OF_RANGE;
    return writeSizeProbe(f->id, buf, buflen, needed);
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_feature_type(OreoFeatureTree t, int index,
                                        char* buf, size_t buflen, size_t* needed) {
    OREO_C_TRY
    const oreo::Feature* f = featureAtIndex(t, index);
    if (!f) return OREO_OUT_OF_RANGE;
    return writeSizeProbe(f->type, buf, buflen, needed);
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_feature_error(OreoFeatureTree t, int index,
                                         char* buf, size_t buflen, size_t* needed) {
    OREO_C_TRY
    const oreo::Feature* f = featureAtIndex(t, index);
    if (!f) return OREO_OUT_OF_RANGE;
    return writeSizeProbe(f->errorMessage, buf, buflen, needed);
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_feature_status(OreoFeatureTree t, int index) {
    OREO_C_TRY
    const oreo::Feature* f = featureAtIndex(t, index);
    if (!f) return -1;
    return static_cast<int>(f->status);
    OREO_C_CATCH_RETURN(-1)
}

int oreo_ctx_feature_tree_feature_suppressed(OreoFeatureTree t, int index) {
    OREO_C_TRY
    const oreo::Feature* f = featureAtIndex(t, index);
    if (!f) return -1;
    return f->suppressed ? 1 : 0;
    OREO_C_CATCH_RETURN(-1)
}

int oreo_ctx_feature_tree_param_count(OreoFeatureTree t, int index) {
    OREO_C_TRY
    const oreo::Feature* f = featureAtIndex(t, index);
    if (!f) return -1;
    return static_cast<int>(f->params.size());
    OREO_C_CATCH_RETURN(-1)
}

int oreo_ctx_feature_tree_param_name(OreoFeatureTree t,
                                      int featureIndex, int paramIndex,
                                      char* buf, size_t buflen, size_t* needed) {
    OREO_C_TRY
    const oreo::Feature* f = featureAtIndex(t, featureIndex);
    if (!f) return OREO_OUT_OF_RANGE;
    if (paramIndex < 0 || paramIndex >= static_cast<int>(f->params.size())) {
        return OREO_OUT_OF_RANGE;
    }
    auto it = f->params.begin();
    std::advance(it, paramIndex);
    return writeSizeProbe(it->first, buf, buflen, needed);
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_param_type(OreoFeatureTree t,
                                      int featureIndex, const char* paramName) {
    OREO_C_TRY
    const oreo::Feature* f = featureAtIndex(t, featureIndex);
    if (!f || !paramName) return -1;
    auto it = f->params.find(paramName);
    if (it == f->params.end()) return -1;
    return static_cast<int>(it->second.index());
    OREO_C_CATCH_RETURN(-1)
}

int oreo_ctx_feature_tree_get_param_double(OreoFeatureTree t,
                                            int featureIndex, const char* paramName,
                                            double* out) {
    OREO_C_TRY
    const oreo::Feature* f = nullptr; const oreo::ParamValue* v = nullptr;
    int rc = lookupParam(t, featureIndex, paramName, f, v); if (rc != OREO_OK) return rc;
    auto* x = std::get_if<double>(v);
    if (!x) return OREO_INVALID_STATE;
    if (out) *out = *x;
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_get_param_int(OreoFeatureTree t,
                                         int featureIndex, const char* paramName,
                                         int* out) {
    OREO_C_TRY
    const oreo::Feature* f = nullptr; const oreo::ParamValue* v = nullptr;
    int rc = lookupParam(t, featureIndex, paramName, f, v); if (rc != OREO_OK) return rc;
    auto* x = std::get_if<int>(v);
    if (!x) return OREO_INVALID_STATE;
    if (out) *out = *x;
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_get_param_bool(OreoFeatureTree t,
                                          int featureIndex, const char* paramName,
                                          int* out) {
    OREO_C_TRY
    const oreo::Feature* f = nullptr; const oreo::ParamValue* v = nullptr;
    int rc = lookupParam(t, featureIndex, paramName, f, v); if (rc != OREO_OK) return rc;
    auto* x = std::get_if<bool>(v);
    if (!x) return OREO_INVALID_STATE;
    if (out) *out = *x ? 1 : 0;
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_get_param_string(OreoFeatureTree t,
                                            int featureIndex, const char* paramName,
                                            char* buf, size_t buflen, size_t* needed) {
    OREO_C_TRY
    const oreo::Feature* f = nullptr; const oreo::ParamValue* v = nullptr;
    int rc = lookupParam(t, featureIndex, paramName, f, v); if (rc != OREO_OK) return rc;
    auto* x = std::get_if<std::string>(v);
    if (!x) return OREO_INVALID_STATE;
    return writeSizeProbe(*x, buf, buflen, needed);
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_get_param_vec(OreoFeatureTree t,
                                         int featureIndex, const char* paramName,
                                         double* x, double* y, double* z) {
    OREO_C_TRY
    const oreo::Feature* f = nullptr; const oreo::ParamValue* v = nullptr;
    int rc = lookupParam(t, featureIndex, paramName, f, v); if (rc != OREO_OK) return rc;
    auto* vec = std::get_if<gp_Vec>(v);
    if (!vec) return OREO_INVALID_STATE;
    if (x) *x = vec->X(); if (y) *y = vec->Y(); if (z) *z = vec->Z();
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_get_param_pnt(OreoFeatureTree t,
                                         int featureIndex, const char* paramName,
                                         double* x, double* y, double* z) {
    OREO_C_TRY
    const oreo::Feature* f = nullptr; const oreo::ParamValue* v = nullptr;
    int rc = lookupParam(t, featureIndex, paramName, f, v); if (rc != OREO_OK) return rc;
    auto* p = std::get_if<gp_Pnt>(v);
    if (!p) return OREO_INVALID_STATE;
    if (x) *x = p->X(); if (y) *y = p->Y(); if (z) *z = p->Z();
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_get_param_dir(OreoFeatureTree t,
                                         int featureIndex, const char* paramName,
                                         double* x, double* y, double* z) {
    OREO_C_TRY
    const oreo::Feature* f = nullptr; const oreo::ParamValue* v = nullptr;
    int rc = lookupParam(t, featureIndex, paramName, f, v); if (rc != OREO_OK) return rc;
    auto* d = std::get_if<gp_Dir>(v);
    if (!d) return OREO_INVALID_STATE;
    if (x) *x = d->X(); if (y) *y = d->Y(); if (z) *z = d->Z();
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_get_param_ax1(OreoFeatureTree t,
                                         int featureIndex, const char* paramName,
                                         double* px, double* py, double* pz,
                                         double* dx, double* dy, double* dz) {
    OREO_C_TRY
    const oreo::Feature* f = nullptr; const oreo::ParamValue* v = nullptr;
    int rc = lookupParam(t, featureIndex, paramName, f, v); if (rc != OREO_OK) return rc;
    auto* a = std::get_if<gp_Ax1>(v);
    if (!a) return OREO_INVALID_STATE;
    auto loc = a->Location(); auto dir = a->Direction();
    if (px) *px = loc.X(); if (py) *py = loc.Y(); if (pz) *pz = loc.Z();
    if (dx) *dx = dir.X(); if (dy) *dy = dir.Y(); if (dz) *dz = dir.Z();
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_get_param_ax2(OreoFeatureTree t,
                                         int featureIndex, const char* paramName,
                                         double* px, double* py, double* pz,
                                         double* nx, double* ny, double* nz) {
    OREO_C_TRY
    const oreo::Feature* f = nullptr; const oreo::ParamValue* v = nullptr;
    int rc = lookupParam(t, featureIndex, paramName, f, v); if (rc != OREO_OK) return rc;
    auto* a = std::get_if<gp_Ax2>(v);
    if (!a) return OREO_INVALID_STATE;
    auto loc = a->Location(); auto dir = a->Direction();
    if (px) *px = loc.X(); if (py) *py = loc.Y(); if (pz) *pz = loc.Z();
    if (nx) *nx = dir.X(); if (ny) *ny = dir.Y(); if (nz) *nz = dir.Z();
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_get_param_pln(OreoFeatureTree t,
                                         int featureIndex, const char* paramName,
                                         double* px, double* py, double* pz,
                                         double* nx, double* ny, double* nz) {
    OREO_C_TRY
    const oreo::Feature* f = nullptr; const oreo::ParamValue* v = nullptr;
    int rc = lookupParam(t, featureIndex, paramName, f, v); if (rc != OREO_OK) return rc;
    auto* p = std::get_if<gp_Pln>(v);
    if (!p) return OREO_INVALID_STATE;
    auto loc = p->Location(); auto axis = p->Axis().Direction();
    if (px) *px = loc.X(); if (py) *py = loc.Y(); if (pz) *pz = loc.Z();
    if (nx) *nx = axis.X(); if (ny) *ny = axis.Y(); if (nz) *nz = axis.Z();
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_get_param_ref(OreoFeatureTree t,
                                         int featureIndex, const char* paramName,
                                         int field, char* buf, size_t buflen,
                                         size_t* needed) {
    OREO_C_TRY
    const oreo::Feature* f = nullptr; const oreo::ParamValue* v = nullptr;
    int rc = lookupParam(t, featureIndex, paramName, f, v); if (rc != OREO_OK) return rc;
    auto* r = std::get_if<oreo::ElementRef>(v);
    if (!r) return OREO_INVALID_STATE;
    return refFieldOut(*r, field, buf, buflen, needed);
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_get_param_ref_list_size(OreoFeatureTree t,
                                                   int featureIndex,
                                                   const char* paramName) {
    OREO_C_TRY
    const oreo::Feature* f = nullptr; const oreo::ParamValue* v = nullptr;
    int rc = lookupParam(t, featureIndex, paramName, f, v); if (rc != OREO_OK) return -1;
    auto* lst = std::get_if<std::vector<oreo::ElementRef>>(v);
    if (!lst) return -1;
    return static_cast<int>(lst->size());
    OREO_C_CATCH_RETURN(-1)
}

int oreo_ctx_feature_tree_get_param_ref_list_entry(OreoFeatureTree t,
                                                    int featureIndex,
                                                    const char* paramName,
                                                    int entryIndex, int field,
                                                    char* buf, size_t buflen,
                                                    size_t* needed) {
    OREO_C_TRY
    const oreo::Feature* f = nullptr; const oreo::ParamValue* v = nullptr;
    int rc = lookupParam(t, featureIndex, paramName, f, v); if (rc != OREO_OK) return rc;
    auto* lst = std::get_if<std::vector<oreo::ElementRef>>(v);
    if (!lst) return OREO_INVALID_STATE;
    if (entryIndex < 0 || entryIndex >= static_cast<int>(lst->size())) return OREO_OUT_OF_RANGE;
    return refFieldOut((*lst)[static_cast<std::size_t>(entryIndex)], field, buf, buflen, needed);
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_get_param_config_ref(OreoFeatureTree t,
                                                int featureIndex,
                                                const char* paramName,
                                                char* buf, size_t buflen,
                                                size_t* needed) {
    OREO_C_TRY
    const oreo::Feature* f = nullptr; const oreo::ParamValue* v = nullptr;
    int rc = lookupParam(t, featureIndex, paramName, f, v); if (rc != OREO_OK) return rc;
    auto* cr = std::get_if<oreo::ConfigRef>(v);
    if (!cr) return OREO_INVALID_STATE;
    return writeSizeProbe(cr->name, buf, buflen, needed);
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_feature_tree_validate(OreoFeatureTree t) {
    OREO_C_TRY
    if (!t || !t->valid()) return OREO_INVALID_INPUT;
    int firstError = OREO_OK;
    // FeatureTree::features() returns const&; validateFeature mutates
    // status/errorMessage on the Feature. We snapshot a mutable copy
    // per feature so the tree's stored state is left untouched — the
    // tree's own status updates happen during replay.
    for (auto& f : t->ptr()->features()) {
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
    if (!t || !t->valid()) return nullptr;
    auto result = t->ptr()->replay();
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

// ============================================================
// PartStudio / ConfigSchema / ConfigValue C ABI
// ============================================================

struct OreoPartStudio_T {
    std::shared_ptr<oreo::KernelContext> ctxBinding;
    std::unique_ptr<oreo::PartStudio>    studio;
    // Long-lived handle for oreo_ctx_part_studio_tree, lazily created
    // on first call. Non-owning from the consumer's perspective: freed
    // together with the studio.
    std::unique_ptr<OreoFeatureTree_T>   treeHandle;
};

// A ConfigValue handle carries a weak reference to the parent studio
// so per-input type/bound validation has a schema to consult. If the
// parent studio is freed before the config, setters on the config
// become no-ops with an error code — use-after-free safety.
struct OreoConfig_T {
    std::weak_ptr<oreo::KernelContext> ctxBinding;
    // Raw pointer to the studio is acceptable here because ConfigValue
    // and the studio have a strict "config created from studio X MUST
    // outlive no longer than X" contract (documented on
    // oreo_config_create). For an extra guard, we also stash the
    // ConfigSchema snapshot at creation time so free-after-use on the
    // studio doesn't take the config's type info with it.
    oreo::ConfigSchema schemaSnapshot;
    oreo::ConfigValue  value;
    // Fingerprint of the studio's ConfigSchema at the moment this config
    // was created. oreo_ctx_part_studio_execute compares this against
    // the target studio's current fingerprint before dispatch, so a
    // config created from studio A cannot silently run against studio B
    // (or against the original studio after its schema was mutated).
    // Audit 2026-04-19 finding P1/P2.
    std::uint64_t      schemaFingerprint = 0;
    // Opaque studio pointer captured at create time. Used only as an
    // identity stamp so diagnostics can point at WHICH studio the
    // config was born from; never dereferenced — the studio may have
    // been freed already by the time the mismatch is caught.
    const void*        originStudioId = nullptr;
};

// extern "C++" escapes the surrounding extern "C" block so studioCtx
// can return a C++ reference (-Wreturn-type-c-linkage otherwise).
extern "C++" {
namespace {

bool studioOk(OreoPartStudio ps) {
    return ps != nullptr && ps->studio != nullptr;
}

oreo::KernelContext& studioCtx(OreoPartStudio ps) {
    return (ps && ps->ctxBinding) ? *ps->ctxBinding : *internalDefaultCtx();
}

} // anonymous namespace
} // extern "C++"

OreoPartStudio oreo_ctx_part_studio_create(OreoContext ctx, const char* name) {
    OREO_C_TRY
    if (!ctx || !ctx->ctx) return nullptr;
    auto* ps = new OreoPartStudio_T{};
    ps->ctxBinding = ctx->ctx;
    ps->studio = std::make_unique<oreo::PartStudio>(ctx->ctx,
                                                     name ? std::string(name) : std::string{});
    return ps;
    OREO_C_CATCH_RETURN(nullptr)
}

void oreo_ctx_part_studio_free(OreoPartStudio ps) {
    OREO_C_TRY
    delete ps;
    OREO_C_CATCH_VOID
}

OreoFeatureTree oreo_ctx_part_studio_tree(OreoPartStudio ps) {
    OREO_C_TRY
    if (!studioOk(ps)) return nullptr;
    if (!ps->treeHandle) {
        // Borrow — the studio owns the tree. OreoFeatureTree_T has an
        // owned/borrowed split so the handle's destructor won't delete
        // a tree it didn't allocate.
        ps->treeHandle = std::make_unique<OreoFeatureTree_T>();
        ps->treeHandle->ctxBinding = ps->ctxBinding;
        ps->treeHandle->borrow     = &ps->studio->tree();
    }
    return ps->treeHandle.get();
    OREO_C_CATCH_RETURN(nullptr)
}

namespace {

// Helper: add-input plumbing. name-validated, variant-packed default,
// forwards to ConfigSchema::addInput.
int addInputImpl(OreoPartStudio ps, const char* name,
                 oreo::ParamType type, oreo::ParamValue dflt,
                 std::optional<double> minInc = std::nullopt,
                 std::optional<double> maxInc = std::nullopt) {
    if (!studioOk(ps))   return OREO_INVALID_INPUT;
    if (!name || !*name) return OREO_INVALID_INPUT;
    oreo::ConfigInputSpec spec;
    spec.name         = name;
    spec.type         = type;
    spec.defaultValue = std::move(dflt);
    spec.minInclusive = minInc;
    spec.maxInclusive = maxInc;
    int rc = ps->studio->configSchema().addInput(studioCtx(ps), std::move(spec));
    return rc;
}

} // anonymous namespace

int oreo_ctx_part_studio_add_input_double(OreoPartStudio ps,
                                           const char* name, double dflt) {
    OREO_C_TRY
    return addInputImpl(ps, name, oreo::ParamType::Double, oreo::ParamValue(dflt));
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_part_studio_add_input_double_bounded(OreoPartStudio ps,
                                                   const char* name, double dflt,
                                                   double minInc, double maxInc) {
    OREO_C_TRY
    return addInputImpl(ps, name, oreo::ParamType::Double,
                        oreo::ParamValue(dflt), minInc, maxInc);
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_part_studio_add_input_int(OreoPartStudio ps,
                                        const char* name, int dflt) {
    OREO_C_TRY
    return addInputImpl(ps, name, oreo::ParamType::Int, oreo::ParamValue(dflt));
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_part_studio_add_input_bool(OreoPartStudio ps,
                                         const char* name, int dflt) {
    OREO_C_TRY
    return addInputImpl(ps, name, oreo::ParamType::Bool,
                        oreo::ParamValue(dflt != 0));
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_part_studio_add_input_string(OreoPartStudio ps,
                                           const char* name,
                                           const char* dflt) {
    OREO_C_TRY
    std::string s = dflt ? dflt : "";
    return addInputImpl(ps, name, oreo::ParamType::String,
                        oreo::ParamValue(std::move(s)));
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_part_studio_add_input_vec(OreoPartStudio ps,
                                        const char* name,
                                        double dx, double dy, double dz) {
    OREO_C_TRY
    return addInputImpl(ps, name, oreo::ParamType::Vec,
                        oreo::ParamValue(gp_Vec(dx, dy, dz)));
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_part_studio_input_count(OreoPartStudio ps) {
    OREO_C_TRY
    if (!studioOk(ps)) return 0;
    return static_cast<int>(ps->studio->configSchema().inputCount());
    OREO_C_CATCH_RETURN(0)
}

uint64_t oreo_ctx_part_studio_schema_fingerprint(OreoPartStudio ps) {
    OREO_C_TRY
    if (!studioOk(ps)) return 0;
    return ps->studio->configSchema().fingerprint();
    OREO_C_CATCH_RETURN(0)
}

uint64_t oreo_config_schema_fingerprint(OreoConfig cfg) {
    OREO_C_TRY
    if (!cfg) return 0;
    return cfg->schemaFingerprint;
    OREO_C_CATCH_RETURN(0)
}

int oreo_feature_builder_set_config_ref(OreoFeatureBuilder b,
                                         const char* paramName,
                                         const char* configInputName) {
    OREO_C_TRY
    OreoErrorCode err = OREO_OK;
    if (!builderValid(b, paramName, err)) return err;
    if (!configInputName || !*configInputName) return OREO_INVALID_INPUT;
    oreo::ConfigRef cr;
    cr.name = configInputName;
    b->feature.params[paramName] = cr;
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

// ─── ConfigValue handle ────────────────────────────────────────

OreoConfig oreo_config_create(OreoPartStudio ps) {
    OREO_C_TRY
    if (!studioOk(ps)) return nullptr;
    auto* cfg = new OreoConfig_T{};
    cfg->ctxBinding        = ps->ctxBinding;
    cfg->schemaSnapshot    = ps->studio->configSchema();  // deep copy of spec list
    cfg->schemaFingerprint = cfg->schemaSnapshot.fingerprint();
    cfg->originStudioId    = static_cast<const void*>(ps);
    cfg->value             = oreo::ConfigValue::fromSchemaDefaults(cfg->schemaSnapshot);
    return cfg;
    OREO_C_CATCH_RETURN(nullptr)
}

void oreo_config_free(OreoConfig cfg) {
    OREO_C_TRY
    delete cfg;
    OREO_C_CATCH_VOID
}

// extern "C++" escapes the surrounding extern "C" block so configCtx
// / configSetImpl can take C++ references and oreo::ParamValue
// arguments (-Wreturn-type-c-linkage otherwise).
extern "C++" {
namespace {

// Helper: find a live ctx. Returns internalDefaultCtx if the studio
// was freed (which would have been a contract violation — we route
// the diagnostic through the default ctx so the error isn't silenced).
oreo::KernelContext& configCtx(OreoConfig cfg) {
    if (auto sp = cfg->ctxBinding.lock()) return *sp;
    return *internalDefaultCtx();
}

int configSetImpl(OreoConfig cfg, const char* name, oreo::ParamValue v) {
    if (!cfg)            return OREO_INVALID_INPUT;
    if (!name || !*name) return OREO_INVALID_INPUT;
    auto sp = cfg->ctxBinding.lock();
    if (!sp) {
        internalDefaultCtx()->diag().error(oreo::ErrorCode::INVALID_INPUT,
            "OreoConfig: parent studio was freed before this set call");
        return OREO_INVALID_STATE;
    }
    return cfg->value.set(*sp, cfg->schemaSnapshot, name, std::move(v));
}

} // anonymous namespace
} // extern "C++"

int oreo_config_set_double(OreoConfig cfg, const char* name, double v) {
    OREO_C_TRY
    return configSetImpl(cfg, name, oreo::ParamValue(v));
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}
int oreo_config_set_int(OreoConfig cfg, const char* name, int v) {
    OREO_C_TRY
    return configSetImpl(cfg, name, oreo::ParamValue(v));
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}
int oreo_config_set_bool(OreoConfig cfg, const char* name, int v) {
    OREO_C_TRY
    return configSetImpl(cfg, name, oreo::ParamValue(v != 0));
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}
int oreo_config_set_string(OreoConfig cfg, const char* name, const char* v) {
    OREO_C_TRY
    return configSetImpl(cfg, name, oreo::ParamValue(std::string(v ? v : "")));
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}
int oreo_config_set_vec(OreoConfig cfg, const char* name,
                         double x, double y, double z) {
    OREO_C_TRY
    return configSetImpl(cfg, name, oreo::ParamValue(gp_Vec(x, y, z)));
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

// ─── Execute ───────────────────────────────────────────────────

OreoSolid oreo_ctx_part_studio_execute(OreoPartStudio ps, OreoConfig cfg) {
    OREO_C_TRY
    if (!studioOk(ps)) return nullptr;
    oreo::ConfigValue effective;
    if (cfg) {
        // Fingerprint guard — ensure this config was created for this
        // studio's current ConfigSchema. Without the guard, two studios
        // with overlapping-but-not-equal schemas could silently accept
        // each other's configs: values present in both would carry over
        // with no warning, values present in only one would be lost to
        // the lenient "unknown input" warning. Audit 2026-04-19 P1/P2.
        //
        // We compare the fingerprint snapshotted at oreo_config_create
        // time against the studio's CURRENT fingerprint; this also
        // catches the case where the studio's schema was mutated after
        // the config was created.
        const std::uint64_t studioFp = ps->studio->configSchema().fingerprint();
        if (cfg->schemaFingerprint != studioFp) {
            studioCtx(ps).diag().error(oreo::ErrorCode::INVALID_INPUT,
                std::string("OreoConfig was created for a different studio "
                            "schema (config fingerprint=")
                + std::to_string(cfg->schemaFingerprint)
                + ", target studio fingerprint=" + std::to_string(studioFp)
                + "). Re-create the config via oreo_config_create against "
                  "the target studio, or stop mutating the studio's schema "
                  "after configs have been created against it.");
            return nullptr;
        }
        effective = cfg->value;
    } else {
        effective = oreo::ConfigValue::fromSchemaDefaults(ps->studio->configSchema());
    }
    auto out = ps->studio->execute(effective);
    if (out.finalShape.isNull()) return nullptr;
    auto* handle = new OreoSolid_T{out.finalShape};
    return handle;
    OREO_C_CATCH_RETURN(nullptr)
}

OreoSolid oreo_ctx_part_studio_execute_defaults(OreoPartStudio ps) {
    return oreo_ctx_part_studio_execute(ps, nullptr);
}

// ─── JSON round-trip ───────────────────────────────────────────

char* oreo_ctx_part_studio_to_json(OreoPartStudio ps) {
    OREO_C_TRY
    if (!studioOk(ps)) return nullptr;
    std::string s = ps->studio->toJSON();
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, s.data(), s.size());
    out[s.size()] = '\0';
    return out;
    OREO_C_CATCH_RETURN(nullptr)
}

OreoPartStudio oreo_ctx_part_studio_from_json(OreoContext ctx, const char* json) {
    OREO_C_TRY
    if (!ctx || !ctx->ctx) return nullptr;
    if (!json) return nullptr;
    auto fr = oreo::PartStudio::fromJSON(ctx->ctx, std::string(json));
    if (!fr.ok || !fr.studio) {
        ctx->ctx->diag().error(oreo::ErrorCode::DESERIALIZE_FAILED,
                                "PartStudio::fromJSON: " + fr.error);
        return nullptr;
    }
    auto* ps = new OreoPartStudio_T{};
    ps->ctxBinding = ctx->ctx;
    ps->studio     = std::move(fr.studio);
    return ps;
    OREO_C_CATCH_RETURN(nullptr)
}

void oreo_free_string(char* s) {
    OREO_C_TRY
    std::free(s);
    OREO_C_CATCH_VOID
}

// ============================================================
// Workspace + three-way merge C ABI
// ============================================================

struct OreoWorkspace_T {
    std::shared_ptr<oreo::KernelContext> ctxBinding;
    std::unique_ptr<oreo::Workspace>     workspace;
    std::unique_ptr<OreoFeatureTree_T>   treeHandle;
};

struct OreoMergeResult_T {
    std::shared_ptr<oreo::KernelContext> ctxBinding;
    std::unique_ptr<oreo::MergeResult>   result;
    std::unique_ptr<OreoFeatureTree_T>   treeHandle;
};

namespace {
bool workspaceOk(OreoWorkspace ws) {
    return ws != nullptr && ws->workspace != nullptr;
}
bool mergeResultOk(OreoMergeResult mr) {
    return mr != nullptr && mr->result != nullptr;
}
} // anonymous namespace

OreoWorkspace oreo_ctx_workspace_create(OreoContext ctx, const char* name) {
    OREO_C_TRY
    if (!ctx || !ctx->ctx) return nullptr;
    auto* ws = new OreoWorkspace_T{};
    ws->ctxBinding = ctx->ctx;
    ws->workspace = std::make_unique<oreo::Workspace>(
        ctx->ctx, name ? std::string(name) : std::string{});
    return ws;
    OREO_C_CATCH_RETURN(nullptr)
}

void oreo_ctx_workspace_free(OreoWorkspace ws) {
    OREO_C_TRY
    delete ws;
    OREO_C_CATCH_VOID
}

OreoWorkspace oreo_ctx_workspace_fork(OreoWorkspace ws, const char* newName) {
    OREO_C_TRY
    if (!workspaceOk(ws)) return nullptr;
    auto forkedUp = ws->workspace->fork(newName ? std::string(newName) : std::string{});
    if (!forkedUp) return nullptr;
    auto* child = new OreoWorkspace_T{};
    child->ctxBinding = ws->ctxBinding;
    child->workspace  = std::move(forkedUp);
    return child;
    OREO_C_CATCH_RETURN(nullptr)
}

OreoFeatureTree oreo_ctx_workspace_tree(OreoWorkspace ws) {
    OREO_C_TRY
    if (!workspaceOk(ws)) return nullptr;
    if (!ws->treeHandle) {
        ws->treeHandle = std::make_unique<OreoFeatureTree_T>();
        ws->treeHandle->ctxBinding = ws->ctxBinding;
        ws->treeHandle->borrow = &ws->workspace->tree();
    }
    return ws->treeHandle.get();
    OREO_C_CATCH_RETURN(nullptr)
}

int oreo_ctx_workspace_name(OreoWorkspace ws,
                             char* buf, size_t buflen, size_t* needed) {
    OREO_C_TRY
    if (!workspaceOk(ws)) return OREO_INVALID_INPUT;
    return writeSizeProbe(ws->workspace->name(), buf, buflen, needed);
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_ctx_workspace_parent_name(OreoWorkspace ws,
                                    char* buf, size_t buflen, size_t* needed) {
    OREO_C_TRY
    if (!workspaceOk(ws)) return OREO_INVALID_INPUT;
    return writeSizeProbe(ws->workspace->parentName(), buf, buflen, needed);
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

char* oreo_ctx_workspace_to_json(OreoWorkspace ws) {
    OREO_C_TRY
    if (!workspaceOk(ws)) return nullptr;
    std::string s = ws->workspace->toJSON();
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, s.data(), s.size());
    out[s.size()] = '\0';
    return out;
    OREO_C_CATCH_RETURN(nullptr)
}

OreoWorkspace oreo_ctx_workspace_from_json(OreoContext ctx, const char* json) {
    OREO_C_TRY
    if (!ctx || !ctx->ctx) return nullptr;
    if (!json) return nullptr;
    auto fr = oreo::Workspace::fromJSON(ctx->ctx, std::string(json));
    if (!fr.ok || !fr.workspace) {
        ctx->ctx->diag().error(oreo::ErrorCode::DESERIALIZE_FAILED,
                                "Workspace::fromJSON: " + fr.error);
        return nullptr;
    }
    auto* ws = new OreoWorkspace_T{};
    ws->ctxBinding = ctx->ctx;
    ws->workspace  = std::move(fr.workspace);
    return ws;
    OREO_C_CATCH_RETURN(nullptr)
}

OreoMergeResult oreo_ctx_workspace_merge(OreoWorkspace base,
                                          OreoWorkspace ours,
                                          OreoWorkspace theirs) {
    OREO_C_TRY
    if (!workspaceOk(base) || !workspaceOk(ours) || !workspaceOk(theirs)) {
        return nullptr;
    }
    // Require a shared ctx — cross-document merge is out of scope.
    if (base->ctxBinding  != ours->ctxBinding ||
        ours->ctxBinding  != theirs->ctxBinding) {
        base->ctxBinding->diag().error(oreo::ErrorCode::INVALID_INPUT,
            "oreo_ctx_workspace_merge: workspaces must share a KernelContext "
            "(cross-document merge is not supported)");
        return nullptr;
    }
    auto result = std::make_unique<oreo::MergeResult>(oreo::threeWayMerge(
        base->ctxBinding,
        base->workspace->tree().features(),
        ours->workspace->tree().features(),
        theirs->workspace->tree().features()));
    auto* mr = new OreoMergeResult_T{};
    mr->ctxBinding = base->ctxBinding;
    mr->result     = std::move(result);
    return mr;
    OREO_C_CATCH_RETURN(nullptr)
}

int oreo_merge_result_is_clean(OreoMergeResult mr) {
    OREO_C_TRY
    if (!mergeResultOk(mr)) return 0;
    return mr->result->clean() ? 1 : 0;
    OREO_C_CATCH_RETURN(0)
}

int oreo_merge_result_conflict_count(OreoMergeResult mr) {
    OREO_C_TRY
    if (!mergeResultOk(mr)) return 0;
    return static_cast<int>(mr->result->conflicts.size());
    OREO_C_CATCH_RETURN(0)
}

int oreo_merge_result_conflict_kind(OreoMergeResult mr, int index) {
    OREO_C_TRY
    if (!mergeResultOk(mr)) return -1;
    if (index < 0 || index >= (int)mr->result->conflicts.size()) return -1;
    return static_cast<int>(mr->result->conflicts[index].kind);
    OREO_C_CATCH_RETURN(-1)
}

namespace {
// Unified size-probe protocol — see queryName and writeSizeProbe above.
//
// Contract:
//   - (buf == NULL && buflen == 0)  → probe mode: write *needed with the
//     number of bytes required (excluding NUL) and return OREO_OK. No
//     diagnostic is raised; this is a legal non-error call.
//   - (buf == NULL && buflen != 0)  → misuse; caller has a buffer but a
//     null pointer. Return OREO_INVALID_INPUT.
//   - (buf != NULL && buflen < src.size() + 1) → too small for the
//     NUL-terminated copy. Write what fits plus NUL, write *needed, and
//     return OREO_BUFFER_TOO_SMALL.
//   - otherwise → full write, NUL-terminate, return OREO_OK.
//
// Prior to 2026-04-18 the probe case returned OREO_BUFFER_TOO_SMALL,
// inconsistent with queryName / writeSizeProbe / oreo_ctx_face_name etc.
// App code had to special-case merge_result_conflict_* accessors vs.
// every other size-probed entry point. Fixed: one protocol now.
int copyStringOut(const std::string& src, char* buf, size_t buflen, size_t* needed) {
    if (needed) *needed = src.size();
    if (buf == nullptr && buflen == 0) return OREO_OK;       // probe
    if (buf == nullptr)                 return OREO_INVALID_INPUT;
    if (buflen < src.size() + 1) {
        // Partial write: fill as much as fits then NUL-terminate.
        if (buflen > 0) {
            std::memcpy(buf, src.data(), buflen - 1);
            buf[buflen - 1] = '\0';
        }
        return OREO_BUFFER_TOO_SMALL;
    }
    std::memcpy(buf, src.data(), src.size());
    buf[src.size()] = '\0';
    return OREO_OK;
}
} // anonymous namespace

int oreo_merge_result_conflict_feature_id(OreoMergeResult mr, int index,
                                           char* buf, size_t buflen,
                                           size_t* needed) {
    OREO_C_TRY
    if (!mergeResultOk(mr)) return OREO_INVALID_INPUT;
    if (index < 0 || index >= (int)mr->result->conflicts.size()) return OREO_OUT_OF_RANGE;
    return copyStringOut(mr->result->conflicts[index].featureId, buf, buflen, needed);
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_merge_result_conflict_param_name(OreoMergeResult mr, int index,
                                           char* buf, size_t buflen,
                                           size_t* needed) {
    OREO_C_TRY
    if (!mergeResultOk(mr)) return OREO_INVALID_INPUT;
    if (index < 0 || index >= (int)mr->result->conflicts.size()) return OREO_OUT_OF_RANGE;
    return copyStringOut(mr->result->conflicts[index].paramName, buf, buflen, needed);
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_merge_result_conflict_message(OreoMergeResult mr, int index,
                                        char* buf, size_t buflen,
                                        size_t* needed) {
    OREO_C_TRY
    if (!mergeResultOk(mr)) return OREO_INVALID_INPUT;
    if (index < 0 || index >= (int)mr->result->conflicts.size()) return OREO_OUT_OF_RANGE;
    return copyStringOut(mr->result->conflicts[index].message, buf, buflen, needed);
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

OreoFeatureTree oreo_merge_result_tree(OreoMergeResult mr) {
    OREO_C_TRY
    if (!mergeResultOk(mr)) return nullptr;
    if (!mr->treeHandle) {
        mr->treeHandle = std::make_unique<OreoFeatureTree_T>();
        mr->treeHandle->ctxBinding = mr->ctxBinding;
        mr->treeHandle->borrow = &mr->result->merged;
    }
    return mr->treeHandle.get();
    OREO_C_CATCH_RETURN(nullptr)
}

void oreo_merge_result_free(OreoMergeResult mr) {
    OREO_C_TRY
    delete mr;
    OREO_C_CATCH_VOID
}

// ─── Resolution set + applyResolutions ─────────────────────────────
//
// Exposes the C++ merge.h `applyResolutions` entry point through the
// ABI. The set is a thin wrapper around std::vector<oreo::Resolution>
// so the ABI can build Resolution records incrementally; the caller
// never sees an oreo::Resolution directly.

struct OreoResolutionSet_T {
    std::vector<oreo::Resolution> resolutions;
};

OreoResolutionSet oreo_resolution_set_create(void) {
    OREO_C_TRY
    return new OreoResolutionSet_T{};
    OREO_C_CATCH_RETURN(nullptr)
}

void oreo_resolution_set_free(OreoResolutionSet rs) {
    OREO_C_TRY
    delete rs;
    OREO_C_CATCH_VOID
}

int oreo_resolution_set_size(OreoResolutionSet rs) {
    OREO_C_TRY
    if (!rs) return 0;
    return static_cast<int>(rs->resolutions.size());
    OREO_C_CATCH_RETURN(0)
}

int oreo_resolution_set_add(OreoResolutionSet rs,
                             const char* featureId,
                             const char* paramName,
                             int choice) {
    OREO_C_TRY
    if (!rs || !featureId) return OREO_INVALID_INPUT;
    if (choice < 0 || choice > 3) return OREO_INVALID_INPUT;
    oreo::Resolution r;
    r.featureId  = featureId;
    r.paramName  = paramName ? paramName : "";
    r.choice     = static_cast<oreo::ResolveChoice>(choice);
    // customValue stays default-constructed; _last_set_* fills it.
    rs->resolutions.push_back(std::move(r));
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

namespace {
// All _last_set_* helpers share this guard: there must be at least one
// resolution, AND its choice must be Custom. Any other choice never
// reads the customValue, so attaching one would indicate caller error.
int lastSetGuard(OreoResolutionSet rs) {
    if (!rs) return OREO_INVALID_INPUT;
    if (rs->resolutions.empty()) return OREO_INVALID_STATE;
    if (rs->resolutions.back().choice != oreo::ResolveChoice::Custom)
        return OREO_INVALID_STATE;
    return OREO_OK;
}
} // anonymous

int oreo_resolution_set_last_set_double(OreoResolutionSet rs, double v) {
    OREO_C_TRY
    int g = lastSetGuard(rs); if (g != OREO_OK) return g;
    if (!std::isfinite(v)) return OREO_INVALID_INPUT;
    rs->resolutions.back().customValue = v;
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_resolution_set_last_set_int(OreoResolutionSet rs, int v) {
    OREO_C_TRY
    int g = lastSetGuard(rs); if (g != OREO_OK) return g;
    rs->resolutions.back().customValue = v;
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_resolution_set_last_set_bool(OreoResolutionSet rs, int v) {
    OREO_C_TRY
    int g = lastSetGuard(rs); if (g != OREO_OK) return g;
    rs->resolutions.back().customValue = (v != 0);
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_resolution_set_last_set_string(OreoResolutionSet rs, const char* v) {
    OREO_C_TRY
    int g = lastSetGuard(rs); if (g != OREO_OK) return g;
    if (!v) return OREO_INVALID_INPUT;
    rs->resolutions.back().customValue = std::string(v);
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

int oreo_resolution_set_last_set_vec(OreoResolutionSet rs,
                                      double x, double y, double z) {
    OREO_C_TRY
    int g = lastSetGuard(rs); if (g != OREO_OK) return g;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        return OREO_INVALID_INPUT;
    rs->resolutions.back().customValue = gp_Vec(x, y, z);
    return OREO_OK;
    OREO_C_CATCH_RETURN(OREO_INTERNAL_ERROR)
}

OreoFeatureTree oreo_merge_result_apply_resolutions(
    OreoMergeResult mr,
    OreoResolutionSet rs) {
    OREO_C_TRY
    if (!mergeResultOk(mr)) return nullptr;
    if (!rs) return nullptr;

    // Pull the baseline from the merge result. Note we pass the
    // current merged features (the mr's tree contents), not a fresh
    // rebuild — the C++ applyResolutions operates on that vector.
    std::vector<oreo::Feature> merged = mr->result->merged.features();
    auto finalized = oreo::applyResolutions(
        *mr->ctxBinding, merged,
        mr->result->conflicts, rs->resolutions);

    // Materialise into a freshly-owned OreoFeatureTree. Identity
    // allocation uses the mr's ctx so generated identities stay in the
    // same document namespace as the rest of the merge lineage.
    auto* handle = new OreoFeatureTree_T{};
    handle->ctxBinding = mr->ctxBinding;
    handle->tree = std::make_unique<oreo::FeatureTree>(mr->ctxBinding);
    for (const auto& f : finalized) {
        if (!handle->tree->addFeature(f)) {
            // applyResolutions preserves the merged vector's uniqueness
            // invariant, so this branch is defensive. If we land here,
            // something is badly wrong — fail closed.
            mr->ctxBinding->diag().error(oreo::ErrorCode::INTERNAL_ERROR,
                "oreo_merge_result_apply_resolutions: addFeature "
                "rejected id '" + f.id + "' — merge post-condition broken");
            delete handle;
            return nullptr;
        }
    }
    return handle;
    OREO_C_CATCH_RETURN(nullptr)
}

} // extern "C"
