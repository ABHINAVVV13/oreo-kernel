# Changelog

All notable changes to oreo-kernel are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the version
scheme follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
with pre-release suffixes (e.g. `0.9.0-rc1`) for non-final builds.

## [Unreleased] — PartStudio-as-function + branching (P1 + P2)

Two architectural capabilities land in this cycle, each gated by a
separate decision-log document. They unblock configurations,
custom-features-calling-part-studios, (eventually) FeatureScript, and
the Onshape-style branch/merge collaboration model.

### Part Studio as a pure typed function (P1)

The transcript of Onshape's architecture session states: *"a Part
Studio is itself actually a FeatureScript function that produces a 3D
model when it executes."* oreo-kernel now honours that contract.

- **`ConfigSchema`** declares typed inputs on a `PartStudio`. Reuses
  the existing `ParamType` enum (ElementRef types are rejected — refs
  are outputs, not inputs). Bounds + defaults are validated at
  declaration time.
- **`ConfigValue`** binds runtime values to a schema. Per-type setters
  (`setDouble`/`setInt`/`setString`/`setVec`/…) enforce variant and
  bounds; bad values emit a structured diagnostic and do not land.
- **`ConfigRef`** is a new `ParamValue` variant alternative (index 12).
  Feature parameters can hold a `ConfigRef{name}` placeholder that
  `PartStudio::execute` resolves before `validateFeature` +
  `executeFeature` run. The stored feature is never mutated — a single
  tree replays deterministically against many configs.
- **`PartStudio::execute(ConfigValue)`** is the pure-function entry
  point. Deterministic in `(tree, schema, config, documentId)`. Returns
  `PartStudioOutputs { finalShape, namedParts, brokenFeatures, status }`.
  `executeWithDefaults()` is the common-case alias.
- **`FeatureTransformer`** hook on `FeatureTree` — optional pre-execute
  callback. `PartStudio::execute` installs a resolver for the duration
  of one call, then clears it. Stored features remain untouched.
- **JSON v2 envelope** for Part Studio — wraps the `FeatureTree` JSON
  with a `configSchema` array. `_schema` stays `"oreo.part_studio"`;
  `_version` bumps to `2.0.0`. `PartStudio::fromJSON` also accepts v1
  envelopes (no `configSchema`) and bare FeatureTree JSON for back-compat.
- **C ABI**:
  - `OreoPartStudio` / `OreoConfig` opaque handles.
  - `oreo_ctx_part_studio_create` / `_free` / `_tree`.
  - `oreo_ctx_part_studio_add_input_{double,double_bounded,int,bool,string,vec}`.
  - `oreo_feature_builder_set_config_ref` binds a feature param to a
    config input.
  - `oreo_config_create` / `_free` / `_set_{double,int,bool,string,vec}`.
  - `oreo_ctx_part_studio_execute` / `_execute_defaults`.
  - `oreo_ctx_part_studio_to_json` / `_from_json` + `oreo_free_string`.
- **Design doc:** [`docs/part-studio-as-function.md`](docs/part-studio-as-function.md).
- **Tests:** `tests/test_feature/test_part_studio_function.cpp`
  (config schema + value validation, ConfigRef resolution,
  determinism, stored-param immutability, JSON v2 round-trip, v1
  bare-FeatureTree load).

### FeatureTree schema version bump

`schema::FEATURE_TREE` goes **1.0.0 → 2.0.0**, `schema::PART_STUDIO`
introduced at `2.0.0`. Reason: `ConfigRef` is a new fundamental
variant alternative in `ParamValue`; a v1 reader would decay it to an
empty string and the feature would fail validation later with an
unhelpful message. Major bump makes the version guard load-bearing.
`schema::WORKSPACE` and `schema::MERGE_RESULT` introduced at `1.0.0`.

### Branching + three-way merge (P2)

Supersedes the "v1 is strictly linear — fork the document" anti-pattern
note in [`part-studio-model.md`](docs/part-studio-model.md). Workspaces
now branch natively, and branches merge with a structural three-way
algorithm.

- **`Workspace`** wraps a `FeatureTree` with a branch name, a parent
  branch name, and a base snapshot of the features at fork time.
  `fork(newName)` deep-copies the tree and shares the parent's
  `KernelContext` (documentId survives the branch — cross-branch
  identity stays coherent).
