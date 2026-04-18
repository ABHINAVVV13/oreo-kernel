cmake_minimum_required(VERSION 3.20)

# Numeric version (CMake's project() requires X.Y.Z). Pre-release suffix
# is carried separately in OREO_KERNEL_PRE_RELEASE so it can survive the
# trip into the C++ source via target_compile_definitions without
# breaking find_package(OreoKernel 0.9.0)-style version checks.
project(oreo-kernel VERSION 0.9.0 LANGUAGES CXX C)
set(OREO_KERNEL_PRE_RELEASE "rc1" CACHE STRING
    "Pre-release suffix appended to OREO_KERNEL_VERSION_FULL (e.g. rc1, beta2). \
Empty string for a final release.")
if(OREO_KERNEL_PRE_RELEASE STREQUAL "")
    set(OREO_KERNEL_VERSION_FULL "${PROJECT_VERSION}")
else()
    set(OREO_KERNEL_VERSION_FULL "${PROJECT_VERSION}-${OREO_KERNEL_PRE_RELEASE}")
endif()
message(STATUS "oreo-kernel ${OREO_KERNEL_VERSION_FULL}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Dependencies ──────────────────────────────────────────────────────────────

find_package(OpenCASCADE CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(Eigen3 CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS graph iostreams)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core)

# ── OCCT STEP-library layout detection ───────────────────────────────────────
#
# OCCT 7.7 consolidated STEP I/O into a single TKDESTEP library covering
# both STEPControl_* and STEPCAFControl_*. Earlier versions (including
# Ubuntu 24.04's apt-shipped 7.6.3) still use the split layout:
#   * STEPControl_Reader/Writer     → TKSTEP
#   * STEPCAFControl_Reader/Writer  → TKXDESTEP
#
# We use imported-target existence checks (not version parsing) so this
# works regardless of which OCCT config format is on disk. Windows vcpkg
# ships 7.7+ → TKDESTEP path; Linux apt ships 7.6.x → TKSTEP+TKXDESTEP.
if(TARGET TKDESTEP)
    set(OCCT_STEP_LIBS TKDESTEP)
else()
    set(OCCT_STEP_LIBS TKSTEP TKXDESTEP)
endif()
message(STATUS "OCCT STEP libraries: ${OCCT_STEP_LIBS}")

# ── Build options ─────────────────────────────────────────────────────────────
#
# OREO_ENABLE_LEGACY_API (default ON):
#   When ON, the flat C API exposes singleton-context entry points
#   (oreo_make_box, oreo_fillet, oreo_init, oreo_last_error, the entire
#   sketch C API, etc.) which route through a process-global KernelContext.
#   These are convenient for small clients and CLI tools but are NOT safe
#   for multi-tenant server processes where contexts must be explicitly
#   isolated per request.
#
#   Turn OFF for server builds (-DOREO_ENABLE_LEGACY_API=OFF). Only the
#   explicit OreoContext API (oreo_ctx_*) and stateless helpers remain.
#   The test target test_foundation_battle2 is also omitted when OFF
#   because its fixtures exercise the legacy surface.
option(OREO_ENABLE_LEGACY_API "Expose singleton-context C API (not safe for server builds)" ON)

# Deprecated. Internal C++ tests now link against the dedicated static
# archive `oreo-kernel-internal` instead of forcing every symbol to be
# exported from the public DLL. The option is preserved for one release
# so existing build scripts that pass -DOREO_EXPORT_ALL_SYMBOLS_FOR_TESTS=OFF
# do not break, but it is unused — the DLL never sets
# CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS=ON anymore. Will be removed in a
# future release; clean production builds need do nothing.
option(OREO_EXPORT_ALL_SYMBOLS_FOR_TESTS
    "[DEPRECATED, no-op] Was used to expose internal symbols for tests" OFF)
if(OREO_EXPORT_ALL_SYMBOLS_FOR_TESTS)
    message(WARNING
        "OREO_EXPORT_ALL_SYMBOLS_FOR_TESTS is deprecated and ignored. "
        "Tests now link against the oreo-kernel-internal static archive.")
endif()

# ── Sanitizers ───────────────────────────────────────────────────────────────
#
# OREO_ENABLE_SANITIZER selects compile/link flags for one or more
# Clang/GCC sanitizers. Supported values (case-insensitive, plus
# combinations joined with `+`):
#   none                — no sanitizer (default)
#   address             — AddressSanitizer (memory bugs)
#   undefined           — UndefinedBehaviorSanitizer
#   address+undefined   — both (recommended for CI)
#   thread              — ThreadSanitizer (data races, mutually exclusive)
#   memory              — MemorySanitizer (uninitialised reads, Clang only)
#
# MSVC has limited sanitizer support — when targeting MSVC, only
# `address` is honoured; everything else triggers a warning and is
# treated as `none`.
set(OREO_ENABLE_SANITIZER "none" CACHE STRING
    "Sanitizer flavour: none|address|undefined|address+undefined|thread|memory")

if(NOT OREO_ENABLE_SANITIZER STREQUAL "none")
    string(TOLOWER "${OREO_ENABLE_SANITIZER}" _oreo_san_lower)
    set(_oreo_san_flags "")
    if(MSVC)
        if(_oreo_san_lower MATCHES "address")
            list(APPEND _oreo_san_flags "/fsanitize=address")
        endif()
        if(NOT _oreo_san_flags AND NOT _oreo_san_lower STREQUAL "none")
            message(WARNING
                "OREO_ENABLE_SANITIZER='${OREO_ENABLE_SANITIZER}' has no MSVC "
                "equivalent; use Clang/GCC for ubsan/tsan/msan.")
        endif()
    else()
        if(_oreo_san_lower MATCHES "address")
            list(APPEND _oreo_san_flags -fsanitize=address -fno-omit-frame-pointer)
        endif()
        if(_oreo_san_lower MATCHES "undefined")
            # -fno-sanitize=vptr: the vptr check emits calls to
            # __ubsan_vptr_type_cache / __ubsan_handle_dynamic_type_cache_miss_*
            # which live in libclang_rt.ubsan_standalone_cxx — the C++
            # UBSan runtime. When a C consumer (test_capi_consumer or a
            # downstream user writing plain C) links liboreo-kernel.so,
            # clang's C driver doesn't auto-link that C++ runtime and the
            # link fails with undefined references. Disabling vptr keeps
            # the flat C ABI linkable under sanitizers.
            #
            # -fno-sanitize-recover=undefined makes every remaining UBSan
            # check abort on the first error, EXCEPT alignment which we
            # mark recoverable via -fsanitize-recover=alignment. Why:
            # OCCT's NCollection_TListNode<T> downcasts through a
            # pointer-arithmetic trick that UBSan's alignment check
            # flags as a false positive (the runtime memory is actually
            # aligned; the static type just claims more than OCCT's
            # ListNode base provides). Recoverable mode lets the
            # suppressions file in ci/ubsan.supp silence that specific
            # check without weakening coverage of the rest. All other
            # UBSan categories (int overflow, null deref, OOB, bool/enum
            # cast, shift-out-of-range, function-ptr mismatch, etc.)
            # remain non-recoverable, abort immediately, and cannot be
            # silenced — that's where real bugs live.
            list(APPEND _oreo_san_flags
                -fsanitize=undefined
                -fno-sanitize=vptr
                -fno-sanitize-recover=undefined
                -fsanitize-recover=alignment)
        endif()
        if(_oreo_san_lower MATCHES "thread")
            list(APPEND _oreo_san_flags -fsanitize=thread)
        endif()
        if(_oreo_san_lower MATCHES "memory")
            list(APPEND _oreo_san_flags -fsanitize=memory)
        endif()
    endif()

    if(_oreo_san_flags)
        message(STATUS "Enabling sanitizers: ${_oreo_san_flags}")
        add_compile_options(${_oreo_san_flags})
        add_link_options(${_oreo_san_flags})
    endif()
endif()

# ── Fuzzers (libFuzzer, Linux/macOS Clang only) ──────────────────────────────
#
# When ON, builds the libFuzzer harnesses in fuzzers/. Requires Clang
# with libFuzzer support and the matching sanitizer flags
# (`-DOREO_ENABLE_SANITIZER=address+undefined` is the standard pairing).
# Silently disabled on MSVC (which lacks libFuzzer).
option(OREO_BUILD_FUZZERS "Build libFuzzer harnesses (Clang only)" OFF)
if(OREO_BUILD_FUZZERS AND MSVC)
    message(WARNING
        "OREO_BUILD_FUZZERS=ON has no effect on MSVC — libFuzzer requires Clang.")
    set(OREO_BUILD_FUZZERS OFF)
endif()

# ── Source list ───────────────────────────────────────────────────────────────
#
# Explicit source list (no GLOB_RECURSE). CMake's docs warn that glob
# patterns "will not, in general, detect new files added by the user
# without a re-run of cmake" — but more importantly, in a build that
# shares its source tree across CI runners and developer machines,
# enumerating every TU here is the only way to guarantee deterministic
# build sets and to make code review surface added/removed sources.
#
# Adding a new TU: append it to the appropriate sub-list below.
# Renaming: update the line in place.
set(OREO_SOURCES
    # ── core/ — context, diagnostics, units, validation, schema, etc.
    src/core/config_loader.cpp
    src/core/diagnostic.cpp
    src/core/diagnostic_scope.cpp
    src/core/feature_flags.cpp
    src/core/kernel_context.cpp
    src/core/localization.cpp
    src/core/occt_scope_guard.cpp
    src/core/oreo_error.cpp
    src/core/oreo_init.cpp
    src/core/oreo_tolerance.cpp
    src/core/schema.cpp
    src/core/shape_identity_v1.cpp
    src/core/units.cpp
    src/core/validation.cpp
    # ── naming/ — element-map, identity v2, FreeCAD-derived helpers.
    src/naming/element_map.cpp
    src/naming/map_shape_elements.cpp
    src/naming/named_shape.cpp
    src/naming/topo_shape_adapter.cpp
    src/naming/freecad/ElementMap.cpp
    src/naming/freecad/ElementNamingUtils.cpp
    src/naming/freecad/IndexedName.cpp
    src/naming/freecad/MappedElement.cpp
    src/naming/freecad/MappedName.cpp
    src/naming/freecad/StringHasher.cpp
    # ── sketch/ — PlaneGCS solver + ctx-aware wrapper.
    src/sketch/Constraints.cpp
    src/sketch/GCS.cpp
    src/sketch/Geo.cpp
    src/sketch/oreo_sketch.cpp
    src/sketch/qp_eq.cpp
    src/sketch/SubSystem.cpp
    # ── geometry/ — primitive + booleans + fillets + manufacturing.
    src/geometry/oreo_boolean.cpp
    src/geometry/oreo_extrude.cpp
    src/geometry/oreo_fillet_chamfer.cpp
    src/geometry/oreo_manufacturing.cpp
    src/geometry/oreo_mirror_pattern.cpp
    src/geometry/oreo_primitives.cpp
    src/geometry/oreo_revolve.cpp
    src/geometry/oreo_shell_loft_sweep.cpp
    src/geometry/oreo_surface.cpp
    # ── query/ — measurements + queries.
    src/query/oreo_query.cpp
    # ── io/ — STEP / serialize / shape fix.
    src/io/oreo_serialize.cpp
    src/io/oreo_step.cpp
    src/io/shape_fix.cpp
    # ── capi/ — flat C ABI.
    src/capi/oreo_capi.cpp
    # ── feature/ — feature tree + replay + schema + Part Studio.
    src/feature/feature.cpp
    src/feature/feature_schema.cpp
    src/feature/feature_tree.cpp
    src/feature/part_studio.cpp
    # ── mesh/ — tessellation + GLB export.
    src/mesh/oreo_mesh.cpp
    src/mesh/oreo_mesh_export.cpp
)

# ── Helper: apply common include paths + third-party deps to a target ─────────
#
# Used by both the SHARED public library and the STATIC internal archive.
# Public include dir is exposed PUBLIC; src/ is PRIVATE because consumers
# only get the flat C ABI.
function(_oreo_kernel_configure_common target)
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/src/naming/freecad
            ${OpenCASCADE_INCLUDE_DIR}
    )

    target_link_libraries(${target}
        PUBLIC
            # OCCT core
            TKernel
            TKMath
            TKBRep
            TKGeomBase
            TKGeomAlgo
            TKG2d
            TKG3d
            TKTopAlgo
            TKPrim
            TKBool
            TKBO
            TKFillet
            TKOffset
            TKShHealing
            TKMesh
            # OCCT STEP I/O — see OCCT_STEP_LIBS detection above
            ${OCCT_STEP_LIBS}
            TKXSBase
            TKXCAF
            TKLCAF
            TKCAF
            TKCDF
            # Eigen + Boost (for PlaneGCS + element-map)
            Eigen3::Eigen
            Boost::graph
            Boost::iostreams
            # Qt Core (for FreeCAD element-map data structures)
            Qt6::Core
            # JSON (for feature tree serialization)
            nlohmann_json::nlohmann_json
    )

    # Required for M_PI on Windows (OCCT headers need it)
    target_compile_definitions(${target} PUBLIC _USE_MATH_DEFINES)

    # Propagate CMake project version to schema.h so KERNEL_VERSION stays in sync.
    # OREO_KERNEL_VERSION_STRING is the SemVer numeric ("0.9.0") and is
    # what gets stamped into serialized document headers — keep it
    # parser-friendly. OREO_KERNEL_VERSION_FULL adds the pre-release
    # suffix ("0.9.0-rc1") and is what oreo_kernel_version() returns at
    # runtime for human-facing displays.
    target_compile_definitions(${target} PUBLIC
        OREO_KERNEL_VERSION_STRING="${PROJECT_VERSION}"
        OREO_KERNEL_VERSION_FULL_STRING="${OREO_KERNEL_VERSION_FULL}"
    )

    # Propagate legacy-API flag (gates public declarations in include/oreo_kernel.h).
    if(OREO_ENABLE_LEGACY_API)
        target_compile_definitions(${target} PUBLIC OREO_ENABLE_LEGACY_API)
    endif()
