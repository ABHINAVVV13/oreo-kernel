cmake_minimum_required(VERSION 3.20)
project(oreo-kernel VERSION 0.1.0 LANGUAGES CXX)

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

# ── oreo-kernel shared library ────────────────────────────────────────────────

# Collect all source files (will be populated as we build out the kernel)
file(GLOB_RECURSE OREO_SOURCES
    src/core/*.cpp
    src/naming/*.cpp
    src/sketch/*.cpp
    src/geometry/*.cpp
    src/query/*.cpp
    src/io/*.cpp
    src/capi/*.cpp
    src/feature/*.cpp
    src/mesh/*.cpp
)

# Export all symbols from DLL (MSVC) — needed for tests to link C++ internals
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

add_library(oreo-kernel SHARED ${OREO_SOURCES})

target_include_directories(oreo-kernel
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/naming/freecad
        ${OpenCASCADE_INCLUDE_DIR}
)

target_link_libraries(oreo-kernel
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
        # OCCT STEP I/O
        TKDESTEP
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

target_compile_definitions(oreo-kernel PRIVATE OREO_KERNEL_EXPORTS)

# Required for M_PI on Windows (OCCT headers need it)
target_compile_definitions(oreo-kernel PUBLIC _USE_MATH_DEFINES)

# Propagate CMake project version to schema.h so KERNEL_VERSION stays in sync.
target_compile_definitions(oreo-kernel PUBLIC
    OREO_KERNEL_VERSION_STRING="${PROJECT_VERSION}"
)

# Propagate legacy-API flag to compilation units (both library and consumers,
# since it gates public declarations in include/oreo_kernel.h).
if(OREO_ENABLE_LEGACY_API)
    target_compile_definitions(oreo-kernel PUBLIC OREO_ENABLE_LEGACY_API)
endif()

# Export macro for DLL
if(WIN32)
    target_compile_definitions(oreo-kernel
        PUBLIC
            $<BUILD_INTERFACE:OREO_BUILDING_DLL>
    )
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────

enable_testing()

# Test helper: creates a test executable linked to oreo-kernel + gtest
function(oreo_add_test TEST_NAME TEST_SOURCE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_link_libraries(${TEST_NAME} PRIVATE
        oreo-kernel
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

# Boolean stress tests
oreo_add_test(test_boolean_stress tests/test_boolean/test_boolean_stress.cpp)

# STEP round-trip
oreo_add_test(test_step_roundtrip tests/test_step/test_step_roundtrip.cpp)

# Serialize round-trip
oreo_add_test(test_serialize tests/test_serialize/test_serialize.cpp)

# Robustness (crash/malformed input)
oreo_add_test(test_robustness tests/test_robustness/test_robustness.cpp)

# Feature tree (parametric replay, parameter changes, broken refs, serialization)
oreo_add_test(test_feature_tree tests/test_feature/test_feature_tree.cpp)

# Foundation (context, diagnostics, units, schema, validation, determinism)
oreo_add_test(test_foundation tests/test_foundation/test_foundation.cpp)

# Foundation hardened (adversarial tests: NaN, Inf, fail-closed, composable diagnostics)
oreo_add_test(test_foundation_hardened tests/test_foundation/test_foundation_hardened.cpp)

# Foundation modules (standalone: cancellation, feature flags, localization, RNG, profile, arena, assertion)
oreo_add_test(test_foundation_modules tests/test_foundation/test_foundation_modules.cpp)

# Foundation observability (config loader, logging sink, resource quotas, diagnostic metadata)
oreo_add_test(test_foundation_obs tests/test_foundation/test_foundation_obs.cpp)

# Foundation new features (KernelContext clone/merge/safeReset, cancellation, progress, combinators, validators, schema introspection)
oreo_add_test(test_foundation_new_features tests/test_foundation/test_foundation_new_features.cpp)

# Tessellation / Meshing
oreo_add_test(test_mesh tests/test_mesh/test_mesh.cpp)
oreo_add_test(test_mesh_export tests/test_mesh/test_mesh_export.cpp)

# Manual-run smoke tool: writes a cube to cube.glb. Not registered as a ctest —
# use it for drag-and-drop validation in three.js, Blender, Windows 3D Viewer,
# etc. Invoke with:  ./Release/glb_smoke [optional/out/path.glb]
add_executable(glb_smoke tests/test_mesh/glb_smoke_main.cpp)
target_link_libraries(glb_smoke PRIVATE oreo-kernel)
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
