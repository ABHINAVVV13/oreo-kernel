// SPDX-License-Identifier: LGPL-2.1-or-later

// assertion.h — CC-14: debug assertion framework for oreo-kernel.
//
// Provides OREO_ASSERT (debug-only) and OREO_VERIFY (always-on) macros that
// delegate to handleAssertionFailure(). Failure handling is configurable at
// runtime via AssertionConfig::setAction() — abort, throw std::logic_error,
// or report-and-continue.
//
// OREO_ASSERT_CTX takes a KernelContext-like object that exposes .diag() and
// emits an INVALID_STATE error before invoking the handler; useful when the
// caller wants the failure captured in the structured diagnostics stream in
// addition to the process-level reaction.
//
// Kept header-only: handleAssertionFailure is defined inline so the macros
// can be used without forcing a link-time dependency on a separate .cpp.

#ifndef OREO_ASSERTION_H
#define OREO_ASSERTION_H

#pragma once

#include "diagnostic.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace oreo {

enum class AssertAction : uint8_t {
    Abort,              // fprintf to stderr + std::abort(). Default in debug.
    Throw,              // throw std::logic_error. Useful for unit tests.
    ReportAndContinue,  // fprintf to stderr, return. Default in release.
};

// Runtime-tunable policy for OREO_ASSERT / OREO_VERIFY failures.
// Access is lock-free via an atomic function-local static; safe to call from
// any thread including inside the failure handler.
class AssertionConfig {
public:
    static AssertAction action() noexcept {
        return slot().load(std::memory_order_relaxed);
    }

    static void setAction(AssertAction a) noexcept {
        slot().store(a, std::memory_order_relaxed);
    }

private:
    static std::atomic<AssertAction>& slot() noexcept {
        // Default: Abort in debug builds, ReportAndContinue in release.
        // OREO_VERIFY escalates this choice — it always calls the handler,
        // whereas OREO_ASSERT is compiled out entirely under NDEBUG.
#ifdef NDEBUG
        static std::atomic<AssertAction> s{AssertAction::ReportAndContinue};
#else
        static std::atomic<AssertAction> s{AssertAction::Abort};
#endif
        return s;
    }
};

// Dispatch an assertion failure.
//
// NOT noexcept — AssertAction::Throw genuinely throws std::logic_error; the
// other two dispositions (Abort, ReportAndContinue) never throw. Callers who
// cannot tolerate an exception (e.g. destructors) should use Abort or
// ReportAndContinue, never Throw.
inline void handleAssertionFailure(const char* expr,
                                   const char* file,
                                   int line,
                                   const char* function,
                                   const char* message) {
    const char* safeExpr     = expr     ? expr     : "<null-expr>";
    const char* safeFile     = file     ? file     : "<null-file>";
    const char* safeFunction = function ? function : "<null-function>";
    const char* safeMessage  = message  ? message  : "";

    const AssertAction action = AssertionConfig::action();

    // Always produce a line on stderr — even Throw logs before unwinding so
    // that failures in noexcept contexts still leave a trace.
    std::fprintf(stderr,
                 "[oreo] assertion failed: %s\n"
                 "       message : %s\n"
                 "       at      : %s:%d\n"
                 "       in      : %s\n",
                 safeExpr, safeMessage, safeFile, line, safeFunction);
    std::fflush(stderr);

    switch (action) {
        case AssertAction::Abort:
            std::abort();
        case AssertAction::Throw: {
            // noexcept on the function means this throw will call std::terminate
            // if the caller is also noexcept — which is the correct behaviour
            // for a failed invariant. Honour Throw anyway so non-noexcept sites
            // can catch and continue.
            std::string msg;
            msg.reserve(64 + std::char_traits<char>::length(safeExpr));
            msg.append("oreo assertion failed: ");
            msg.append(safeExpr);
            if (safeMessage[0] != '\0') {
                msg.append(" (");
                msg.append(safeMessage);
                msg.append(")");
            }
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
            // Safe to throw here — the enclosing noexcept will fire
            // std::terminate, but that is the documented behaviour of
            // AssertAction::Throw on a noexcept caller.
            throw std::logic_error(msg);
#else
            std::abort();
#endif
        }
        case AssertAction::ReportAndContinue:
        default:
            return;
    }
}

} // namespace oreo

#ifdef NDEBUG
  #define OREO_ASSERT(expr, msg)           ((void)0)
  #define OREO_ASSERT_CTX(ctx, expr, msg)  ((void)0)
#else
  #define OREO_ASSERT(expr, msg)                                                           \
      do {                                                                                 \
          if (!(expr)) {                                                                   \
              ::oreo::handleAssertionFailure(#expr, __FILE__, __LINE__, __func__, (msg));  \
          }                                                                                \
      } while (0)

  // OREO_ASSERT_CTX: same as OREO_ASSERT but also pushes a structured
  // INVALID_STATE diagnostic onto ctx.diag() before dispatching. ctx must
  // expose diag() returning something with an .error(ErrorCode, std::string)
  // overload — typically a KernelContext.
  #define OREO_ASSERT_CTX(ctx, expr, msg)                                                  \
      do {                                                                                 \
          if (!(expr)) {                                                                   \
              (ctx).diag().error(                                                          \
                  ::oreo::ErrorCode::INVALID_STATE,                                        \
                  std::string("Assertion failed: ") + #expr + " (" + (msg) + ")");         \
              ::oreo::handleAssertionFailure(#expr, __FILE__, __LINE__, __func__, (msg));  \
          }                                                                                \
      } while (0)
#endif

// Always-on check. Runs in release too. Useful for invariants whose failure
// indicates kernel corruption that must never be silently swallowed.
#define OREO_VERIFY(expr, msg)                                                             \
    do {                                                                                   \
        if (!(expr)) {                                                                     \
            ::oreo::handleAssertionFailure(#expr, __FILE__, __LINE__, __func__, (msg));    \
        }                                                                                  \
    } while (0)

#endif // OREO_ASSERTION_H
