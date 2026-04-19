# oreo-kernel architecture

A high-level map of the kernel for new contributors. For deep-dive
specifications see the per-subsystem docs:

* [identity-model.md](identity-model.md) — ShapeIdentity, naming v2.
* [server-safe-api.md](server-safe-api.md) — multi-tenant rules.
* [part-studio-model.md](part-studio-model.md) — feature timeline.
* [feature-schema.md](feature-schema.md) — per-feature param schemas.
* [error-codes.md](error-codes.md) — public OreoErrorCode reference.

## What it is

oreo-kernel is a C++17 CAD kernel built on **OpenCASCADE Technology
(OCCT)**. It exposes a single flat C ABI
([`include/oreo_kernel.h`](../include/oreo_kernel.h)) so it can be
embedded in cloud workers, native desktop apps, JS-via-WASM clients,
and language bindings via plain FFI.

Substrate (locked, do not propose alternatives):

| Layer        | Library            |
|--------------|--------------------|
| BRep / topology / booleans / fillets | OpenCASCADE 7.x |
| Sketch solver | PlaneGCS (FreeCAD-derived) |
| Linear algebra | Eigen 3 |
| Graph data   | Boost::graph |
| JSON         | nlohmann::json |
| Element-map data structures | Qt6 Core (FreeCAD-derived) |

## Source layout

```
include/oreo_kernel.h           ← the ONLY public header
src/
├── core/                       ← context, diagnostics, units, validation, schemas
│   ├── kernel_context.{h,cpp}      → KernelContext + KernelConfig
│   ├── diagnostic.{h,cpp}          → ErrorCode / Severity / collector
│   ├── operation_result.h          → OperationResult<T>
│   ├── tag_allocator.{h,cpp}       → identity-v2 tag allocator
│   ├── shape_identity.{h,cpp}      → ShapeIdentity (docId+counter)
│   ├── resource_quotas.h           → per-context limits
│   ├── cancellation.h              → CancellationToken
│   ├── progress.h                  → progress callback
│   └── …
├── naming/                     ← element map, FreeCAD-derived MappedName
│   ├── element_map.{h,cpp}         → core ElementMap
│   ├── topo_shape_adapter.{h,cpp}  → bridge to OCCT TopoDS_Shape
│   ├── map_shape_elements.{h,cpp}  → post-op naming
│   └── freecad/                    → vendored FreeCAD naming primitives
├── sketch/                     ← PlaneGCS + ctx-aware wrapper
├── geometry/                   ← OCCT wrappers (booleans, fillet, …)
├── query/                      ← measurements
├── io/                         ← STEP I/O + binary serialize
├── feature/                    ← FeatureTree, Feature, Schema, PartStudio
├── mesh/                       ← tessellation + GLB export
└── capi/                       ← flat C ABI implementation
fuzzers/                        ← libFuzzer harnesses (Linux/Clang)
corpora/                        ← seed inputs for the fuzzers
tests/                          ← gtest suites + property-based fuzz
ci/                             ← cross-platform CI gates (bash)
docs/                           ← per-subsystem specs (this folder)
sample/                         ← reference downstream consumer
```

## Layering

Strictly bottom-up — every layer may use anything below it but never
above. Cycles are forbidden by convention; the dependency direction
is enforced via include directives.

```
                             ┌──────────────────┐
                             │  capi/ (flat C)  │
                             └─────────┬────────┘
                                       │
                ┌──────────────────────┼──────────────────────┐
                │                      │                      │
            feature/                geometry/             mesh/
                │                      │                      │
                ├──────► query/       ├──────► io/            │
                │                      │                      │
                └──────► naming/ ─────┘                      │
                              │                               │
                              ▼                               │
                         core/ (context, diagnostics, units)──┘
                              │
                              ▼
                         OCCT / Boost / Eigen / Qt
```

