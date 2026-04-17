# oreo-kernel

Standalone C++ CAD geometry kernel built on OpenCASCADE Technology (OCCT), with topological naming stability derived from FreeCAD's element-map implementation and constraint solving via PlaneGCS.

**Current version:** 0.2.0 — context-aware API plus a hardened foundation layer. 19/19 test binaries green on MSVC Release; ~402 gtest cases across the foundation suites.

## What is this?

oreo-kernel is the core geometry engine for oreoCAD. It is **not** a fork of FreeCAD — it is a greenfield C++ library that:

- Wraps OCCT for solid modeling (primitives, booleans, fillets, sweeps, lofts, etc.)
- Integrates FreeCAD's topological naming / element-map system for stable face/edge references across edits
- Embeds the PlaneGCS 2D constraint solver for sketch solving
- Exposes everything through a flat C API (`oreo_kernel.h`) suitable for FFI bindings
- Implements a parametric feature tree with replay, undo, and serialization
- Provides a foundation layer for error handling, diagnostics, units, schema migration, and validation (see [Foundation layer](#foundation-layer))

## Build requirements

| Requirement | Version |
|---|---|
| CMake | 3.20+ |
| vcpkg | latest |
| C++ compiler | MSVC 2022+ or GCC 11+ |
| OS | Windows 10/11, Linux |

Dependencies managed by vcpkg (see `vcpkg.json`):
- OpenCASCADE (OCCT)
- Eigen3
- Boost (graph, iostreams)
- Google Test
- nlohmann/json
- Qt6 Core

## Build instructions

### Using CMake presets (recommended)

```bash
cmake --preset default
cmake --build --preset release
ctest --preset default
```

### Manual (without presets)

```bash
cmake -B build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=<path-to-vcpkg>/scripts/buildsystems/vcpkg.cmake \
    -DCMAKE_BUILD_TYPE=Release

cmake --build build --config Release
```

### Running tests

```bash
cd build
ctest --output-on-failure
```

Build directories (`build/`, `build_vs/`, `build_msvc/`) are gitignored.

## Test suite (19 executables, ~580 gtest cases)

**Unit / geometry**
- `test_smoke` — basic lifecycle and primitive creation
- `test_geometry` — extrude, revolve, fillet, chamfer, booleans, patterns
- `test_mesh`, `test_mesh_export` — tessellation / meshing (incl. GLB export)

**Subsystems**
- `test_topo_naming` — topological naming stability across edits
- `test_sketch_solver` — 2D constraint solver (41 constraint types)
- `test_boolean_stress` — heavy boolean operation sequences
- `test_step_roundtrip` — STEP import/export fidelity
- `test_serialize` — binary serialization round-trip
- `test_robustness` — crash resistance with malformed inputs
- `test_feature_tree` — parametric replay, parameter edits, broken refs

**Foundation (8 binaries, ~402 cases)**
- `test_foundation` — context, diagnostics, units, schema, validation, determinism
- `test_foundation_hardened` — NaN/Inf, fail-closed paths, composable diagnostics
- `test_foundation_production` — production-path invariants
- `test_foundation_battle` — adversarial threading, overflow, corruption, C API boundary
- `test_foundation_battle2` — singleton-context C API coverage (only built when `OREO_ENABLE_LEGACY_API=ON`)
- `test_foundation_modules` — standalone modules: cancellation, feature flags, localization, RNG, profile/metrics, arena, assertion framework
- `test_foundation_obs` — config loader (env + JSON overlay), logging sink interface, resource-quota enforcement, diagnostic metadata
- `test_foundation_new_features` — KernelContext clone/merge/safeReset, progress callback, OperationResult combinators, `occtSafeCall`, validation helpers, schema introspection

## API overview

The public API is a single C header: `include/oreo_kernel.h` (~91 exported functions).

Two parallel surfaces are exposed:

- **Context-aware API** (`oreo_ctx_*`, `OreoContext`) — per-call context holds error state, diagnostics, and tolerance config. Thread-safe and composable; this is the preferred surface.
- **Legacy global API** (`oreo_make_box`, etc.) — uses a process-global context. Retained for FFI bindings that haven't migrated yet.

| Category | Functions |
|---|---|
| Context | `oreo_context_create/free`, `oreo_context_has_errors/warnings`, `oreo_context_error_count`, `oreo_context_last_error_message` |
| Lifecycle | `oreo_init`, `oreo_shutdown`, `oreo_last_error` |
| Primitives | `oreo_make_box`, `oreo_make_cylinder`, `oreo_make_sphere`, `oreo_make_cone`, `oreo_make_torus`, `oreo_make_wedge` |
| Geometry | `oreo_extrude`, `oreo_revolve`, `oreo_fillet`, `oreo_chamfer`, `oreo_boolean_union/subtract/intersect`, `oreo_shell`, `oreo_loft`, `oreo_sweep`, `oreo_mirror`, `oreo_pattern_linear/circular` |
| Manufacturing | `oreo_draft`, `oreo_hole`, `oreo_pocket`, `oreo_rib` |
| Surfaces | `oreo_offset`, `oreo_thicken`, `oreo_split_body`, `oreo_fillet_variable`, `oreo_make_face_from_wire`, `oreo_combine` |
| Queries | `oreo_aabb`, `oreo_footprint`, `oreo_face_count`, `oreo_edge_count`, `oreo_get_face/edge`, `oreo_measure_distance`, `oreo_mass_properties` |
| Naming | `oreo_face_name`, `oreo_edge_name` |
| I/O | `oreo_import_step`, `oreo_import_step_file`, `oreo_export_step_file` |
| Serialization | `oreo_serialize`, `oreo_deserialize`, `oreo_free_buffer` |
| Sketch | `oreo_sketch_create/free`, `oreo_sketch_add_point/line/circle/arc`, `oreo_sketch_add_constraint`, `oreo_sketch_solve`, `oreo_sketch_dof`, `oreo_sketch_get_point/line`, `oreo_sketch_to_wire` |
| Memory | `oreo_free_solid/wire/edge/face` |

41 constraint types are supported in the sketch solver (distances, angles, coincidence, tangency, symmetry, equal, perpendicular, parallel, etc.).

## Foundation layer

The `src/core/` layer provides the primitives every other subsystem relies on:

- **`KernelContext`** — per-call container for tolerance, units, tag allocator, diagnostics, cancellation, progress, resource quotas, schema registry. `tags()` and `diag()` are accessor methods (not public data members); construction validates `KernelConfig` and clamps NaN/Inf/negative values with a Fatal diagnostic.
- **`DiagnosticCollector`** — structured errors with `Severity { Debug, Trace, Info, Warning, Error, Fatal }`, O(1) cached error/warning/degraded flags, JSON `toJSON`/`fromJSON`, pluggable `IDiagnosticSink` observers, bounded size (`maxDiagnostics`) with a single `DIAG_TRUNCATED` marker on overflow, and a `thread_local` ring buffer as last-resort fallback under `bad_alloc`.
- **`TagAllocator`** — deterministic 64-bit tag allocation; `std::atomic<int64_t>` counter; SipHash-2-4 for `documentIdFromString`; per-allocator OCCT 32-bit map for stable `toOcctTag` without cross-document collisions; `peek`/`allocateRange`/`snapshot`/`restore` for introspection and rollback.
- **`OperationResult<T>`** — success/failure type with cached flag queries, `map`/`flatMap`/`andThen` combinators, `valueOrThrow()` for exception mode, `tryValue()` for pointer mode, `operator==` comparing (ok, severity, code) pairs.
- **`occt_try.h`** — `occtSafeCall<F>(ctx, scope, op, f)` wraps OCCT calls, mapping `std::bad_alloc → RESOURCE_EXHAUSTED/Fatal`, `Standard_Failure → OCCT_FAILURE`, `std::system_error`/`bad_cast`/`bad_typeid → INTERNAL_ERROR`. Legacy macros `OREO_OCCT_TRY/CATCH` still work.
- **`OcctStaticGuard`** — per-context mutex for OCCT's global `Interface_Static`; supports string, int, and real settings; exception-safe restore; reports originally-unset keys via `unsetKeys()`.
- **`UnitSystem`** — length, angle, mass, volume, density, stress, torque enums with `safeMultiply` overflow detection and locale-independent ASCII parsing.
- **`SchemaRegistry`** — versioned JSON migrations; O(1) lookup via composite-key map; duplicate registration throws; `testMigration` round-trip helper; `freezeSchemas()` + `mutableSchemas()` lifecycle.
- **`ConfigLoader`** — overlay `KernelConfig` from environment variables (`OREO_LINEAR_PRECISION`, `OREO_TAG_SEED`, …) or a JSON file.
- **Optional scaffolding** (present in the tree, not yet consumed by geometry code): `Metrics`/`ProfileScope`, `MessageCatalog`, `FeatureFlags`, `DeterministicRNG`, `Arena` bump allocator, `OREO_ASSERT`/`OREO_VERIFY` framework.

Thread-safety contract: a `KernelContext` is single-owner by default. `ContextThreadGuard` (debug builds) aborts on cross-thread access; `ContextSharedMutex` offers opt-in reader/writer locking for callers that need it.

## Architecture

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
  tests/                   # GTest suite (19 test executables, ~580 cases total)
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
