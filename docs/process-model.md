# oreo-kernel production process model

<!--
SPDX-License-Identifier: LGPL-2.1-or-later

Status: ratified 2026-04-19. This document is the authoritative
deployment guarantee for multi-tenant hosts. Any change to it needs
sign-off from whoever owns the operational side of a hosted install.
-->

## Summary

**oreo-kernel is not safe to run as a single-process multi-threaded
service that executes OCCT operations concurrently.** Hosts that want
to service concurrent requests MUST pick one of these two models:

1. **Per-worker-process isolation.** Each in-flight request gets its
   own OS process, own `KernelContext`, own OCCT state. The process
   pool bounds concurrency. This is the recommended model and the one
   the reference [oreo-worker](../../oreo-worker) uses.
2. **Per-process serialisation of OCCT work.** One process, one mutex
   serialising every OCCT call, as many worker threads as you like
   but only one runs kernel code at a time. Useful for desktop /
   embedded deployments with no concurrency need; useless for a
   hosted service.

Any third model ("I will share one process across threads and just
never let two requests touch OCCT at the same time") is the same as
option 2 with extra steps — write it down as option 2 and budget for
the lock cost.

Continuing past these is undefined behaviour: the kernel can and will
produce inconsistent element maps, corrupt tessellations, and
occasionally crash. `test_determinism` has caught some of those; TSan
has flagged more; the rest live in the long tail of OCCT's static
caches that we cannot annotate without patching upstream.

## Why this is not optional

Three reasons, in priority order.

### 1. OCCT has process-wide static state that is not thread-safe

The modelling kernel (OCCT 7.9) maintains several caches and
singletons that make a single OCCT call from thread A observable from
thread B with no synchronisation. Documented in
[src/core/thread_safety.h](../src/core/thread_safety.h) rule #2.
Concrete examples we have audited:

- **BRepMesh's internal maps.** Every feature that tessellates (STL
  export, 3MF export, the `oreo_ctx_tessellate` path, the OpenGL
  display path, OCCT's own bool operand meshing) touches a process-
  global map of triangulation handles. Two concurrent tessellations
  can interleave writes and produce a triangulation that looks
  consistent but references the wrong face.
- **Standard_Mutex arena allocators.** OCCT's `NCollection_*`
  containers lazy-allocate out of a process-global arena. The arena
  is serialised inside OCCT, but the allocation count is read and
  published out-of-order, so a reader on thread B can see a partially
  constructed node.
- **TBB task arenas.** When OCCT is built with `TBB_USE_PREVIEW=1`
  (our Docker image is), OCCT's parallel boolean engine and parallel
  meshing register tasks onto a process-global TBB task arena. Two
  concurrent parallel operations feed into the same arena; TBB's
  work-stealing scheduler assumes exclusive access to its own
  internal queues.
- **Global trace / debug hooks.** OCCT writes diagnostics to `fd 1`
  (see [feedback_stdout_isolation.md](../../../.claude/projects/C--Users-AbhinavPutta-Desktop-projects-oreoCAD/memory/feedback_stdout_isolation.md)).
  Two concurrent operations corrupt each other's stdout, breaking
  JSON-RPC framing for any worker that forwards OCCT output.

### 2. `test_determinism` assumes per-process single-threading

The determinism test suite ([tests/test_determinism](../tests/test_determinism))
hashes a canonical feature-tree build and asserts byte-identical
output against a golden hash. That assertion only holds when the
operations run in a deterministic order. Concurrent execution
schedules the tag allocator, cache writes, and element-map inserts
non-deterministically across threads, producing legitimate-looking
shapes that hash to different values. A multi-threaded production
host that passed `test_determinism` at build time can still silently
corrupt a customer's feature tree at run time.

### 3. TSan is a regression detector, not a proof of thread safety

CI runs a TSan cell as a **hard gate** (promoted from advisory on
2026-04-19 once the last ambiguous-suppression + vector<bool>-race
findings were closed). A green TSan cell proves:

- The kernel's own synchronisation (per-context mutex, tag allocator
  atomics, diagnostic collector) does not have a race that TSan can
  detect during the test suite.
