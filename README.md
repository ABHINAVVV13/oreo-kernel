# oreo-kernel

**Status: v0.9.0-rc1 — release candidate, API-stable, pre-1.0 field testing.**
See [CHANGELOG.md](CHANGELOG.md) for what's in this release and what's still
pending for 1.0.

Standalone C++ CAD geometry kernel built on OpenCASCADE Technology (OCCT), with topological naming stability derived from FreeCAD's element-map implementation and constraint solving via PlaneGCS.

## What is this?

oreo-kernel is the core geometry engine for oreoCAD. It is **not** a fork of FreeCAD — it is a greenfield C++ library that:

- Wraps OCCT for solid modeling (primitives, booleans, fillets, sweeps, lofts, etc.)
- Integrates FreeCAD's topological naming / element-map system for stable face/edge references across edits
- Embeds the PlaneGCS 2D constraint solver for sketch solving
- Exposes everything through a flat C API (`oreo_kernel.h`) suitable for FFI bindings
- Implements a parametric feature tree with replay, undo, and serialization
- Provides a foundation layer for error handling, diagnostics, units, schema migration, and validation (see [Foundation layer](#foundation-layer))

## Getting started

> oreo-kernel is **Docker-first**. There are no supported native
> Windows / Linux / macOS build paths — the determinism test suite
> asserts byte-identical output across every host, and the only
> honest way to guarantee that is to pin the toolchain at the image
> layer. The full rationale is in [docs/deployment.md](docs/deployment.md).

### For consumers — use oreo-kernel in your code

```dockerfile
FROM ghcr.io/abhinavvv13/oreo-kernel:v0.9.0
# Your source + CMakeLists with find_package(OreoKernel REQUIRED)
# and target_link_libraries(... PRIVATE OreoKernel::oreo-kernel)
```

Complete worked example (C++, CMake, Dockerfile): see
[docs/deployment.md § For consumers](docs/deployment.md#for-consumers).

### For contributors — develop on oreo-kernel itself

Requirements:

| | |
|---|---|
| Docker Desktop (Windows / macOS) *or* Docker Engine (Linux) | [docker.com/products/docker-desktop](https://www.docker.com/products/docker-desktop/) |
| VS Code + the **Dev Containers** extension | [code.visualstudio.com](https://code.visualstudio.com/) |

That's the entire prerequisite list. No MSVC, no vcpkg, no OCCT, no Qt SDK.

```bash
git clone https://github.com/ABHINAVVV13/oreo-kernel
cd oreo-kernel
code .
# → VS Code shows "Reopen in Container"  →  click it
# → terminal, compiler, IntelliSense, debugger all run in the container
```

Inside the container:

```bash
cmake --preset debug          # configure
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Available CMake presets: `debug`, `release`, `no-legacy`, `asan`,
`fuzz` (see [CMakePresets.json](CMakePresets.json)).

Full contributor guide, including how to reproduce every CI matrix
cell locally and how to debug in VS Code:
[docs/deployment.md § For contributors](docs/deployment.md#for-contributors).

### What the image contains

Built from a single multi-stage [docker/Dockerfile](docker/Dockerfile):

| Component | Version | Source |
|---|---|---|
| OCCT (OpenCASCADE) | **7.9.3** | built from source, pinned git tag `V7_9_3` |
| Base OS | Ubuntu 24.04 | apt |
| GCC | 13.x | apt (`build-essential`) |
| Clang + sanitizer runtimes | 18.x | apt (`clang`, `libclang-rt-18-dev`, `llvm-18`) |
| Boost | 1.83 | apt |
| Qt 6 (Core only, headless) | 6.4.2 | apt |
| Eigen 3 | 3.4.0 | apt |
| nlohmann-json | 3.11.3 | apt |
| GTest | 1.14.0 | apt |

Everything is pinned. Rebuilds from scratch are deterministic.

## Test suite (47 ctest targets in the default Release build, all required green on every PR)

Exact counts (verifiable in CI): **47** ctest targets in the legacy
Release build (`linux-gcc-release`), **43** in the server-safe no-
legacy build (`linux-gcc-nolegacy`). The 4-test delta is the legacy-
gated suite: `test_integration`, `test_doc_identity`, `test_perf`,
`test_foundation_battle2`.

**Unit / geometry**

- `test_smoke` — basic lifecycle and primitive creation
- `test_geometry` — extrude, revolve, fillet, chamfer, booleans, patterns
- `test_mesh`, `test_mesh_export` — tessellation / meshing (incl. GLB export)
- `test_mesh_iges_io` — STL / IGES / 3MF interchange round-trips

**Subsystems**

- `test_topo_naming`, `test_naming_edge_cases` — topological naming
  stability across edits; FreeCAD-derived element-map corner cases
- `test_sketch_solver`, `test_sketch_persistence`, `test_construction_geometry`
  — 2D constraint solver (34 public constraint types), stable entity
  IDs across edits, construction-geometry flag behaviour
- `test_boolean_stress` — heavy boolean operation sequences
- `test_step_roundtrip`, `test_step_identity` — STEP import/export
  fidelity + v2-identity on imported sub-shapes
- `test_serialize`, `test_serialize_migration` — binary serialization
  round-trip + v1/v2/v3 wire-format compatibility corpus
- `test_robustness` — crash resistance with malformed inputs

**Feature tree + Part Studio + branching**

- `test_feature_tree` — parametric replay, parameter edits, broken
  refs, JSON round-trip
- `test_feature_schema`, `test_feature_extended` — schema validator;
  MakeCone / MakeTorus / MakeWedge / Loft / Sweep / BooleanIntersect
- `test_part_studio_function` — PartStudio-as-function acceptance
  (ConfigSchema / ConfigValue / ConfigRef / v2 JSON envelope)
- `test_branch_merge` — Workspace branching + three-way merge
- `test_audit_2026_04_19` — app-readiness audit acceptance (C ABI
  parametric edits + read API, config fingerprint guard, strict
  JSON decode, ROOT-ref rejection, workspace JSON)

**Foundation (13 binaries under `tests/test_foundation/`)**

- `test_foundation` — context, diagnostics, units, schema, validation, determinism
- `test_foundation_hardened` — NaN/Inf, fail-closed paths, composable diagnostics
- `test_foundation_production` — production-path invariants
- `test_foundation_battle` — adversarial threading, overflow, corruption, C API boundary
- `test_foundation_battle2` — singleton-context C API coverage (only built when `OREO_ENABLE_LEGACY_API=ON`)
- `test_foundation_modules` — standalone modules: cancellation, feature flags, localization, RNG, profile/metrics, arena, assertion framework
- `test_foundation_obs` — config loader (env + JSON overlay), logging sink interface, resource-quota enforcement, diagnostic metadata
- `test_foundation_new_features` — KernelContext clone/merge/safeReset, progress callback, OperationResult combinators, `occtSafeCall`, validation helpers, schema introspection
- `test_foundation_audit_fixes`, `test_hardening_2026_04_18` — post-0.9.0-rc1 audit regressions
- `test_shape_identity`, `test_tag_allocator_v2`, `test_naming_v2` —
  v2 identity model (Phases 1-3)

**Integration + C ABI surface**

- `test_integration` — end-to-end (legacy-gated)
- `test_doc_identity`, `test_doc_identity_nolegacy` — 64-bit
  documentId survives every layer in both build configurations
- `test_ctx_apis` — ctx-aware sketch + feature-edit C API
- `test_concurrency` — per-context state isolation across N parallel
  contexts (NOT a proof of OCCT in-process thread-safety; see
  [docs/process-model.md](docs/process-model.md))
- `test_replay_golden` — deterministic golden replay, server/client parity
- `test_transaction_api` — per-type setters, broken-feature query,
  move, replace-reference
- `test_capi_consumer` — pure-C99 consumer of the public `oreo_kernel.h`

## API overview

The public API is a single C header: `include/oreo_kernel.h`
(**307 OREO_API-exported declarations** as of v0.9.0-rc1;
`grep -c '^OREO_API' include/oreo_kernel.h` is the canonical count).

Two parallel surfaces are exposed:

- **Context-aware API** (`oreo_ctx_`*, `OreoContext`) — per-call
  context holds error state, diagnostics, tolerance config, quotas,
  document identity. **The only surface supported for multi-tenant
  server deployments.** Always on (no build gate).
- **Legacy singleton API** (`oreo_init`, `oreo_make_box`,
  `oreo_sketch_*`, `oreo_ctx_sketch_*`'s legacy counterparts, etc.)
  — routes through a process-global `KernelContext`. Gated behind
  `OREO_ENABLE_LEGACY_API=ON` (the build default) and compiled OUT
  of server-safe builds. Retained for CLIs, offline tools, and
  language bindings that haven't migrated yet.

| Category           | Representative functions                                                                                                                                                                                                                                                                                                            |
| ------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Library identity   | `oreo_kernel_version`                                                                                                                                                                                                                                                                                                               |
| Context            | `oreo_context_create`, `oreo_context_create_with_doc`, `oreo_context_free`, `oreo_context_document_id`, `oreo_context_has_errors/warnings`, `oreo_context_diagnostic_count/code/message`, `oreo_context_last_error_message`                                                                                                         |
| Ctx primitives     | `oreo_ctx_make_box/cylinder/sphere/cone/torus/wedge`                                                                                                                                                                                                                                                                                |
| Ctx geometry       | `oreo_ctx_extrude/revolve/fillet/chamfer`, `oreo_ctx_boolean_union/subtract/intersect`, `oreo_ctx_shell/loft/sweep/mirror`, `oreo_ctx_pattern_linear/circular`, `oreo_ctx_draft/hole/pocket/rib`, `oreo_ctx_offset/thicken/split_body/fillet_variable/make_face_from_wire/combine`                                                   |
| Ctx queries        | `oreo_ctx_aabb/face_count/edge_count/get_face/get_edge/measure_distance/footprint/mass_properties`                                                                                                                                                                                                                                  |
| Ctx identity v2    | `oreo_face_shape_id`, `oreo_edge_shape_id`, `oreo_ctx_face_name`, `oreo_ctx_edge_name`, `oreo_ctx_serialize`, `oreo_ctx_deserialize`                                                                                                                                                                                                |
| Ctx interchange    | `oreo_ctx_import_step/stl/iges`, `oreo_ctx_export_step_file/stl_file/iges_file/3mf_file`                                                                                                                                                                                                                                            |
| Ctx sketch         | `oreo_ctx_sketch_create/free/add_*/solve/dof/get_*/to_wire`, stable entity IDs, construction-geometry flags                                                                                                                                                                                                                         |
| Feature edit       | `oreo_feature_builder_*` (create/reset + 10 typed setters for double/int/bool/string/vec/pnt/dir/ax1/ax2/pln, single+list ref, config ref), `oreo_ctx_feature_tree_*` (add/remove/suppress/move, per-type param updates, introspection/readers by feature+param index, broken-feature query, replay, JSON)                          |
| PartStudio         | `oreo_ctx_part_studio_create/free/tree/input_count`, `oreo_ctx_part_studio_add_input_*`, `oreo_ctx_part_studio_execute/_defaults/_to_json/_from_json`, `oreo_ctx_part_studio_schema_fingerprint`, `oreo_config_create/free/set_*/schema_fingerprint`                                                                                |
| Workspace / merge  | `oreo_ctx_workspace_create/fork/free/tree/name/parent_name/merge`, `oreo_ctx_workspace_to_json/from_json`, `oreo_merge_result_*`, `oreo_resolution_set_*`, `oreo_merge_result_apply_resolutions`                                                                                                                                    |
| Tessellation       | `oreo_ctx_tessellate`, `oreo_mesh_*` (vertex/triangle/face-group accessors)                                                                                                                                                                                                                                                         |
| Memory             | `oreo_free_solid/wire/edge/face/string/buffer`                                                                                                                                                                                                                                                                                      |
| Legacy (LEGACY=ON) | `oreo_init`, `oreo_shutdown`, `oreo_last_error`, `oreo_make_*`, `oreo_extrude/revolve/…`, `oreo_sketch_*`, `oreo_serialize`, `oreo_deserialize`, `oreo_free_buffer`, `oreo_tessellate`                                                                                                                                              |

34 constraint types are exposed by the public sketch API (distances, angles,
coincidence, tangency, symmetry, equal, perpendicular, parallel, etc.) — see
`ConstraintType` in [src/sketch/oreo_sketch.h](src/sketch/oreo_sketch.h). The
underlying PlaneGCS solver implements 37 low-level primitives; a handful are
internal-only (elliptical-arc range, internal-alignment-point, Snell, B-spline
parametric constraints) and are not currently reachable from the public API.



```
oreo-kernel/
  include/
    oreo_kernel.h          # Public C API (the only header consumers need)
  src/
    core/                  # Foundation: context, diagnostics, units, schema,
                           #   validation, tolerance, tag allocator, thread
                           #   safety, cancellation, progress, quotas, metrics,
                           #   profile, config loader, sinks, assertion,
                           #   localization, feature flags, RNG, memory arena
    naming/                # Topological naming adapter + FreeCAD element-map
      freecad/             # Extracted FreeCAD TNaming code (LGPL)
    sketch/                # PlaneGCS constraint solver integration
    geometry/              # OCCT wrappers: primitives, booleans, fillets, sweeps…
    query/                 # Bounding box, mass properties, face/edge queries
    io/                    # STEP import/export, shape healing
    feature/               # Parametric feature tree (replay, undo, serialize)
    mesh/                  # Tessellation / meshing
    capi/                  # C API implementation (bridges C calls to C++ internals)
    worker/                # (reserved, not yet implemented)
  tests/                   # GTest suite — 47 ctest targets in the default Release build
                           #   (43 in the server-safe no-legacy build); each ctest entry is its
                           #   own executable, plus glb_smoke (manual-run mesh dump) and the 3
                           #   fuzzer harnesses under fuzzers/.
```

### Layer dependencies

```
capi --> feature --> geometry --> naming --> core
                 --> sketch               --> io
                 --> query
                 --> mesh
```

The `core/` foundation layer is depended on by every other module. It owns error/diagnostic propagation, unit conversion, schema versioning, input validation, and thread-safety primitives.

## License

This project is licensed under the **GNU Lesser General Public License v2.1** (LGPL-2.1).

This license was chosen for compatibility with:

- OpenCASCADE Technology (OCCT), which is LGPL-2.1
- FreeCAD topological naming code extracted into `src/naming/freecad/`, which is LGPL-2.0+

See [LICENSE](LICENSE) for the full text.