endfunction()

# ── oreo-kernel: production SHARED library ───────────────────────────────────
#
# Compiled with OREO_KERNEL_EXPORTS so OREO_API expands to dllexport on
# Windows (visibility("default") elsewhere). Consumers including
# oreo_kernel.h see OREO_API as dllimport. CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS
# is intentionally NOT set — only symbols decorated with OREO_API leave
# the DLL boundary, which is the production-correct behaviour.
add_library(oreo-kernel SHARED ${OREO_SOURCES})
set_target_properties(oreo-kernel PROPERTIES POSITION_INDEPENDENT_CODE ON)
_oreo_kernel_configure_common(oreo-kernel)
target_compile_definitions(oreo-kernel PRIVATE OREO_KERNEL_EXPORTS)
if(WIN32)
    target_compile_definitions(oreo-kernel
        PUBLIC
            $<BUILD_INTERFACE:OREO_BUILDING_DLL>
    )
endif()

# ── oreo-kernel-internal: STATIC archive for internal C++ tests ──────────────
#
# Same source list as the SHARED lib, compiled with OREO_KERNEL_STATIC
# (PUBLIC) so OREO_API expands to nothing — no dllexport/dllimport on
# this code path, which is what a static archive needs. Tests link
# against this target so they can call internal C++ symbols directly
# without forcing them through the DLL boundary. The archive is never
# installed or distributed — it exists purely to keep the public DLL
# surface clean while preserving full test coverage.
add_library(oreo-kernel-internal STATIC ${OREO_SOURCES})
set_target_properties(oreo-kernel-internal PROPERTIES POSITION_INDEPENDENT_CODE ON)
_oreo_kernel_configure_common(oreo-kernel-internal)
target_compile_definitions(oreo-kernel-internal PUBLIC OREO_KERNEL_STATIC)