- OCCT's TBB-backed parallel paths (boolean engine, NCollection
  allocator, Handle refcount) produce only races matched by the
  audited entries in [ci/tsan.supp](../ci/tsan.supp).

A green TSan cell does NOT prove:

- That concurrent OCCT calls on two distinct contexts in the same
  process are safe. **TSan cannot see races that don't fire during
  the test workload.** The boolean engine's TBB task queue, the
  BRepMesh internal cache, and OCCT's global type registry all have
  code paths that don't execute in every test but DO execute in
  production. The suppression file covers the audited patterns only;
  unaudited OCCT paths are still undefined under multi-threaded use.
- That every caller pattern is race-free. TSan instruments the code
  paths the test suite executes, not every possible caller.

**The process-model split stands regardless of TSan colour.** Per-
worker-process isolation or per-process serialisation is still the
only supported deployment shape. TSan is the CI gate that catches
regressions in oreo-kernel code against its own single-threaded
contract; production thread safety is a process-model question, not
a sanitizer question. See the extensive matrix-cell comment in
`ci.yml` for the details.

## The two supported deployment models

### Model A: per-worker-process isolation (recommended)

**What it looks like:**

```
         ┌──────────────────────────────────────┐
         │  Gateway / HTTP front-end (Node, Go,  │
         │  Python, …) — no kernel calls.        │
         └───────────────┬──────────────────────┘
                         │ one request per
                         │ OS process (fork or spawn)
                         ▼
         ┌──────────────────────────────────────┐
         │  oreo-worker process instance        │
         │  ┌──────────────────────────────┐    │
         │  │  one KernelContext           │    │
         │  │  one OCCT                    │    │
         │  │  one thread (+ OCCT's own    │    │
         │  │    internal TBB threads —    │    │
         │  │    fine, they're confined    │    │
         │  │    to this process)          │    │
         │  └──────────────────────────────┘    │
         └──────────────────────────────────────┘
```

Each worker process owns its OCCT state and its kernel context. The
gateway pools workers (typically `nprocs × 2` to amortise startup
cost) and hands each request to a free worker. Two workers never
share memory, so no OCCT-level race is possible between them.

**Trade-offs:**

- Memory: each worker carries its own OCCT shared-library mapping
  and caches. Budget ≈ 40-80 MB resident per worker at baseline, rising
  with feature-tree size.
- Startup: an unwarmed worker spends 150-300 ms on first OCCT init
  (depends on host CPU + disk). Keep a small pool warm.
- IPC cost: serialising feature-tree / shape input + output across
  the process boundary is measurable (see oreo-worker's JSON-RPC
  framing). The kernel's `oreo_ctx_serialize` / `oreo_ctx_deserialize`
  use the v3 wire format which is both compact and stable.

**Reference implementation:**
[../../oreo-worker](../../oreo-worker) is a ready-made per-process
worker built on this model. It exposes the kernel over JSON-RPC on
stdin/stdout, handles OCCT stdout isolation (redirecting `fd 1` to
`fd 2` before importing so the wire protocol stays clean), and is
the model the reference gateway spawns.

### Model B: per-process serialisation

**What it looks like:**

```
         ┌──────────────────────────────────────┐
         │  Single process, N worker threads,    │
         │  ONE global OCCT mutex.               │
         │                                       │
         │  Thread work:                         │
         │    ┌─────────┐     ┌─────────┐        │
         │    │ T1 run  │ ──► │ T2 run  │  ...   │
         │    │ OCCT    │     │ OCCT    │        │
         │    └─────────┘     └─────────┘        │
         │    ← one at a time through the mutex →│
         └──────────────────────────────────────┘
```

Every kernel entry point takes a process-global `std::mutex` before
touching OCCT; the critical section is the entire kernel call plus
any state that references the produced `NamedShape`. Effective
concurrency on kernel work is 1.

**When to pick this:**

