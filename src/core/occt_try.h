// SPDX-License-Identifier: LGPL-2.1-or-later

// occt_try.h — Exception-boundary macros for C++ geometry functions.
//
// Every C++ function that calls OCCT APIs and returns OperationResult<T>
// should wrap its OCCT-touching body with these macros. OCCT can throw
// Standard_Failure from essentially any call (BRepBuilderAPI_*, BRepPrim*,
// BRepFilletAPI_*, etc.) and if such an exception escapes the function,
// it leaves orphaned diagnostics in ctx.diag that leak into the next
// operation on the same context.
//
// This is analogous to OREO_C_TRY / OREO_C_CATCH_RETURN at the C boundary,
// but for the C++ geometry layer — the exception is converted into an
// OperationResult<T> failure before it can escape.
//
// ── Macro usage (legacy) ─────────────────────────────────────
//   GeomResult extrude(KernelContext& ctx, const NamedShape& base, ...) {
//       DiagnosticScope scope(ctx);
//       // ... validation ...
//       OREO_OCCT_TRY               // defaults to identifiers `ctx` and `scope`
//           // ... OCCT calls, shape building, mapping ...
//           return scope.makeResult(mapped);
//       OREO_OCCT_CATCH_NS(scope, ctx, "extrude")
//   }
//
// ── Macro usage (explicit, OT-4/OT-5) ────────────────────────
//   GeomResult extrude(KernelContext& myCtx, ...) {
//       DiagnosticScope myScope(myCtx);
//       OREO_OCCT_TRY_CTX(myCtx, myScope)
//           return myScope.makeResult(mapped);
//       OREO_OCCT_CATCH_T_CTX(myScope, myCtx, NamedShape, "extrude")
//   }
//
// ── Preferred modern API: occtSafeCall (OT-5) ────────────────
//   GeomResult extrude(KernelContext& ctx, ...) {
//       DiagnosticScope scope(ctx);
//       return occtSafeCall(ctx, scope, "extrude", [&]() -> NamedShape {
//           // ... OCCT calls ...
//           return mapped;
//       });
//   }
//
// The lambda form is header-only, inline, catches the full exception
// hierarchy (Standard_Failure ref/ptr, std::bad_alloc, std::system_error,
// std::bad_cast, std::bad_typeid, std::exception, ...), and emits the
// same diagnostics as the macros. Prefer it for new code.

#ifndef OREO_OCCT_TRY_H
#define OREO_OCCT_TRY_H

#include "diagnostic_scope.h"
#include "kernel_context.h"
#include "operation_result.h"
#include "oreo_error.h"

#include <Standard_Failure.hxx>

#include <exception>
#include <new>
#include <string>
#include <system_error>
#include <type_traits>
#include <typeinfo>
#include <utility>

// ── Catch-clause generators ──────────────────────────────────
//
// The catch order is LOAD-BEARING. We must match the most specific
// exception types first; `std::bad_alloc`, `std::system_error`,
// `std::bad_cast`, `std::bad_typeid` are all derived from
// `std::exception`, so the generic clause must come last.

// Body emitted by every catch clause that constructs a failure result.
#define OREO_OCCT_CATCH_BODY_(scope_, T_) \
    return (scope_).template makeFailure<T_>();

#define OREO_OCCT_CATCH_T_CTX(scope_, ctx_, T_, op_name) \
    } catch (const Standard_Failure& e) { \
        (ctx_).diag().error(oreo::ErrorCode::OCCT_FAILURE, \
            std::string(op_name) + ": OCCT exception: " + \
            (e.GetMessageString() ? e.GetMessageString() : "")); \
        OREO_OCCT_CATCH_BODY_(scope_, T_) \
    } catch (Standard_Failure* e) { \
        (ctx_).diag().error(oreo::ErrorCode::OCCT_FAILURE, \
            std::string(op_name) + ": OCCT exception (ptr): " + \
            ((e && e->GetMessageString()) ? e->GetMessageString() : "")); \
        OREO_OCCT_CATCH_BODY_(scope_, T_) \
    } catch (const std::bad_alloc&) { \
        (ctx_).diag().fatal(oreo::ErrorCode::RESOURCE_EXHAUSTED, \
            std::string("Out of memory in ") + (op_name)); \
        OREO_OCCT_CATCH_BODY_(scope_, T_) \
    } catch (const std::system_error& e) { \
        (ctx_).diag().error(oreo::ErrorCode::INTERNAL_ERROR, \
            std::string(op_name) + ": System error: " + e.what()); \
        OREO_OCCT_CATCH_BODY_(scope_, T_) \
    } catch (const std::bad_cast& e) { \
        (ctx_).diag().error(oreo::ErrorCode::INTERNAL_ERROR, \
            std::string(op_name) + ": bad_cast: " + e.what()); \
        OREO_OCCT_CATCH_BODY_(scope_, T_) \
    } catch (const std::bad_typeid& e) { \
        (ctx_).diag().error(oreo::ErrorCode::INTERNAL_ERROR, \
            std::string(op_name) + ": bad_typeid: " + e.what()); \
        OREO_OCCT_CATCH_BODY_(scope_, T_) \
    } catch (const std::exception& e) { \
        (ctx_).diag().error(oreo::ErrorCode::INTERNAL_ERROR, \
            std::string(op_name) + ": C++ exception: " + e.what()); \
        OREO_OCCT_CATCH_BODY_(scope_, T_) \
    } catch (...) { \
        (ctx_).diag().error(oreo::ErrorCode::INTERNAL_ERROR, \
            std::string(op_name) + ": unknown exception"); \
        OREO_OCCT_CATCH_BODY_(scope_, T_) \
    }