# ── Tests ─────────────────────────────────────────────────────────────────────

enable_testing()

# Test helper: links every test against the STATIC internal archive so
# tests can call C++ internals directly. This decouples test linkage
# from the public DLL ABI — the SHARED oreo-kernel.dll only ever exports
# OREO_API-marked symbols.
function(oreo_add_test TEST_NAME TEST_SOURCE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_link_libraries(${TEST_NAME} PRIVATE
        oreo-kernel-internal
        GTest::gtest
        GTest::gtest_main
    )
    target_include_directories(${TEST_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/naming/freecad
        ${OpenCASCADE_INCLUDE_DIR}
    )
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endfunction()

# ── Test suite ────────────────────────────────────────────────────────────────

# Unit tests
oreo_add_test(test_smoke     tests/test_unit/test_smoke.cpp)
oreo_add_test(test_geometry  tests/test_unit/test_geometry.cpp)

# Topo naming regression
oreo_add_test(test_topo_naming tests/test_naming/test_topo_naming.cpp)

# Sketch solver battery
oreo_add_test(test_sketch_solver tests/test_sketch/test_sketch_solver.cpp)

# Sketch persistence — stable IDs across delete/edit + cascade.
# Closes the auditor's "persistent sketch IDs" gap (Phase D6).
oreo_add_test(test_sketch_persistence tests/test_sketch/test_sketch_persistence.cpp)

# Boolean stress tests
oreo_add_test(test_boolean_stress tests/test_boolean/test_boolean_stress.cpp)

# STEP round-trip
oreo_add_test(test_step_roundtrip tests/test_step/test_step_roundtrip.cpp)

# Serialize round-trip
oreo_add_test(test_serialize tests/test_serialize/test_serialize.cpp)

# Serialize migration (v1/v2/v3 wire-format compatibility + adversarial
# fail-closed corpus). Closes the auditor's "compatibility corpus
# missing" gap.
oreo_add_test(test_serialize_migration tests/test_serialize/test_serialize_migration.cpp)

# Robustness (crash/malformed input)
oreo_add_test(test_robustness tests/test_robustness/test_robustness.cpp)

# Feature tree (parametric replay, parameter changes, broken refs, serialization)
oreo_add_test(test_feature_tree tests/test_feature/test_feature_tree.cpp)

# Feature parameter schemas + pre-execution validator
oreo_add_test(test_feature_schema tests/test_feature/test_feature_schema.cpp)

# Foundation (context, diagnostics, units, schema, validation, determinism)
oreo_add_test(test_foundation tests/test_foundation/test_foundation.cpp)

# ShapeIdentity + v1 bridge (Phase 1 of the identity-model v2 hardening).
# Does not depend on OREO_ENABLE_LEGACY_API — uses only core headers.
oreo_add_test(test_shape_identity tests/test_foundation/test_shape_identity.cpp)

# TagAllocator v2 (Phase 2). Exercises the v2 allocation / snapshot /
# toOcctTag surface plus the v1-compat wrappers routing through it.
oreo_add_test(test_tag_allocator_v2 tests/test_foundation/test_tag_allocator_v2.cpp)

# Naming v2 (Phase 3). Covers ;:P postfix round-trips, rightmost-marker
# algorithm in extractShapeIdentity, ElementMap v3 format, and v2 reader
# downgrade warnings.
oreo_add_test(test_naming_v2 tests/test_foundation/test_naming_v2.cpp)

# Foundation hardened (adversarial tests: NaN, Inf, fail-closed, composable diagnostics)
oreo_add_test(test_foundation_hardened tests/test_foundation/test_foundation_hardened.cpp)

# Foundation modules (standalone: cancellation, feature flags, localization, RNG, profile, arena, assertion)
oreo_add_test(test_foundation_modules tests/test_foundation/test_foundation_modules.cpp)

# Foundation observability (config loader, logging sink, resource quotas, diagnostic metadata)
oreo_add_test(test_foundation_obs tests/test_foundation/test_foundation_obs.cpp)

# Foundation new features (KernelContext clone/merge/safeReset, cancellation, progress, combinators, validators, schema introspection)
oreo_add_test(test_foundation_new_features tests/test_foundation/test_foundation_new_features.cpp)

# Determinism (tag streams, shape hashes, golden op-sequence, cross-platform-intended)
oreo_add_test(test_determinism tests/test_determinism/test_determinism.cpp)

# Integration (end-to-end workflows: sketch->extrude->fillet->boolean, STEP round-trip, feature tree replay)
# Uses legacy singleton APIs (oreo_init, oreo_sketch_*, oreo_make_*, oreo_fillet, etc.).
# Gated so the OREO_ENABLE_LEGACY_API=OFF build configures cleanly — the
# no-legacy equivalent lives in test_doc_identity_nolegacy.
if(OREO_ENABLE_LEGACY_API)
    oreo_add_test(test_integration tests/test_integration/test_integration.cpp)
endif()

# Document-identity end-to-end regression (Part 1 of the April 2026 audit).
# Verifies that a 64-bit documentId reaches every layer unchanged on both
# Windows MSVC (where `long` is 32-bit) and Linux GCC.
#
# The test currently relies on oreo_face_name / oreo_serialize /
# oreo_deserialize / oreo_free_buffer, which are gated behind
# OREO_ENABLE_LEGACY_API (they take explicit handles but route diagnostics
# through the process-global internalDefaultCtx). Part 2 of the audit
# introduces ctx-aware wrappers; until then, this test is gated on the
# legacy surface the same way test_foundation_battle2 is.
if(OREO_ENABLE_LEGACY_API)
    oreo_add_test(test_doc_identity tests/test_integration/test_doc_identity.cpp)
endif()

# No-legacy doc-identity suite (Phase 6). Uses only ctx-aware APIs
# (oreo_ctx_* + oreo_*_shape_id) so it runs on both OREO_ENABLE_LEGACY_API
# configurations — critically, it is the acceptance gate for the OFF
# build where none of the singleton-context APIs exist.
oreo_add_test(test_doc_identity_nolegacy
              tests/test_integration/test_doc_identity_nolegacy.cpp)

# Acceptance suite for the ctx-aware sketch + feature-edit C APIs.
# Uses no legacy entry points so it runs in both build configurations.
oreo_add_test(test_ctx_apis tests/test_integration/test_ctx_apis.cpp)

# STEP-import identity regression — every imported face/edge/vertex
# must carry a v2 ShapeIdentity bound to the importing context's
# documentId. Closes the auditor's "imported element names lack v2
# identity" gap. No legacy gate; runs in both build configurations.
oreo_add_test(test_step_identity tests/test_integration/test_step_identity.cpp)

# Concurrency: parallel contexts must not share state — Phase D3.
oreo_add_test(test_concurrency tests/test_integration/test_concurrency.cpp)

# Deterministic golden replay — server/client parity (Phase D4).
oreo_add_test(test_replay_golden tests/test_integration/test_replay_golden.cpp)

# Transaction API: per-type setters, broken-feature query, move,
# replace-reference (Phases D2 + D5 + D8).
oreo_add_test(test_transaction_api tests/test_integration/test_transaction_api.cpp)

# Pure-C consumer smoke test. Builds as C99 against ONLY the public
# header — proves the C ABI is C-clean (no leaked C++ keywords) and
# that a downstream C consumer can drive the ctx-aware API end-to-end.
add_executable(test_capi_consumer tests/test_capi_consumer/test_capi_consumer.c)
set_target_properties(test_capi_consumer PROPERTIES
    LINKER_LANGUAGE C
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
)
# Link against the SHARED public DLL (oreo-kernel), NOT the internal
# static archive — this is exactly the linkage path a real C consumer
# would use after install().
target_link_libraries(test_capi_consumer PRIVATE oreo-kernel)
target_include_directories(test_capi_consumer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
add_test(NAME test_capi_consumer COMMAND test_capi_consumer)

# Smoke-fuzz (random-input robustness for deserialize / schema migrate / sketch / STEP)
oreo_add_test(test_fuzz tests/test_fuzz/test_fuzz.cpp)

# Perf regression (generous time budgets — catches 10x regressions, not microbenchmark drift)
# Uses legacy oreo_make_box / oreo_fillet etc. Gate until it's ported to ctx APIs.
# Also skipped under sanitizers — ASan/UBSan/TSan slow every load/store
# by 2-10x, which busts the microsecond-granularity wall-clock budgets
# these tests assert against. Running them under sanitizers produces
# false-positive perf regressions that drown out real ones. Perf
# regression detection belongs in the non-sanitized gcc-release cell.
if(OREO_ENABLE_LEGACY_API AND OREO_ENABLE_SANITIZER STREQUAL "none")
    oreo_add_test(test_perf tests/test_perf/test_perf.cpp)
endif()

# Identity benchmark (Phase 7). Measures name hash/compare/insert/serialize
# throughput on 10k-op workloads so CI can diff against a baseline. No
# legacy API dependency — runs in both build configurations.
oreo_add_test(bench_identity tests/test_perf/bench_identity.cpp)

# Audit-fix regressions (5 issues from external contributor review: OcctStaticGuard
# de-dup, SchemaRegistry freeze enforcement, ConfigLoader uint64 docId, TagAllocator
# low-32 collision detection, DiagnosticCollector hard cap)
oreo_add_test(test_foundation_audit_fixes tests/test_foundation/test_foundation_audit_fixes.cpp)

# Tessellation / Meshing
oreo_add_test(test_mesh tests/test_mesh/test_mesh.cpp)
oreo_add_test(test_mesh_export tests/test_mesh/test_mesh_export.cpp)

# Manual-run smoke tool: writes a cube to cube.glb. Not registered as a ctest —
# use it for drag-and-drop validation in three.js, Blender, Windows 3D Viewer,
# etc. Invoke with:  ./Release/glb_smoke [optional/out/path.glb]
#
# Links to the internal static archive because the smoke tool calls
# C++ internals (oreo::tessellate, oreo::makeBox) rather than the flat
# C ABI.
add_executable(glb_smoke tests/test_mesh/glb_smoke_main.cpp)
target_link_libraries(glb_smoke PRIVATE oreo-kernel-internal)
target_include_directories(glb_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OpenCASCADE_INCLUDE_DIR}
)

oreo_add_test(test_foundation_production tests/test_foundation/test_foundation_production.cpp)

# Foundation battle tests (adversarial: all 4 layers, threading, overflow, corruption, C API boundary)
oreo_add_test(test_foundation_battle tests/test_foundation/test_foundation_battle.cpp)

# test_foundation_battle2 exercises the legacy singleton C API directly, so it
# only builds when that API is enabled.
if(OREO_ENABLE_LEGACY_API)
    oreo_add_test(test_foundation_battle2 tests/test_foundation/test_foundation_battle_part2.cpp)
endif()

# ── Install / export targets ─────────────────────────────────────────────────
#
# Production install layout:
#   <prefix>/include/oreo_kernel.h
#   <prefix>/lib/oreo-kernel.lib            (Windows import lib)
#   <prefix>/bin/oreo-kernel.dll            (Windows runtime)
#   <prefix>/lib/oreo-kernel.so             (Linux)
#   <prefix>/lib/cmake/OreoKernel/OreoKernelConfig.cmake
#   <prefix>/lib/cmake/OreoKernel/OreoKernelConfigVersion.cmake
#   <prefix>/lib/cmake/OreoKernel/OreoKernelTargets.cmake
#
# Downstream consumers (see sample/ for a worked example):
#   find_package(OreoKernel CONFIG REQUIRED)
#   target_link_libraries(myapp PRIVATE OreoKernel::oreo-kernel)
#
# The internal STATIC archive is intentionally NOT installed — it is a
# build-time artefact of the test split, not part of the public ABI.

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS oreo-kernel
    EXPORT  OreoKernelTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(FILES include/oreo_kernel.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT OreoKernelTargets
    FILE        OreoKernelTargets.cmake
    NAMESPACE   OreoKernel::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OreoKernel
)

# Versioned config so find_package(OreoKernel 0.1.0 EXACT) works.
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/OreoKernelConfigVersion.cmake"
    VERSION       ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion
)

# Resolve consumer-side third-party dependencies. The shared lib's
# PUBLIC link interface needs OCCT / Boost / etc. visible to the
# downstream find_package() call.
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/OreoKernelConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/OreoKernelConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OreoKernel
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/OreoKernelConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/OreoKernelConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OreoKernel
)