* **core/** owns KernelContext and the shared types every other
  layer consumes. Has no dependencies on naming, geometry, etc.
* **naming/** depends on core only. Provides ElementMap and the
  OCCT↔FreeCAD glue.
* **geometry/**, **io/**, **mesh/**, **query/**, **sketch/** depend
  on core + naming. They never call into each other; the FeatureTree
  composes them.
* **feature/** depends on geometry/, query/, naming/, core. Provides
  Feature, FeatureTree, FeatureSchema, PartStudio.
* **capi/** wraps everything below it in extern "C" entry points.
  No business logic lives here — every function is a thin marshalling
  layer over the C++ implementation.

## Two C APIs

* **Legacy singleton API** (`oreo_init`, `oreo_make_box`, …): single
  process-global context. Built only when
  `-DOREO_ENABLE_LEGACY_API=ON` (default for back-compat).
* **Ctx-aware API** (`oreo_ctx_*`, `oreo_context_*`,
  `oreo_feature_builder_*`, `oreo_ctx_feature_tree_*`,
  `oreo_ctx_sketch_*`): explicit OreoContext per request. Always
  available. Required for multi-tenant servers.

## Build artefacts

```
build_msvc/Release/
├── oreo-kernel.dll                 ← public SHARED library (consumers link this)
├── oreo-kernel.lib                 ← Windows import lib
├── oreo-kernel-internal.lib        ← internal STATIC archive (tests link this)
├── test_*.exe                      ← gtest binaries
├── test_capi_consumer.exe          ← pure-C99 consumer smoke test
└── glb_smoke.exe                   ← manual GLB export tool
```

The two-target split (added 2026-04-17) keeps the public DLL surface
clean: only symbols decorated with `OREO_API` leave the boundary,
giving the production DLL a deterministic export table while internal
tests retain full C++ access via the static archive. See the
[server-safe-api.md](server-safe-api.md) build-configuration section.

## Quotas & cancellation

Every long-running op (booleans, fillets, shell, loft, sweep,
tessellate, STEP import) polls `ctx.checkCancellation()` at entry,
and every output-producing op (mesh / GLB / serialize / STEP / feature
tree) consults `ctx.quotas()` before allocating large buffers. The
table of quota fields and their default values lives in
[server-safe-api.md](server-safe-api.md).

## Wire formats

| Format               | Owner                     | Versions accepted | Header marker        |
|----------------------|---------------------------|-------------------|----------------------|
| Binary serialize     | `io/oreo_serialize.{h,cpp}` | v1 (read-only) + v3 (current) | first byte = 3 + magic `ORC3` |
| Element map (inner)  | `naming/element_map.cpp`   | v3 (current)      | first 4 bytes = `EM03` (ish) |
| FeatureTree JSON     | `feature/feature_tree.cpp` | v1 schema         | `_schema: "oreo.feature_tree"` |
| PartStudio JSON      | `feature/part_studio.cpp`  | v1 envelope       | `_schema: "oreo.part_studio"` |
| GLB                  | `mesh/oreo_mesh_export.cpp`| glTF 2.0 binary   | magic `glTF` |

All readers fail-closed on truncated / corrupt / oversized buffers.
The migration test suite
([`test_serialize_migration.cpp`](../tests/test_serialize/test_serialize_migration.cpp))
locks down the v1↔v3 compatibility contract.

## CI matrix

`.github/workflows/ci.yml` runs five jobs on every push, all inside the
`ghcr.io/abhinavvv13/oreo-kernel-dev:latest` container (Ubuntu 24.04 +
OCCT 7.9.3 + deps pinned):

1. **gcc-release** — full build + ctest + `grep_gate.sh` + `spdx_check.sh --strict`.
2. **gcc-no-legacy** — `OREO_ENABLE_LEGACY_API=OFF` acceptance gate.
3. **clang-asan-ubsan** — AddressSanitizer + UndefinedBehaviorSanitizer test run.
4. **clang-tsan** — ThreadSanitizer on the concurrency + feature-tree test set.
5. **clang-fuzz** — 60s libFuzzer smoke per harness (deserialize, step_import,
   feature_tree_json).

Windows is intentionally not in the matrix: the determinism suite enforces
byte-identical output across consumers, and pinning the entire toolchain at
the image layer is the only honest way to guarantee that. Windows is verified
locally by maintainers. See `docs/deployment.md` for the full rationale.

`ci/grep_gate.sh` blocks the v1 documentId-squeeze idiom from re-entering
the codebase. `ci/spdx_check.sh` reports SPDX header coverage. TSan
suppressions live in `ci/tsan.supp`.
