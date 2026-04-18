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

## Test suite (39 ctest targets, all required green on every PR)

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

- **Context-aware API** (`oreo_ctx_`*, `OreoContext`) — per-call context holds error state, diagnostics, and tolerance config. Thread-safe and composable; this is the preferred surface.
- **Legacy global API** (`oreo_make_box`, etc.) — uses a process-global context. Retained for FFI bindings that haven't migrated yet.


| Category      | Functions                                                                                                                                                                                      |
| ------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Context       | `oreo_context_create/free`, diagnostic count/iteration, `oreo_context_last_error_message`                                                                                                      |
| Context API   | `oreo_ctx_make_box/cylinder/sphere`, `oreo_ctx_extrude`, `oreo_ctx_boolean_union/subtract`, `oreo_ctx_fillet`, `oreo_ctx_aabb`, `oreo_ctx_face_count`, `oreo_ctx_edge_count`, `oreo_ctx_mass_properties`, `oreo_ctx_import_step`, `oreo_ctx_import_step_file`, `oreo_ctx_export_step_file`, `oreo_ctx_serialize/deserialize`, `oreo_ctx_tessellate` |
| Lifecycle     | `oreo_init`, `oreo_shutdown`, `oreo_last_error`                                                                                                                                                |
| Primitives    | `oreo_make_box`, `oreo_make_cylinder`, `oreo_make_sphere`, `oreo_make_cone`, `oreo_make_torus`, `oreo_make_wedge`                                                                              |
| Geometry      | `oreo_extrude`, `oreo_revolve`, `oreo_fillet`, `oreo_chamfer`, `oreo_boolean_union/subtract/intersect`, `oreo_shell`, `oreo_loft`, `oreo_sweep`, `oreo_mirror`, `oreo_pattern_linear/circular` |
| Manufacturing | `oreo_draft`, `oreo_hole`, `oreo_pocket`, `oreo_rib`                                                                                                                                           |
| Surfaces      | `oreo_offset`, `oreo_thicken`, `oreo_split_body`, `oreo_fillet_variable`, `oreo_make_face_from_wire`, `oreo_combine`                                                                           |
| Queries       | `oreo_aabb`, `oreo_footprint`, `oreo_face_count`, `oreo_edge_count`, `oreo_get_face/edge`, `oreo_measure_distance`, `oreo_mass_properties`                                                     |
| Naming        | `oreo_face_name`, `oreo_edge_name`                                                                                                                                                             |
| I/O           | `oreo_import_step`, `oreo_import_step_file`, `oreo_export_step_file`                                                                                                                           |
| Serialization | `oreo_serialize`, `oreo_deserialize`, `oreo_free_buffer`                                                                                                                                       |
| Sketch        | `oreo_sketch_create/free`, `oreo_sketch_add_point/line/circle/arc`, `oreo_sketch_add_constraint`, `oreo_sketch_solve`, `oreo_sketch_dof`, `oreo_sketch_get_point/line`, `oreo_sketch_to_wire`  |
| Memory        | `oreo_free_solid/wire/edge/face`                                                                                                                                                               |


41 constraint types are supported in the sketch solver (distances, angles, coincidence, tangency, symmetry, equal, perpendicular, parallel, etc.).



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