#define OREO_OCCT_CATCH_NS_CTX(scope_, ctx_, op_name) \
    OREO_OCCT_CATCH_T_CTX(scope_, ctx_, oreo::NamedShape, op_name)

// ── Explicit-identifier try (OT-4) ───────────────────────────

// Enter a try-block with explicit ctx/scope identifiers. A compile-time
// static_assert confirms that the supplied identifiers actually name
// references / lvalues; catches "I passed nothing" misuse.
#define OREO_OCCT_TRY_CTX(ctx_, scope_) \
    static_assert( \
        std::is_lvalue_reference_v<decltype((ctx_))>, \
        "OREO_OCCT_TRY_CTX: first argument must be a KernelContext& in scope"); \
    static_assert( \
        std::is_lvalue_reference_v<decltype((scope_))>, \
        "OREO_OCCT_TRY_CTX: second argument must be a DiagnosticScope& in scope"); \
    try {

// ── Back-compat wrappers (default identifiers) ──────────────
//
// These assume identifiers `ctx` and `scope` are visible at the
// call site — same convention the kernel has used since day one.

#define OREO_OCCT_TRY OREO_OCCT_TRY_CTX(ctx, scope)

#define OREO_OCCT_CATCH_T(scope, ctx, T, op_name) \
    OREO_OCCT_CATCH_T_CTX(scope, ctx, T, op_name)

#define OREO_OCCT_CATCH_NS(scope, ctx, op_name) \
    OREO_OCCT_CATCH_NS_CTX(scope, ctx, op_name)

// ── Lambda-based alternative (OT-5) ──────────────────────────
//
// Preferred API for new code. Wraps the callable in the same exception
// handlers as the macros, but without preprocessor magic and with a
// proper function signature. The callable must return a T (the happy-path
// value); failure is always signalled by an exception. The returned
// OperationResult<T> has the same shape the macro path produces.

namespace oreo {

template <typename F>
auto occtSafeCall(KernelContext& ctx, DiagnosticScope& scope, const char* op, F&& f)
    -> OperationResult<decltype(f())>
{
    using T = decltype(f());
    try {
        if constexpr (std::is_void_v<T>) {
            std::forward<F>(f)();
            // void specialisation not supported — catchers below rely on T.
            // Callers returning void should use a trivial struct / bool.
            static_assert(!std::is_void_v<T>,
                "occtSafeCall: callable must return a non-void value (wrap in struct)");
        } else {
            T value = std::forward<F>(f)();
            return scope.template makeResult<T>(std::move(value));
        }
    } catch (const Standard_Failure& e) {
        ctx.diag().error(ErrorCode::OCCT_FAILURE,
            std::string(op) + ": OCCT exception: " +
            (e.GetMessageString() ? e.GetMessageString() : ""));
    } catch (Standard_Failure* e) {
        ctx.diag().error(ErrorCode::OCCT_FAILURE,
            std::string(op) + ": OCCT exception (ptr): " +
            ((e && e->GetMessageString()) ? e->GetMessageString() : ""));
    } catch (const std::bad_alloc&) {
        ctx.diag().fatal(ErrorCode::RESOURCE_EXHAUSTED,
            std::string("Out of memory in ") + op);
    } catch (const std::system_error& e) {
        ctx.diag().error(ErrorCode::INTERNAL_ERROR,
            std::string(op) + ": System error: " + e.what());
    } catch (const std::bad_cast& e) {
        ctx.diag().error(ErrorCode::INTERNAL_ERROR,
            std::string(op) + ": bad_cast: " + e.what());
    } catch (const std::bad_typeid& e) {
        ctx.diag().error(ErrorCode::INTERNAL_ERROR,
            std::string(op) + ": bad_typeid: " + e.what());
    } catch (const std::exception& e) {
        ctx.diag().error(ErrorCode::INTERNAL_ERROR,
            std::string(op) + ": C++ exception: " + e.what());
    } catch (...) {
        ctx.diag().error(ErrorCode::INTERNAL_ERROR,
            std::string(op) + ": unknown exception");
    }
    return scope.template makeFailure<T>();
}

} // namespace oreo

#endif // OREO_OCCT_TRY_H
