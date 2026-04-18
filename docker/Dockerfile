# SPDX-License-Identifier: LGPL-2.1-or-later
#
# oreo-kernel — multi-stage Dockerfile.
#
# This is the single source of truth for both the development and the
# release images. Four stages, each with a clear contract:
#
#   ┌─ occt-builder ──────────────────────────────────────────────────┐
#   │  Builds OCCT from source, pinned to a git tag. The rest of      │
#   │  the pipeline depends on the install tree at /opt/occt.         │
#   │  Slow (45-60 min cold, seconds when cached by BuildKit).        │
#   └─────────────────────────────────────────────────────────────────┘
#                                 │
#                                 ▼
#   ┌─ dev ───────────────────────────────────────────────────────────┐
#   │  Development + CI environment. Toolchain (gcc, clang, cmake,    │
#   │  ninja, sanitizers), all third-party deps, the source-built     │
#   │  OCCT from above, but NOT oreo-kernel itself. CI mounts the     │
#   │  working tree and drives cmake from here. Contributors attach   │
#   │  VS Code Dev Containers to this image.                          │
#   │  Published as ghcr.io/<owner>/oreo-kernel-dev:latest            │
#   └─────────────────────────────────────────────────────────────────┘
#                                 │
#                                 ▼
#   ┌─ builder ───────────────────────────────────────────────────────┐
#   │  Intermediate stage: takes `dev`, copies the oreo-kernel source │
#   │  in, builds and installs it into /opt/oreo-kernel. Only exists  │
#   │  to produce the release image below; never published.           │
#   └─────────────────────────────────────────────────────────────────┘
#                                 │
#                                 ▼
#   ┌─ release ───────────────────────────────────────────────────────┐
#   │  Consumer-facing image. Base = dev (so consumers get the        │
#   │  toolchain for compiling their own code), plus the oreo-kernel  │
#   │  install tree copied in from `builder`. Consumers:              │
#   │      FROM ghcr.io/<owner>/oreo-kernel:v0.9.0                    │
#   │  and get oreo_kernel.h + liboreo-kernel.so + OreoKernelConfig   │
#   │  + all transitive dependencies, ready to find_package().        │
#   │  Published as ghcr.io/<owner>/oreo-kernel:<version>             │
#   └─────────────────────────────────────────────────────────────────┘
#
# Rationale for source-building OCCT instead of apt-installing it:
#   Ubuntu 24.04's apt-shipped OCCT is 7.6.3, which is one major
#   version behind what vcpkg installs on Windows (7.9.3). The two
#   produce byte-level-different serialization output — the project's
#   determinism tests (DeterminismGolden.CanonicalOpSequenceHash etc.)
#   explicitly catch cross-platform drift and refuse to pass unless
#   every platform computes the same hash. Pinning OCCT to a single
#   source-built version on every platform is the only honest fix.
#
#   Cost: ~45 min of extra build time on the first `dev-image.yml`
#   run. After that, BuildKit caches the `occt-builder` layer and
#   subsequent dev-image.yml runs are as fast as before — the layer
#   is only re-executed when OCCT_TAG or the OCCT build flags change.

ARG UBUNTU_VERSION=24.04
ARG OCCT_TAG=V7_9_3
ARG OCCT_PREFIX=/opt/occt
ARG OREO_PREFIX=/opt/oreo-kernel


# ════════════════════════════════════════════════════════════════════
# Stage 1: occt-builder — build OCCT from source, pinned
# ════════════════════════════════════════════════════════════════════
FROM ubuntu:${UBUNTU_VERSION} AS occt-builder

ARG OCCT_TAG
ARG OCCT_PREFIX

ENV DEBIAN_FRONTEND=noninteractive

