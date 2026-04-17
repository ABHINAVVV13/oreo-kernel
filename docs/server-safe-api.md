# Server-safe API guide

oreo-kernel ships with **two parallel C APIs**:

* The **legacy singleton API** (`oreo_init`, `oreo_make_box`,
  `oreo_sketch_*`, etc.) routes every call through a single process-
  global `KernelContext`. Convenient for CLIs and short-lived tools;
  **not safe** for multi-tenant servers because two concurrent
  requests will share diagnostic state, the tag allocator, and
  tolerance settings.

* The **ctx-aware API** (`oreo_context_*`, `oreo_ctx_*`,
  `oreo_feature_builder_*`, `oreo_ctx_feature_tree_*`,
  `oreo_ctx_sketch_*`) takes an explicit `OreoContext` per call. Each
  context owns its own state, so concurrent requests on distinct
  contexts cannot interact.

This document describes the rules a multi-tenant server must follow
to use the ctx-aware API safely.

## Build configuration

Server builds **must** configure with:

```
-DOREO_ENABLE_LEGACY_API=OFF
```

This compiles the singleton entry points out of the binary entirely,
so server code cannot accidentally reach for `oreo_init()` or
`oreo_make_box()` and contaminate the shared default context.

The `OREO_ENABLE_LEGACY_API=OFF` build is gated by a dedicated CI job
(`linux-gcc-nolegacy` in `.github/workflows/ci.yml`) and a no-legacy
acceptance test (`tests/test_integration/test_doc_identity_nolegacy.cpp`).

## One context per request

```
[ HTTP request ]
       │
       ▼
[ create OreoContext ]      ← oreo_context_create_with_doc(documentId, NULL)
       │
       ▼
[ thread-local work ]       ← oreo_ctx_make_box, oreo_ctx_extrude, ...
       │
       ▼
[ inspect diagnostics ]     ← oreo_context_diagnostic_count + _diagnostic_message
       │
       ▼
[ free OreoContext ]        ← oreo_context_free
```

Threading rules (`src/core/thread_safety.h`):

* **One context per thread.** Calling any `oreo_ctx_*` function on the
  same context from two threads concurrently is undefined.
* **Two contexts in two threads is supported and tested** —
  `tests/test_integration/test_concurrency.cpp` proves N parallel
  workers can do geometry, serialize, and feature-tree replay without
  cross-contamination.
* The static OCCT initialisation that runs on first context creation
  is `std::call_once`-guarded.

## Document identity

`oreo_context_create_with_doc(documentId, documentUUID)` namespaces
every shape identity produced by the context under the given
`documentId`. Use this in a server:

```c
uint64_t docId = parse_request_doc_id(request);
OreoContext ctx = oreo_context_create_with_doc(docId, NULL);
```

The kernel refuses to create two distinct 64-bit `documentId`s whose
**low 32 bits** alias — this would silently collide in the legacy v1
tag squeeze. Pick distinct low-32 prefixes per tenant.

`oreo_context_create()` (no docId) creates a "single-document" context
appropriate for offline tools. Servers should never call this.

## Quotas

Set per-context quotas via `KernelConfig::quotas` before creating the
context (in C++) — the C ABI does not yet expose quota knobs but the
defaults are sensible for a 64 GB cloud worker. Defaults:

| Field                  | Default            | Enforcement                                |
|------------------------|--------------------|--------------------------------------------|
| `maxDiagnostics`       | 10 000             | Diagnostic collector caps (always)         |
| `maxMeshTriangles`     | 50 000 000         | `tessellate()` fails closed                |
| `maxMeshVertices`      | 100 000 000        | `tessellate()` fails closed                |
| `maxGlbBytes`          | 3 GiB              | `exportGLB()` (also bounded by uint32 max) |
| `maxStepBytes`         | 512 MiB            | `importStep()` / `importStepFile()`        |
| `maxSerializeBytes`    | 1 GiB              | `serialize()` / `deserialize()`            |
| `maxFeatures`          | 10 000             | `FeatureTree::fromJSON` (ctx overload)     |

A trigger emits `OREO_RESOURCE_EXHAUSTED` (= `ErrorCode::RESOURCE_EXHAUSTED`).

## Cancellation

`KernelContext::cancellationToken()` returns a shared token. Set it
from the request handler thread; long-running ops (booleans, fillet,
chamfer, shell, loft, sweep, tessellate, STEP import) poll
`ctx.checkCancellation()` at entry and abort with `OREO_CANCELLED`.

```c
OreoContext ctx = ...;
/* Wire up the per-request cancellation. */
auto* ctxCpp = (oreo::KernelContext*)oreo_context_internal_handle(ctx);
ctxCpp->setCancellationToken(myRequestToken);
```

## Diagnostic stream

Errors and warnings are accumulated in the context's collector, not
via globals. After each `oreo_ctx_*` call returns failure:

```c
int n = oreo_context_diagnostic_count(ctx);
for (int i = 0; i < n; ++i) {
    int code = oreo_context_diagnostic_code(ctx, i);
    const char* msg = oreo_context_diagnostic_message(ctx, i);
    log("oreo:%d %s", code, msg);
}
```

The codes match the documented public enum
(see [error-codes.md](error-codes.md)).

## Anti-patterns

* **Sharing a single `OreoContext` across worker threads.** Crash or
  data-race risk; not supported.
* **Mixing legacy and ctx-aware APIs in the same process.** The
  legacy default context bypasses your per-request quotas and
  diagnostics; if you must use a legacy entry point, route it through
  a dedicated worker process.
* **Persisting `OreoSolid` handles across context lifetimes.** A
  handle is owned by the context that produced it; freeing the
  context invalidates every handle it issued.

## End-to-end test surface

| Test                                                  | What it locks down                                                 |
|-------------------------------------------------------|--------------------------------------------------------------------|
| `test_doc_identity_nolegacy`                          | Acceptance: every identity-v2 invariant holds in the OFF build     |
| `test_ctx_apis`                                       | Sketch + feature-edit ctx-aware API surface                        |
| `test_step_identity`                                  | STEP-imported faces/edges carry a v2 identity                      |
| `test_concurrency`                                    | N parallel ctx workers don't cross-contaminate                     |
| `test_replay_golden`                                  | Server / client compute identical replay identities                |
| `test_transaction_api`                                | Per-type setters, broken-feature query, move, replace_reference    |
| `test_capi_consumer` (pure C99)                       | Public header is C-clean and works from a C-only consumer          |
| `test_serialize_migration`                            | Wire format fail-closes on tampered/truncated/oversized buffers    |