# ── Fuzzers ──────────────────────────────────────────────────────────────────
#
# libFuzzer harnesses for the highest-risk parsers. Compiled only when
# OREO_BUILD_FUZZERS=ON and a Clang toolchain is selected. Each harness
# is a single .cc file in fuzzers/ that defines LLVMFuzzerTestOneInput
# and links the static internal archive directly.
#
# Run a harness:
#   build_fuzz/fuzzers/fuzz_deserialize -max_total_time=60 corpora/fuzz_deserialize/
#
# The CI workflow in .github/workflows/ci.yml drives a 60s smoke run for
# each harness on every push to main.
if(OREO_BUILD_FUZZERS)
    set(_oreo_fuzzers
        fuzz_deserialize
        fuzz_step_import
        fuzz_feature_tree_json
    )
    foreach(_h IN LISTS _oreo_fuzzers)
        add_executable(${_h} fuzzers/${_h}.cc)
        target_link_libraries(${_h} PRIVATE oreo-kernel-internal)
        target_compile_options(${_h} PRIVATE -fsanitize=fuzzer)
        target_link_options(${_h} PRIVATE -fsanitize=fuzzer)
        target_include_directories(${_h} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/src/naming/freecad
            ${OpenCASCADE_INCLUDE_DIR}
        )
        set_target_properties(${_h} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fuzzers"
        )
    endforeach()
endif()
