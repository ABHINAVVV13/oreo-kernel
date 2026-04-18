# oreo-kernel deployment & developer guide

<!--
SPDX-License-Identifier: LGPL-2.1-or-later

This document is the single source of truth for:
  * how a consumer uses the oreo-kernel library in their own code,
  * how a contributor develops on oreo-kernel itself,
  * how a maintainer cuts a release.

oreo-kernel is a Docker-first project. Every supported path runs
inside a container image we publish. There are no supported
native-Windows / native-Linux / native-macOS build paths. This is a
deliberate trade-off: the determinism test suite asserts byte-
identical serialization across every host, and the only honest way to
guarantee that is to pin every dependency — OCCT in particular — at
the image layer.

If you believe you want a native build, stop and re-read §"Why
Docker-first". The answer has a cost you should understand before
taking it.
-->

## Table of contents

1. [What we ship](#what-we-ship)
2. [For consumers — using oreo-kernel in your code](#for-consumers)
3. [For contributors — developing on oreo-kernel](#for-contributors)
4. [For maintainers — publishing a release](#for-maintainers)
5. [Operational reference](#operational-reference)
6. [Why Docker-first](#why-docker-first)
7. [Troubleshooting](#troubleshooting)

---

## What we ship

Two container images, both published to the GitHub Container Registry
at `ghcr.io/abhinavvv13/` (or the fork owner's namespace, lowercased).

| Image | Purpose | Who pulls it |
|---|---|---|
| `oreo-kernel-dev:latest` | Development + CI environment. Toolchain (gcc 13, clang 18, cmake, ninja, gdb, sanitizer runtimes) + all third-party deps (Boost, Qt6, Eigen, nlohmann-json, GTest) + **OCCT 7.9.3 built from source**. No oreo-kernel source inside. | CI workflows, VS Code Dev Containers, contributors |
| `oreo-kernel:<version>` | Consumer image. Everything in the dev image **plus** the installed oreo-kernel library at `/opt/oreo-kernel` (headers, `liboreo-kernel.so`, CMake config). | Downstream applications |

Both images are built from a single multi-stage
[docker/Dockerfile](../docker/Dockerfile). The stages are layered so
BuildKit reuses the expensive occt-builder layer across every CI run,
every release, and every developer's local build — once OCCT compiles
for a given `OCCT_TAG`, nobody pays that ~45 min cost again.

### Image tag strategy for `oreo-kernel`

| Tag pattern | Meaning | Pushed by |
|---|---|---|
| `:v0.9.0-rc1`, `:0.9.0-rc1` | Exact pinned version (pre-release) | release.yml on `v*.*.*-*` tag |
| `:v0.9.0`, `:0.9.0` | Exact pinned version (stable) | release.yml on `v*.*.*` tag |
| `:0.9` | Latest patch in the 0.9.x series | release.yml, stable only |
| `:latest` | Newest stable release | release.yml, stable only |
| `:<sha>` | Commit-SHA pin, for forensic builds | release.yml, always |

**Pre-release tags never update `:latest` or `:0.9`.** A user pinning
to `:0.9` will not accidentally land on an `rc` build.

---

## For consumers

> **Goal:** you are writing code that *uses* oreo-kernel. You don't
> care how it's built — you want to `#include <oreo_kernel.h>` and
> call the API.

### Quick start — a working example end-to-end

Alice is building a STEP-file validator service. Three files, no
other dependencies she needs to install:

**1. `main.cpp`**

```cpp
// SPDX-License-Identifier: LGPL-2.1-or-later
#include <oreo_kernel.h>
#include <cstdio>

int main() {
    std::printf("oreo-kernel version: %s\n", oreo_kernel_version());
    oreo_init();
    // ... call oreo_step_import, oreo_count_faces, etc. ...
    return 0;
}
```

**2. `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.20)
project(validator CXX)
set(CMAKE_CXX_STANDARD 17)

find_package(OreoKernel REQUIRED)

add_executable(validator main.cpp)
target_link_libraries(validator PRIVATE OreoKernel::oreo-kernel)
```

One `find_package`. CMake resolves the imported target
`OreoKernel::oreo-kernel` and transitively brings in OCCT, Boost, Qt,
Eigen, nlohmann-json — everything `liboreo-kernel.so` was linked
against.

**3. `Dockerfile`**

```dockerfile
# Build stage: uses our image as-is (has compilers + oreo-kernel headers + OCCT)
FROM ghcr.io/abhinavvv13/oreo-kernel:v0.9.0 AS builder
WORKDIR /app
COPY . .
RUN cmake -B build -G Ninja && cmake --build build

# Runtime stage: same image, contains all the shared libs the binary needs
FROM ghcr.io/abhinavvv13/oreo-kernel:v0.9.0
COPY --from=builder /app/build/validator /usr/local/bin/validator
ENTRYPOINT ["/usr/local/bin/validator"]
```

**Build + run:**

```bash
docker build -t validator .
docker run --rm -v "$PWD":/data validator /data/part.step
# → oreo-kernel version: 0.9.0
# → faces=42 edges=126
```

**That's the whole developer experience for a consumer.** No OCCT to
install. No vcpkg. No environment variables. No platform-specific
build flags. Works identically on Windows, macOS, and Linux hosts
that have Docker.

> **Validated:** this exact flow is exercised on every release by
> the release.yml workflow, and during local Dockerfile changes by a
> smoke test against `/tmp/oreo-consumer-test/` that builds a toy
> consumer and confirms `find_package(OreoKernel REQUIRED)` resolves
> cleanly and the binary runs.

### What lives where inside the consumer image

| Path | Contents |
|---|---|
| `/opt/oreo-kernel/include/oreo_kernel.h` | Public C header |
| `/opt/oreo-kernel/lib/liboreo-kernel.so` | Shared library |
| `/opt/oreo-kernel/lib/cmake/OreoKernel/` | `OreoKernelConfig.cmake` + target exports |
| `/opt/occt/lib/libTK*.so.7.9.3` | OCCT 7.9.3 shared libs |
| `/usr/lib/x86_64-linux-gnu/libboost_*`, `libQt6*`, `libeigen3`, `libnlohmann_json` | Third-party shared libs from apt |
| `/usr/bin/gcc-13`, `/usr/bin/clang-18`, `/usr/bin/cmake` | Full toolchain (so consumers can compile their own code) |

Environment variables baked into the image:

```
CMAKE_PREFIX_PATH=/opt/occt/lib/cmake:/opt/oreo-kernel/lib/cmake
OpenCASCADE_DIR=/opt/occt/lib/cmake/opencascade
OreoKernel_DIR=/opt/oreo-kernel/lib/cmake/OreoKernel
LD_LIBRARY_PATH=/opt/occt/lib:/opt/oreo-kernel/lib
```

Downstream `find_package` just works.

### Non-C++ consumers

Python / Node.js / Go / Rust bindings do **not exist today**. If you
want them, each would live in its own repository and consume our
image during its build step:

| Ecosystem | Shape of the binding repo |
|---|---|
| Python | `oreo-kernel-py` — pybind11 module, builds wheels with `cibuildwheel`, bundles `liboreo-kernel.so`; consumers `pip install oreo-kernel` |
| Node.js | `oreo-kernel-node` — N-API binding, prebuilt binaries per platform; consumers `npm install oreo-kernel` |
| Go | `oreo-kernel-go` — cgo wrapper; consumers `go get github.com/.../oreo-kernel-go` |
| Rust | `oreo-kernel-rs` — bindgen wrapper + `cc` build crate; `cargo add oreo-kernel` |

All four of those would use `FROM ghcr.io/abhinavvv13/oreo-kernel-dev:latest`
as their build environment to guarantee the same OCCT 7.9.3 bytes.

---

## For contributors

> **Goal:** you are changing code *inside* oreo-kernel. You want an
> IDE with IntelliSense, breakpoints, a test runner, and the same
> environment CI uses.

### Prerequisites (one-time, ~10 minutes)

| Platform | Install |
|---|---|
| Windows | [Docker Desktop](https://www.docker.com/products/docker-desktop/) + [VS Code](https://code.visualstudio.com/) + the [Dev Containers extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers) |
| macOS | same |
| Linux | Docker Engine + VS Code + Dev Containers extension (`docker` group membership) |

No MSVC. No vcpkg. No OCCT. No Python. Nothing else.

### Getting started

```bash
git clone https://github.com/ABHINAVVV13/oreo-kernel
cd oreo-kernel
code .
```

VS Code shows a banner:

> **Folder contains a Dev Container configuration file. Reopen folder to develop in a container?**

Click **Reopen in Container**. VS Code pulls
`ghcr.io/abhinavvv13/oreo-kernel-dev:latest` (~2.85 GB, one-time) and
restarts attached to a container instance. Subsequent opens reuse the
cached image and take seconds.

From that point the terminal, the compiler, IntelliSense, and the
debugger all run inside the container. CMake Tools auto-configures a
Debug build on first open via the `postCreateCommand` declared in
[.devcontainer/devcontainer.json](../.devcontainer/devcontainer.json).

### Daily inner loop

Edit code. Then one of:

| Action | Command / key |
|---|---|
| Build default target | `Ctrl+Shift+B` (CMake Tools task) |
| Rebuild one target | `cmake --build build --target oreo-kernel` |
| Run all tests | `ctest --test-dir build --output-on-failure` |
| Run one test | `ctest --test-dir build -R test_sketch -V` or `./build/tests/test_sketch` |
| Debug a test | Set breakpoint, open `test_sketch.cpp`, **F5** |
| Switch build config | CMake Tools status bar → pick preset (see `CMakePresets.json`) |

### Available CMake presets

Defined in [CMakePresets.json](../CMakePresets.json). Pick one from
the VS Code CMake Tools status bar, or use the CLI:

```bash
cmake --preset debug        # default for Dev Container auto-configure
cmake --preset release      # gcc Release, matches CI gcc-release
cmake --preset no-legacy    # server-safe, matches CI gcc-no-legacy
cmake --preset asan         # clang + ASan+UBSan, matches CI clang-asan-ubsan
cmake --preset fuzz         # clang + libFuzzer, matches CI clang-fuzz
```

Each preset writes to its own `build_*` directory so they don't
invalidate each other.

### Running the full CI matrix locally

Before opening a PR, a contributor can reproduce every CI cell on
their own machine, bit-for-bit:

```bash
# gcc-release with policy gates (the canonical "is main green" run)
MSYS_NO_PATHCONV=1 docker run --rm -v "$PWD":/w -w /w \
    -e BUILD_DIR=build_ci_gcc -e CC=gcc -e CXX=g++ \
    -e BUILD_TYPE=Release -e LEGACY_API=ON -e SANITIZER=none \
    -e BUILD_FUZZERS=OFF -e RUN_GATES=1 \
    ghcr.io/abhinavvv13/oreo-kernel-dev:latest \
    ci/build.sh

# gcc-no-legacy (server-safe)
MSYS_NO_PATHCONV=1 docker run --rm -v "$PWD":/w -w /w \
    -e BUILD_DIR=build_ci_nolegacy -e CC=gcc -e CXX=g++ \
    -e BUILD_TYPE=Release -e LEGACY_API=OFF -e SANITIZER=none \
    -e BUILD_FUZZERS=OFF -e RUN_GATES=0 \
    ghcr.io/abhinavvv13/oreo-kernel-dev:latest \
    ci/build.sh

# clang-asan-ubsan
MSYS_NO_PATHCONV=1 docker run --rm -v "$PWD":/w -w /w \
    -e BUILD_DIR=build_ci_san -e CC=clang -e CXX=clang++ \
    -e BUILD_TYPE=RelWithDebInfo -e LEGACY_API=ON \
    -e SANITIZER=address+undefined -e BUILD_FUZZERS=OFF -e RUN_GATES=0 \
    ghcr.io/abhinavvv13/oreo-kernel-dev:latest \
    ci/build.sh

# clang-fuzz (60s smoke run per libFuzzer harness)
MSYS_NO_PATHCONV=1 docker run --rm -v "$PWD":/w -w /w \
    -e BUILD_DIR=build_ci_fuzz -e CC=clang -e CXX=clang++ \
    -e BUILD_TYPE=RelWithDebInfo -e LEGACY_API=ON \
    -e SANITIZER=address+undefined -e BUILD_FUZZERS=ON -e RUN_GATES=0 \
    ghcr.io/abhinavvv13/oreo-kernel-dev:latest \
    ci/build.sh
```

> `MSYS_NO_PATHCONV=1` is only needed on Git Bash for Windows — it
> stops MSYS from mangling the container path `/w` into `W:/`. On
> macOS, Linux, or WSL2 it's a harmless no-op.

**Because the Dev Container and CI run the same image, locally-green
is sufficient to predict CI-green.** No "works on my machine"
divergence is possible — the machine is the image.

### Opening a pull request

```bash
git checkout -b feature/new-constraint
# ... edit, build, test inside the container ...
git commit -m "feat(sketch): add tangent-to-circle constraint"
git push -u origin feature/new-constraint
gh pr create
```

GitHub triggers `ci.yml` across the 4-cell matrix. Required to be
green before merge.

### Architecture references

Domain-level documents for deeper dives:

- [architecture.md](architecture.md) — module layout, dependency graph
- [identity-model.md](identity-model.md) — ShapeIdentity, tags, versioning
- [feature-schema.md](feature-schema.md) — feature-tree JSON schema
- [part-studio-model.md](part-studio-model.md) — document + transaction model
- [server-safe-api.md](server-safe-api.md) — the OREO_ENABLE_LEGACY_API=OFF surface
- [error-codes.md](error-codes.md) — C API error codes
- [migration-v1-to-v2.md](migration-v1-to-v2.md) — identity v1 → v2 migration

---

## For maintainers

> **Goal:** you are cutting a release, bumping a dependency, or
> investigating a CI breakage.

### Cutting a release

1. Decide the version. Stable: `0.9.0`. Pre-release: `0.9.0-rc1`.
2. Update version in [CMakeLists.txt](../CMakeLists.txt):
   ```cmake
   project(oreo-kernel VERSION 0.9.0 LANGUAGES CXX C)
   set(OREO_KERNEL_PRE_RELEASE "")   # empty for stable, "rc1" for pre-release
   ```
3. Update [CHANGELOG.md](../CHANGELOG.md) with the release notes.
4. Commit + push on `main`.
5. Tag and push:
   ```bash
   git tag v0.9.0
   git push origin v0.9.0
   ```
6. [.github/workflows/release.yml](../.github/workflows/release.yml) fires:
   - Checks out the tagged commit.
   - Builds the `release` target of `docker/Dockerfile`.
   - Pushes `oreo-kernel:0.9.0`, `oreo-kernel:<sha>`, and (stable only) `:0.9` + `:latest`.
   - Generates a GitHub-native build provenance attestation.
   - Writes a summary to the workflow run.
7. Verify on `https://github.com/ABHINAVVV13/oreo-kernel/pkgs/container/oreo-kernel`.

**Release.yml takes ~10-15 min** because BuildKit reuses the cached
occt-builder and dev layers from the most recent `dev-image.yml` run;
only the `builder` stage (compile oreo-kernel itself) and the
`release` stage copy actually execute.

### Bumping OCCT

OCCT version is pinned in [docker/Dockerfile](../docker/Dockerfile)
via the `OCCT_TAG` build arg (default `V7_9_3`).

1. Update `ARG OCCT_TAG=V7_9_X` in the Dockerfile.
2. Update the three comments in the Dockerfile and [ci.yml](../.github/workflows/ci.yml)
   that mention the version string.
3. Update this file's "What we ship" table.
4. Commit + push. `dev-image.yml` fires because Dockerfile changed.
5. **Wait** for `dev-image.yml` to finish pushing the new `:latest`
   before merging any code change that depends on the bump. If you
   don't, `ci.yml` will run against the stale image.
6. `test_determinism` will fail immediately if the OCCT bump changed
   byte-level output. When it does, regenerate the golden hashes:
   - Locally run the ASan matrix to confirm no new leaks/alignment issues.
   - Update the `kGolden` constants in
     [tests/test_determinism/test_determinism.cpp](../tests/test_determinism/test_determinism.cpp)
     with the new hashes reported by the failing run.
   - Commit the golden update *in the same PR* as the OCCT bump.
7. Re-run ASan locally and add any newly-needed
   [ci/lsan.supp](../ci/lsan.supp) or [ci/ubsan.supp](../ci/ubsan.supp)
   entries for OCCT-internal leaks or alignment reports that shifted
   location.

### Understanding the CI pipeline

Three workflows, each with a distinct cadence and scope:

| Workflow | Triggers | Produces | Typical runtime |
|---|---|---|---|
| [dev-image.yml](../.github/workflows/dev-image.yml) | push to main touching `docker/Dockerfile` or the workflow itself, or manual dispatch | `oreo-kernel-dev:latest`, `:<sha>`, `:buildcache` (BuildKit cache) | ~45-60 min cold, ~10 min warm |
| [ci.yml](../.github/workflows/ci.yml) | every push / PR to main | nothing — passes or fails the branch | 5-10 min |
| [release.yml](../.github/workflows/release.yml) | push of `v*.*.*` tag, or manual dispatch with `tag` input | `oreo-kernel:<version>`, `:<sha>`, and (stable only) `:0.9` + `:latest` | 10-15 min |

The data flow:

```
docker/Dockerfile change
        │
        ▼
  dev-image.yml ────────▶  GHCR: oreo-kernel-dev:latest
                                        │
                            (every ci.yml run pulls this)
                                        │
                                        ▼
  every push/PR  ───────▶  ci.yml ───▶ pass/fail gate
                                        │
  tag v0.9.0 ───────────▶  release.yml ──▶  GHCR: oreo-kernel:v0.9.0, :latest
```

### Investigating a CI failure

1. Reproduce locally using the exact matrix command from §"Running the
   full CI matrix locally". If it doesn't reproduce locally, the image
   layer between CI and your machine has drifted — pull the latest
   `oreo-kernel-dev:latest` and retry.
2. For ASan/UBSan failures: stack traces are symbolized via
   `llvm-symbolizer` (installed in the dev stage) — you'll see file +
   line numbers, not just module offsets.
3. For test_determinism regressions: this test is *correct* by design.
   If it fails, the modeling engine produced different bytes. Either
   OCCT changed, the build flags changed, or there's a real
   non-determinism bug somewhere in oreo-kernel. **Do not update the
   golden without investigating root cause.**
4. For fuzzer crashes: the `ci.yml` workflow uploads `crash-*` files
   as an artifact. Download, drop them in the container, and reproduce
   with `./build/fuzzers/<harness> <reproducer-file>`.

---

## Operational reference

### Repository layout (what belongs where)

```
.
├── .devcontainer/devcontainer.json    VS Code Dev Containers config
├── .dockerignore                       keep host build trees out of image
├── .github/workflows/
│   ├── ci.yml                          per-push matrix gate
│   ├── dev-image.yml                   push oreo-kernel-dev on Dockerfile change
│   └── release.yml                     push oreo-kernel on tag
├── ci/
│   ├── build.sh                        single entrypoint: configure+build+test
│   ├── grep_gate.sh                    identity v1 squeeze policy gate
│   ├── spdx_check.sh                   SPDX coverage gate
│   ├── spdx_add.sh                     developer tool to backfill SPDX headers
│   ├── lsan.supp                       LSan suppressions for OCCT internals
│   └── ubsan.supp                      UBSan suppressions for OCCT alignment
├── docker/Dockerfile                   multi-stage: occt-builder → dev → builder → release
├── docs/                               architecture + model guides (this file)
├── include/oreo_kernel.h               public C API
├── src/                                oreo-kernel implementation
├── tests/                              39 GTest suites
├── fuzzers/                            3 libFuzzer harnesses
├── corpora/                            fuzzer seed inputs
├── CMakeLists.txt                      root CMake config
└── CMakePresets.json                   Debug/Release/ASan/Fuzz presets
```

### Key files, in priority order for a new maintainer

1. [docker/Dockerfile](../docker/Dockerfile) — the operating system. Stages explained inline.
2. [CMakeLists.txt](../CMakeLists.txt) — build logic, sanitizer flags, install tree.
3. [ci/build.sh](../ci/build.sh) — every CI and local reproduction runs this.
4. [.github/workflows/](../.github/workflows/) — automation.
5. [.devcontainer/devcontainer.json](../.devcontainer/devcontainer.json) — contributor entrypoint.

---

## Why Docker-first

### The cost of not pinning the environment

oreo-kernel has a `test_determinism` suite that hashes a canonical
feature-tree build, serializes it, and asserts byte-identical output
against a golden hash. That test exists because CAD kernels that
produce different bytes for the same input are broken — downstream
consumers doing content-hash-based caching, version control of feature
trees, or cross-platform sync will silently corrupt.

OCCT is the single largest determinism surface. The library's Boolean
operations, tessellator, and STEP writer all have version-to-version
changes that alter byte output — sometimes due to bugfixes (e.g.
tolerance handling), sometimes due to refactored internals. Running
Windows on OCCT 7.8 and Linux on OCCT 7.6 produces legitimate-looking
shapes on both platforms that hash to different values.

In an earlier iteration of this project, Linux CI used Ubuntu's
apt-shipped OCCT (7.6.3) and Windows developers used vcpkg (7.9.3).
`test_determinism` correctly flagged the drift on the first CI run
that got far enough to execute it. The lesson: pinning the build
toolchain *and* the dependency versions is a correctness requirement,
not a convenience.

The Dockerfile is how we pin. Every contributor, every CI job, every
release uses a container built from the same `docker/Dockerfile` at
the same `OCCT_TAG`. There is exactly one OCCT on exactly one libc
with exactly one compiler, and that's the only environment we test
against.

### What we give up

- **Native IDE tooling.** Windows MSVC users lose the ability to build
  with MSBuild / Visual Studio directly. The replacement is VS Code
  Dev Containers, which gives essentially the same ergonomics
  (breakpoints, IntelliSense, integrated terminal) at the cost of ~10
  minutes of one-time setup.
- **The option of shipping static .lib files for Windows desktop
  consumers.** oreo-kernel is positioned as a server/container-target
  library; desktop integration would need a separate cross-compile
  effort and is out of scope.
- **Build speed on underpowered machines.** Docker Desktop with the
  WSL2 backend has ~10% file-I/O overhead vs. native NTFS. For
  interactive C++ compile-test loops this is negligible; for churning
  full-matrix rebuilds it's noticeable.

### What we get

- Byte-identical modeling output across every platform.
- No "works on my machine." The machine is the image.
- Contributors onboard in 10 minutes, not 2 hours.
- Consumer integration is a single `FROM` line in a Dockerfile.
- The Dockerfile is auditable: one file, pinned git tag, explicit
  build flags, reproducible from scratch.

---

## Troubleshooting

### "invalid reference format: repository name must be lowercase"

`github.repository_owner` preserves casing. If your GitHub login has
uppercase letters, `ghcr.io/<owner>/...` refs fail to parse. Either
hardcode the lowercase path in the workflow (as done in
[ci.yml](../.github/workflows/ci.yml)) or introduce a prep job that
lowercases the owner and exposes it via job outputs.

### "Permission denied" running ci/*.sh in CI

The `+x` bit must be set in the git index on Linux runners. Git on
Windows does not propagate filesystem exec bits. If you add a new
shell script, run:

```bash
git update-index --chmod=+x ci/your_new_script.sh
```

before committing.

### `cmake` error: "source ... does not match the source used to generate cache"

You built natively on the host into a `build*/` directory that then
got copied into the container by Docker. [.dockerignore](../.dockerignore)
blocks this in fresh builds. If you're hitting it in an existing
clone, `rm -rf build* vcpkg_installed/` before rebuilding.

### `test_determinism` fails

See [§"Bumping OCCT"](#bumping-occt). Do not update the golden
without understanding why the hash changed.

### ASan reports leaks inside `libTKernel.so` / `libTKVCAF.so` / `libTKMesh.so`

These are suppressed in [ci/lsan.supp](../ci/lsan.supp) — OCCT has
process-lifetime static caches it never frees. Only add to the
suppressions file if the leak stack is *entirely* inside OCCT
libraries. A leak stack that passes through oreo-kernel code is a
real bug to fix, not to suppress.

### UBSan reports `alignment:NCollection_TListNode`

Suppressed in [ci/ubsan.supp](../ci/ubsan.supp) — OCCT's
`NCollection_TListNode<T>` downcast is a false positive on x86_64.
Requires `-fsanitize-recover=alignment` in the build (already set in
CMakeLists) so the suppression is actually consulted.

### Fuzzer finds a crash

`ci.yml` uploads `crash-*`, `leak-*`, and `timeout-*` as an artifact
when the clang-fuzz cell fails. Download from the Actions run page,
drop the file in the repo, and reproduce:

```bash
docker run --rm -v "$PWD":/w -w /w ghcr.io/abhinavvv13/oreo-kernel-dev:latest \
    bash -c "cmake -S . -B build_fuzz -G Ninja \
             -DCMAKE_BUILD_TYPE=RelWithDebInfo \
             -DOREO_ENABLE_SANITIZER=address+undefined \
             -DOREO_BUILD_FUZZERS=ON \
             -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
          && cmake --build build_fuzz --parallel \
          && ./build_fuzz/fuzzers/fuzz_deserialize crash-<hash>"
```

Attach the reproducer and the symbolized stack trace to the bug
report.

### "I can't install Docker on my machine"

Then you cannot develop on or consume oreo-kernel. This is the
deliberate trade explained in §"Why Docker-first". Options:

- **Free**: Docker Desktop is free for personal use, small business,
  education, and non-commercial open-source projects.
- **Alternative runtimes**: `podman` works as a drop-in replacement
  for most commands (`alias docker=podman`).
- **Cloud dev env**: GitHub Codespaces picks up
  `.devcontainer/devcontainer.json` and gives you a browser-based
  VS Code attached to the dev image. No local install.

If none of those work for your environment, oreo-kernel is not the
right library for your project.