# Build toolchain + OCCT's own build-time deps. Kept separate from the
# dev-stage toolchain install so the builder layer doesn't invalidate
# when we tweak dev-stage packages. Leaner install than the dev stage:
# we don't need clang, ripgrep, or test libs to compile OCCT.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        git \
        ca-certificates \
        pkg-config \
        libtbb-dev \
        libfreetype-dev \
        libfreeimage-dev \
        libfontconfig1-dev \
        libgl-dev \
        libglu1-mesa-dev \
        libxmu-dev \
        libxi-dev \
        libx11-dev \
        libxext-dev \
    && rm -rf /var/lib/apt/lists/*

# Shallow clone the pinned tag. Tag V7_9_3 corresponds to OCCT 7.9.3
# which is what Windows vcpkg installs as of April 2026. Bumping this
# requires updating vcpkg.json on Windows in lockstep; see
# docs/deployment.md §"Updating OCCT" for the full procedure.
WORKDIR /src
RUN git clone --depth 1 --branch ${OCCT_TAG} \
        https://github.com/Open-Cascade-SAS/OCCT.git occt

# CMake flags chosen to match what vcpkg's opencascade port produces
# for x64-windows, so determinism tests pass. Key choices:
#   * BUILD_LIBRARY_TYPE=Shared — consumers link shared, same as vcpkg
#   * USE_TBB=ON — parallel BOP / meshing; required for matching output
#   * USE_FREETYPE/FREEIMAGE=ON — visualization deps that oreo-kernel's
#     link interface references (even though we don't render)
#   * USE_VTK/FFMPEG/RAPIDJSON=OFF — unused and just bloat image size
#   * BUILD_MODULE_Draw=OFF — the Tcl test harness, ~50 MB, unused
#   * BUILD_DOC_Overview=OFF — avoid pulling a doxygen toolchain
#   * BUILD_WITH_DEBUG=OFF — production flag; asserts enabled would
#     make shapes take the slow path and diverge from Windows.
RUN cmake -S /src/occt -B /src/occt-build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=${OCCT_PREFIX} \
        -DBUILD_LIBRARY_TYPE=Shared \
        -DBUILD_WITH_DEBUG=OFF \
        -DBUILD_MODULE_Draw=OFF \
        -DBUILD_DOC_Overview=OFF \
        -DUSE_TBB=ON \
        -DUSE_FREETYPE=ON \
        -DUSE_FREEIMAGE=ON \
        -DUSE_OPENGL=ON \
        -DUSE_VTK=OFF \
        -DUSE_FFMPEG=OFF \
        -DUSE_RAPIDJSON=OFF

RUN cmake --build /src/occt-build --parallel
RUN cmake --install /src/occt-build


# ════════════════════════════════════════════════════════════════════
# Stage 2: dev — development + CI environment
# ════════════════════════════════════════════════════════════════════
FROM ubuntu:${UBUNTU_VERSION} AS dev

ARG OCCT_PREFIX

ENV DEBIAN_FRONTEND=noninteractive

# Toolchain.
#   gcc/g++   — primary GCC compiler (13.x on noble)
#   clang/clang++ + libclang-rt-18-dev — Clang 18 with ASan/UBSan
#                runtime archives; the `clang` metapackage does NOT
#                pull libclang_rt.*-dev, so sanitizer link fails
#                without it.
#   lld       — faster link step, especially for Clang + sanitizers.
#   ninja     — faster than make, deterministic parallelism.
#   ripgrep   — used by ci/grep_gate.sh.
#   gdb       — for VS Code Dev Containers breakpoint debugging.
#   pkg-config + git + ca-certificates — standard build hygiene.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        ripgrep \
        clang \
        libclang-rt-18-dev \
        llvm-18 \
        lld \
        gdb \
        git \
        ca-certificates \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

# ASan/UBSan/LSan look up `llvm-symbolizer` in PATH by default to
# turn module+offset into source file:line. Ubuntu installs the
# LLVM 18 binary with a version suffix; symlink to the canonical
# name so the sanitizer runtimes find it without any environment
# variable tweaks.
RUN ln -sf /usr/bin/llvm-symbolizer-18 /usr/bin/llvm-symbolizer

# Runtime + build-interface deps. OCCT is copied in below; these are
# either deps OCCT links against (and so appear on its exported
# link line, requiring the -dev symlinks), or direct oreo-kernel deps.
#
#   libtbb-dev / libfreetype-dev / libfreeimage-dev / libfontconfig1-dev
#   libgl-dev / libglu1-mesa-dev / libxmu-dev / libxi-dev / libx11-dev /
#   libxext-dev
#       — OCCT link interface's unversioned .so symlinks.
#
#   libgtest-dev        — GTest for tests/.
#   libeigen3-dev       — Eigen for PlaneGCS.
#   nlohmann-json3-dev  — JSON for feature-tree serialization.
#   libboost-all-dev    — Boost graph + iostreams + math + etc.
#   qt6-base-dev        — Qt Core (headless) for element-map structures.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libtbb-dev \
        libfreetype-dev \
        libfreeimage-dev \
        libfontconfig1-dev \
        libgl-dev \
        libglu1-mesa-dev \
        libxmu-dev \
        libxi-dev \
        libx11-dev \
        libxext-dev \
        libgtest-dev \
        libeigen3-dev \
        nlohmann-json3-dev \
        libboost-all-dev \
        qt6-base-dev \
    && rm -rf /var/lib/apt/lists/*

# Copy the source-built OCCT in. This is the single most important
# reason this Dockerfile exists: Linux CI now uses the exact same
# OCCT bits as Windows vcpkg.
COPY --from=occt-builder ${OCCT_PREFIX} ${OCCT_PREFIX}

# Tell CMake + the runtime loader where to find OCCT. The install
# prefix above lays the CMake config at <prefix>/lib/cmake/opencascade.
ENV CMAKE_PREFIX_PATH=${OCCT_PREFIX}/lib/cmake \
    OpenCASCADE_DIR=${OCCT_PREFIX}/lib/cmake/opencascade \
    LD_LIBRARY_PATH=${OCCT_PREFIX}/lib

# Where the working tree lands during CI runs and when attached to
# a VS Code Dev Container.
WORKDIR /workspace

LABEL org.opencontainers.image.title="oreo-kernel-dev" \
      org.opencontainers.image.description="oreo-kernel development environment: OCCT 7.9.3 (source-built, pinned) + Boost + Qt6 + Eigen + nlohmann-json + GTest + GCC/Clang + sanitizers + ninja + gdb. Matches Windows vcpkg byte-for-byte." \
      org.opencontainers.image.source="https://github.com/ABHINAVVV13/oreo-kernel" \
      org.opencontainers.image.licenses="LGPL-2.1-or-later"


# ════════════════════════════════════════════════════════════════════
# Stage 3: builder — compile + install oreo-kernel (intermediate)
# ════════════════════════════════════════════════════════════════════
FROM dev AS builder

ARG OREO_PREFIX

COPY . /src/oreo-kernel
WORKDIR /src/oreo-kernel

RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=${OREO_PREFIX} \
        -DOREO_ENABLE_LEGACY_API=ON \
    && cmake --build build --parallel \
    && cmake --install build


# ════════════════════════════════════════════════════════════════════
# Stage 4: release — consumer-facing image
# ════════════════════════════════════════════════════════════════════
FROM dev AS release

ARG OREO_PREFIX

# Copy the installed oreo-kernel tree (headers + .so + CMake config)
# from the builder stage into the release image. The dev base already
# has OCCT + all the transitive runtime deps, so consumers who FROM
# this image get a fully-provisioned build environment for their own
# code that links against oreo-kernel.
COPY --from=builder ${OREO_PREFIX} ${OREO_PREFIX}

# Point consumers' find_package() + runtime loader at our install tree.
ENV CMAKE_PREFIX_PATH=${OCCT_PREFIX}/lib/cmake:${OREO_PREFIX}/lib/cmake \
    OpenCASCADE_DIR=${OCCT_PREFIX}/lib/cmake/opencascade \
    OreoKernel_DIR=${OREO_PREFIX}/lib/cmake/OreoKernel \
    LD_LIBRARY_PATH=${OCCT_PREFIX}/lib:${OREO_PREFIX}/lib

WORKDIR /workspace

LABEL org.opencontainers.image.title="oreo-kernel" \
      org.opencontainers.image.description="oreo-kernel CAD library with full build environment. FROM this image and `find_package(OreoKernel REQUIRED)` — everything is pre-provisioned." \
      org.opencontainers.image.source="https://github.com/ABHINAVVV13/oreo-kernel" \
      org.opencontainers.image.licenses="LGPL-2.1-or-later"