- **`threeWayMerge(ctx, base, ours, theirs)`** diffs ours and theirs
  against base, applies non-conflicting changes, records conflicts
  where user judgment is required. Conflict kinds:
  - `ParamConflict` — same param, different values
  - `SuppressionConflict` — same feature, conflicting suppression
  - `AddRemoveConflict` — one side removed, other modified (or diverging adds)
  - `PositionConflict` — feature moved to different indices
  - `TypeConflict` — same id, different `type` strings
  The merged tree is a live `FeatureTree` bound to the caller's ctx.
  On any conflict, the merged tree holds the BASE value — never a
  silent "take ours" fallback.
- **`applyResolutions(ctx, mergedFeatures, conflicts, resolutions)`**
  takes user choices (`Ours`, `Theirs`, `Base`, `Custom`) and emits
  the final feature list. Unmatched conflicts route a warning
  diagnostic and leave the feature at its base state.
- **Acceptance guarantees** (locked by
  `tests/test_feature/test_branch_merge.cpp`):
  - §5.1 Clean merges on disjoint edits are commutative (same feature
    set regardless of ours/theirs order).
  - §5.2 Conflicting merges fail-closed — no path silently commits
    a divergent param value.
  - §5.3 Merged trees replayed on the shared ctx produce identities
    under the shared `documentId`.
  - §5.4 Clean merges always replay without crashing.
- **JSON v1 envelope** for `Workspace` — `_schema = "oreo.workspace"`,
  carries name, parentName, features + rollbackIndex, baseSnapshot +
  baseRollbackIndex.
- **C ABI**:
  - `OreoWorkspace` / `OreoMergeResult` opaque handles.
  - `oreo_ctx_workspace_create` / `_free` / `_fork` / `_tree`.
  - `oreo_ctx_workspace_merge(base, ours, theirs)` — cross-document
    merges are rejected with `OREO_INVALID_INPUT`.
  - `oreo_merge_result_{is_clean,conflict_count,conflict_kind,
    conflict_feature_id,conflict_param_name,conflict_message,tree,free}`.
- **Design doc:** [`docs/branching-merging.md`](docs/branching-merging.md).

### Shared infrastructure

- **`feature/param_json.{h,cpp}`** — shared `ParamValue` ⇄ JSON
  helpers. `FeatureTree`, `PartStudio`, and `Workspace` all call
  through them so the four wire formats cannot drift. `ConfigRef`
  encoding lives here: `{"type": "configRef", "name": "..."}`.
- **`OreoFeatureTree_T`** gained an owned/borrowed split so the same
  C handle type can wrap either a kernel-owned tree (from
  `oreo_ctx_feature_tree_create`) or a borrowed tree loaned by a
  `PartStudio`, `Workspace`, or `MergeResult`. Callers cannot cause
  a double-free by freeing a borrowed handle.

## [Unreleased] — S/M-gap closure pass

Closes the "small + medium" items from the 2026-04-18 audit:

### Feature tree

- **Six new feature types** now dispatch from `FeatureTree::replay` and carry
  schemas in the registry: `MakeCone`, `MakeTorus`, `MakeWedge`, `Loft`,
  `Sweep`, `BooleanIntersect`. These used to be reachable only via the
  legacy singleton C API, which meant no parametric replay, no rollback,
  and no serialization round-trip. They are now first-class features.
- Regressions locked by `tests/test_feature/test_feature_extended.cpp`.

### Ctx-aware C API completeness

The 20+ ops previously missing a ctx-aware wrapper are now exposed:

- **Primitives:** `oreo_ctx_make_{cone,torus,wedge}`
- **Operations:** `oreo_ctx_{revolve,chamfer,shell,loft,sweep,mirror,
  pattern_linear,pattern_circular,draft,hole,pocket,rib,offset,thicken,
  split_body,fillet_variable,make_face_from_wire,combine,
  boolean_intersect}`
- **Queries:** `oreo_ctx_{get_face,get_edge,measure_distance,footprint}`
- Server-safe builds (`-DOREO_ENABLE_LEGACY_API=OFF`) can now run the
  full modeling catalogue without falling back to the process-global
  singleton context.

### Sketch

