# SPDX-License-Identifier: LGPL-2.1-or-later
#
# oreo-kernel-dev — reproducible build environment for CI + contributors.
#
# Layers in build order (most stable on top, most volatile on bottom)
# so that bumping a single dep doesn't invalidate the whole image cache:
#
#   1. Base OS (ubuntu:24.04) — locked to the LTS that ships OCCT 7.7+
#      with a working CMake config file (Ubuntu 22.04's OCCT 7.5 does not).
#   2. apt build toolchain (gcc/clang/cmake/ninja/ripgrep/lld).
#   3. apt third-party deps (OCCT, Boost, Qt6, Eigen, nlohmann-json, GTest).
#   4. Environment hints (CMAKE_PREFIX_PATH, OpenCASCADE_DIR) so a bare
#      `cmake -S . -B build` inside the container Just Works.
#
# Build locally:
#   docker build -t oreo-kernel-dev -f docker/Dockerfile .
#
# CI uses the published image at ghcr.io/abhinavvv13/oreo-kernel-dev:latest.
# Push pipeline: .github/workflows/dev-image.yml.

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Toolchain. lld speeds up the link step a lot on Clang+sanitizer builds.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        ripgrep \
        clang \
        lld \
        git \
        ca-certificates \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

# Third-party C/C++ deps consumed by oreo-kernel.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libocct-foundation-dev \
        libocct-modeling-algorithms-dev \
        libocct-modeling-data-dev \
        libocct-ocaf-dev \
        libocct-data-exchange-dev \
        libocct-visualization-dev \
        libgtest-dev \
        libeigen3-dev \
        nlohmann-json3-dev \
        libboost-all-dev \
        qt6-base-dev \
    && rm -rf /var/lib/apt/lists/*

# CMake hints. OCCT's CMake config lives on the multiarch path, which
# CMake doesn't search by default — exporting CMAKE_PREFIX_PATH +
# OpenCASCADE_DIR here means callers don't have to pass them per-job.
ENV CMAKE_PREFIX_PATH=/usr/lib/x86_64-linux-gnu/cmake \
    OpenCASCADE_DIR=/usr/lib/x86_64-linux-gnu/cmake/opencascade

# Where the GitHub Actions checkout lands.
WORKDIR /workspace

# Sanity: keep this label up to date when adding a major dep so the
# image-bump checklist is obvious from `docker inspect`.
LABEL org.opencontainers.image.title="oreo-kernel-dev" \
      org.opencontainers.image.description="OCCT 7.7 + Boost + Qt6 + Eigen + nlohmann-json + GTest + Clang/GCC, ready to build oreo-kernel." \
      org.opencontainers.image.source="https://github.com/ABHINAVVV13/oreo-kernel" \
      org.opencontainers.image.licenses="LGPL-2.1-or-later"