- Desktop / embedded single-user applications (no concurrency need
  → the mutex is always uncontended).
- Prototyping / tests.
- **Not** for hosted services. You give up all parallelism on the
  dimension that matters while keeping the process memory footprint
  of the threaded model.

**Not supplied by the kernel.** Apps that choose this model are
responsible for the mutex and are responsible for documenting every
entry point that acquires it. We have no internal testing for this
model — the whole kernel test surface runs in model A.

## What the kernel itself guarantees

To help both models hold together, the kernel provides:

- **`KernelContext` is context-bound** — see `OREO_CONTEXT_BOUND` in
  [thread_safety.h](../src/core/thread_safety.h). Each thread must
  own its own context. The debug-build `ContextThreadGuard` aborts
  on a cross-thread access.
- **`TagAllocator` is thread-safe within its owning context** under
  the "one context per thread" rule. A single context's tags are
  never simultaneously mutated from two threads because there is
  never a second thread holding the same context.
- **`DiagnosticCollector` is bound to the context and writes under
  the same rule.** Readers on the same thread as the writer see a
  consistent view; cross-thread reads are undefined.
- **Immutable snapshots.** `SchemaVersion`, `UnitSystem`, the
  `FeatureSchemaRegistry` singleton, and the `ConfigSchema`'s
  `fingerprint()` are pure read-only data after construction and
  safe to share across threads without synchronisation.
- **`BRepMesh` is invoked with `isInParallel=False` everywhere in
  the kernel.** The audit 2026-04-19 caught one path (direct STL
  export) that had parallel=true and fixed it to match the rest of
  the kernel; all tessellation entry points now pass `parallel=False`.

## What the kernel does NOT guarantee

- Safety of concurrent calls on the same context. Undefined.
- Safety of concurrent calls that share any `OreoSolid` / `OreoMesh`
  / `OreoFeatureTree` handle. Undefined.
- Absence of OCCT-internal races between concurrent operations on
  *different* contexts in the same process. **This is the one that
  forces the two-model split.** Per-worker-process isolation avoids
  it by design; per-process serialisation avoids it by taking a
  global lock.

## Checklist for a new deployment

Before shipping a production host that embeds oreo-kernel:

- [ ] Pick model A or B and write down which. "We're figuring it out"
      means you picked B and should own the mutex.
- [ ] If model A: document your worker pool size, your restart policy
      (at least on OOM / crash), your gateway's queueing model.
- [ ] If model B: audit every kernel entry point in your code and
      confirm the mutex is acquired. The kernel will not help you.
- [ ] Confirm no code path opens a user-visible stdout before the
      kernel imports OCCT. See
      [feedback_stdout_isolation.md](../../../.claude/projects/C--Users-AbhinavPutta-Desktop-projects-oreoCAD/memory/feedback_stdout_isolation.md).
- [ ] Run the TSan CI cell locally against your integration layer.
      It will not prove thread safety (see §3 above), but it WILL
      catch an accidental introduction of new shared mutable state
      in YOUR code, which is where your bugs will be.
- [ ] Set `OMP_NUM_THREADS=1` and `TBB_NUM_THREADS=1` on workers if
      you want deterministic intra-worker timing; OCCT's internal
      parallelism can still help single-request throughput but only
      at the cost of non-determinism you may already be trying to
      avoid.

## References

- [src/core/thread_safety.h](../src/core/thread_safety.h) — contract
  annotations + `ContextThreadGuard` runtime check.
- [src/mesh/oreo_mesh.h](../src/mesh/oreo_mesh.h) — `MeshParams::parallel`
  default + rationale.
- [src/io/oreo_mesh_io.cpp](../src/io/oreo_mesh_io.cpp) — STL / 3MF
  export meshing call sites (all `parallel=False` after 2026-04-19).
- [.github/workflows/ci.yml](../.github/workflows/ci.yml) — TSan cell
  matrix entry with the OCCT/TBB noise rationale.
- [ci/tsan.supp](../ci/tsan.supp) — suppression list for audited
  OCCT/TBB patterns.