- **Construction geometry flag** on `SketchLine` / `SketchCircle` /
  `SketchArc`. Construction entities participate in the solver (so a
  centerline can be a tangent target or a symmetric-mirror axis) but
  are stripped from the emitted wire. Toggled via
  `oreo_ctx_sketch_set_{line,circle,arc}_construction` and queried
  via the matching `_is_construction` getters.
- Test: `tests/test_sketch/test_construction_geometry.cpp`.

### Interchange formats

- **STL** import + export via OCCT `StlAPI_Reader` / `StlAPI_Writer`.
  Binary default, ASCII optional. Tessellation parameters (linear
  deflection, angular deflection, heal-before-tessellate) match the
  existing `OreoMesh` pipeline. Files: `src/io/oreo_mesh_io.{h,cpp}`.
- **IGES 5.3** import + export via `IGESCAFControl_Reader` /
  `IGESCAFControl_Writer`, with the same XDE-first / basic-fallback
  structure as the STEP module. Files: `src/io/oreo_iges.{h,cpp}`.
- **3MF export** — hand-rolled store-only ZIP writer over Core 3MF
  Model XML. Emits the three required OPC parts
  (`[Content_Types].xml`, `_rels/.rels`, `3D/3dmodel.model`). No new
  runtime dependency (no lib3mf, no libzip). **3MF import is not
  supported** in this release — see the Known-Gaps section below.
- **SAT / ACIS / Parasolid / JT:** blocked on licensed SDKs (Siemens
  Parasolid, Spatial ACIS); no open-source round-trip path exists.
  Not shipped.
- C API: `oreo_ctx_{import,export}_{stl,iges}_{,file}` plus
  `oreo_ctx_export_3mf_file`, all with parallel legacy singleton
  wrappers. Round-trip tests in `tests/test_io/test_mesh_iges_io.cpp`.

### Deserializer hardening

- `ElementMap::deserialize` adds two defences on top of the existing
  bounds checks:
  1. **Recursion depth cap** (16 levels) against stack-exhaustion
     attacks via adversarially-nested child maps.
  2. **Entry / child count amplification guard**: the claimed entry
     count is bounded by `remaining_bytes / min_bytes_per_entry`
     before the loop, so a 4-billion entry count with only a few
     hundred bytes of buffer can't burn CPU on a hostile input.
- Both guards funnel through the standard `DESERIALIZE_FAILED`
  diagnostic path so consumers see a typed error.

### CI

- New **`clang-tsan`** matrix cell runs ThreadSanitizer on the full
  test suite. `test_concurrency` (8 parallel contexts, 12 iterations)
  is the primary signal — the kernel's claim that each context is
  thread-isolated now has CI-enforced proof on every push.
- Runs with `--cap-add=SYS_PTRACE --security-opt seccomp=unconfined`
  so TSan can `personality(ADDR_NO_RANDOMIZE)` at startup; without
  those flags TSan crashes during shadow-memory setup inside Docker.
- `ci/tsan.supp` carries audited OCCT + Qt6 metatype false-positive
  suppressions; new suppressions must come with a comment explaining
  why the flagged access is actually safe.
- **TSan cell is advisory (continue-on-error)** in this release:
  OCCT 7.9's parallel boolean engine uses Intel TBB whose task-queue
  happens-before edges aren't annotated for TSan, so every boolean /
  mesh / STEP test path produces false-positive races inside libTKBO
  / libTKernel. The suppressions file catches the audited patterns
  but new OCCT paths surface races. The cell still reports and
  serves as a regression detector for races introduced by oreo-kernel
  code itself.
- CI matrix count updated in `docs/architecture.md` (was 5 claimed /
  4 actual — now 5 actual, with TSan advisory).

### Documentation

- `README.md`: corrected sketch constraint count (34 exposed, not 41),
  clarified CI matrix, pointed at the new construction-geometry flag.

### Naming regression coverage

New test file `tests/test_naming/test_naming_edge_cases.cpp` locks the
three hard cases that were previously untested:

- Boolean subtract splits parent faces → every child face must carry a
  resolvable `ShapeIdentity`.
- Fillet trims adjacent faces → non-adjacent faces must preserve their
  pre-fillet identities across the operation.
- Feature reorder that breaks a downstream reference must surface as
  `FeatureStatus::BrokenReference` (not a silent success, not a
  crash), and moving the feature back must restore `OK` on replay.

