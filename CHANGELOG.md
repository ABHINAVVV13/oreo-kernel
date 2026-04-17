# Changelog

All notable changes to oreo-kernel are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the version
scheme follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
with pre-release suffixes (e.g. `0.9.0-rc1`) for non-final builds.

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

- **CI on Linux/Clang has not been executed end-to-end.** First push to
  GitHub triggers it. Please report any breakage via Issues.
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