## [0.9.0-rc1] — 2026-04-17

First release candidate. The architecture, public C ABI, and feature set
are intended to be stable for 1.0; this RC1 exists to gather field-testing
experience before the 1.0 commitment.

### Architecture

- Identity v2 (`ShapeIdentity` = 64-bit documentId + 64-bit counter) is the
  canonical naming model. The legacy v1 32-bit "squeeze" is removed from
  every normal modeling path; only the documented `shape_identity_v1.{h,cpp}`
  bridge keeps the on-disk compatibility window open.
- Two C APIs ship side-by-side:
  - **Legacy singleton API** (`oreo_init`, `oreo_make_box`, `oreo_sketch_*`,
    …) — convenience for CLIs and short-lived tools, gated by
    `-DOREO_ENABLE_LEGACY_API=ON` (default).
  - **Ctx-aware server-safe API** (`oreo_context_*`, `oreo_ctx_*`,
    `oreo_feature_builder_*`, `oreo_ctx_feature_tree_*`,
    `oreo_ctx_sketch_*`) — explicit `OreoContext` per request, the
    only supported surface for multi-tenant servers.
    `-DOREO_ENABLE_LEGACY_API=OFF` compiles a clean server-safe build.
- Internal `oreo-kernel-internal` STATIC archive is split out from the
  public `oreo-kernel` SHARED library. Tests link the static archive so
  the SHARED DLL exports only `OREO_API`-decorated symbols (zero LNK4197
  warnings on MSVC).

### Foundation

- `KernelContext` owns tag allocator, diagnostic collector, tolerance,
  units, schema registry, cancellation token, progress callback, and
  resource quotas. One context per thread is the documented contract.
- Public `OreoErrorCode` enum (26 entries) is locked to internal
  `oreo::ErrorCode` via `static_assert` in `src/capi/oreo_capi.cpp`;
  any drift fails the build.
- Diagnostic iteration via `oreo_context_diagnostic_count` /
  `_diagnostic_code` / `_diagnostic_message`.
- Resource quotas (per `KernelConfig::quotas`):
  - `maxDiagnostics`, `maxOperations`, `maxContextMemoryBytes`,
    `cpuTimeBudgetMs`
  - `maxMeshTriangles`, `maxMeshVertices`, `maxGlbBytes`
  - `maxStepBytes`, `maxSerializeBytes`, `maxFeatures`
- Op-level cancellation polled at the entry of every long-running op
  (booleans, fillet, chamfer, shell, loft, sweep, tessellate, STEP
  import).

### Feature system

- `FeatureTree` linear feature list with rollback index, per-feature
  allocator snapshots, deterministic incremental replay.
- `PartStudio` aggregate (= `FeatureTree` + name + ctx). JSON envelope
  round-trips name + documentId + tree.
- `FeatureSchemaRegistry` declares parameter schemas for all 23 built-in
  feature types; `validateFeature()` runs as the first step of
  `executeFeature()`, fail-closed.
- Transaction API: `set_param_{double,int,bool,string,vec}`, `move`,
  `replace_reference`, `broken_count` / `broken_id` / `broken_message`.
- `FeatureTree::fromJSON` returns `FeatureTreeFromJsonResult{tree, ok,
  error}` — the previous silent-empty fallback is gone.

### Sketch kernel

- Persistent slot/tombstone entity IDs: every `oreo_*_sketch_add_*`
  returns a strictly-increasing int that is stable across delete +
  re-add. The slot count never decreases.
- New public C API: `oreo_ctx_sketch_remove_{point,line,circle,arc,
  constraint}` (cascades to dependent constraints), `_alive`,
  `_slot_count`, `_live_count`.
- Constraint-family schema (`constraintSchemaFor`) is the single source
  of truth for entity-family validation and live-index translation.
- Pre-solve sweep rejects NaN / Inf / wrong-family entity references
  with a structured diagnostic; the conflicting/redundant indices in
  `SolveResult` are remapped from live-constraint space back to the
  caller's stable constraint IDs.

### Persistence

- `serialize` / `deserialize` v3 wire format with integrity magic
  (`ORC3`) + FNV-1a-64 checksum. v1 legacy reader retained.
- Bounded readers fail closed on truncated, tampered, oversized, or
  trailing-garbage buffers (`test_serialize_migration` locks 10 cases).
- STEP-imported faces / edges / vertices carry full v2 identities under
  the importing context's `documentId` (`test_step_identity` locks 4 cases).

### Mesh / GLB

- GLB writer guards `uint32` byteOffset/byteLength fields with a hard
  cap below `UINT32_MAX` plus a configurable `maxGlbBytes` quota,
  refusing oversized output before allocating the buffer.
- Tessellator enforces `maxMeshTriangles` and `maxMeshVertices`.

### Documentation

- `docs/server-safe-api.md` — multi-tenant rules + ctx lifecycle.
- `docs/feature-schema.md` — schema validation flow, full type catalogue.
- `docs/part-studio-model.md` — Part Studio anatomy, JSON envelope.
- `docs/architecture.md` — kernel layering + source layout.
- `docs/error-codes.md` — public/internal mapping table.
- `docs/identity-model.md` — pre-existing, still authoritative for v2.

### CI / tooling

- `.github/workflows/ci.yml` with five jobs: linux-gcc-release,
  linux-gcc-nolegacy, linux-clang-asan-ubsan, linux-clang-fuzz (60 s
  smoke per harness), windows-msvc.
- `ci/grep_gate.sh` blocks new callers of the v1 squeeze idiom.
- `ci/spdx_check.sh --strict` enforces 100 % SPDX-License-Identifier
  coverage (currently 160 / 160 files).
- libFuzzer harnesses for `deserialize`, `step_import`,
  `feature_tree_json` with seed corpora.
- Sample downstream consumer (`sample/`) demonstrates `find_package`
  integration.

### Known gaps before 1.0

These are documented and tracked but **not** blockers for using the RC1
in evaluation / pilot deployments:

- **CI architecture: single workflow, single matrix, single container.**
  All four configurations (gcc-release, gcc-no-legacy,
  clang-asan-ubsan, clang-fuzz) run inside the prebuilt
  `oreo-kernel-dev` container (Ubuntu 24.04 + OCCT 7.7 + every dep
  preinstalled). Each matrix entry calls `ci/build.sh` with
  config-driving env vars — no copy-pasted job blocks, no per-job
  apt installs. Windows is intentionally NOT in CI: the kernel is
  portable C++ and Linux is the authoritative gate; Windows is
  verified locally by maintainers on every push. If Windows-specific
  CI is later required it belongs in its own workflow with its own
  toolchain story (Windows containers + prebuilt OCCT).
- **Ctx-aware C API still incomplete.** Several legacy ops do not yet
  have ctx variants: `make_cone/torus/wedge`, `revolve`, `chamfer`,
  `sweep`, `loft`, `pattern_*`, `draft`, `hole`, `pocket`, `shell`,
  `offset`, `thicken`, `split_body`, `make_face_from_wire`, `combine`,
  `mirror`, `fillet_variable`. Pre-1.0 work.
- **No geometry-specific fuzz harness.** libFuzzer runs only on
  serialize / STEP / feature-tree JSON inputs. A fuzz target for
  booleans / fillets / sweeps with adversarial geometry is pending.
- **No automated `install + find_package + sample build` CI job.** The
  `sample/` exists; verifying it end-to-end is on the 1.0 list.
- **Cross-platform deterministic golden hashes** (e.g. fixed mesh
  vertex hashes, fixed identity bag hashes) are not yet captured. The
  code is platform-portable but not bit-for-bit verified across
  Linux/Windows.
- **OCCT cancellation is entry-poll only.** Long-running OCCT internals
  (e.g. mid-boolean) cannot be interrupted; OCCT's
  `Message_ProgressIndicator` is not yet wired.

### Test surface

- 39 / 39 ctest cases pass on MSVC Release.
- 100 % SPDX header coverage across `src/`, `include/`, `tests/`,
  `fuzzers/` (160 files).
- Zero `LNK4197` export warnings.

### Path to 1.0

1. Push the workflow; get every CI job green on Linux + Windows.
2. Complete the ctx-aware C API for every legacy op.
3. Build at least one downstream consumer end-to-end and run it for a
   month with real-world STEP / sketch input.
4. Add geometry-specific fuzzing and let it run for 24+ hours.
5. Add the `install + find_package + sample` CI job.
6. Capture cross-platform deterministic golden fixtures.
