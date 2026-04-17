// SPDX-License-Identifier: LGPL-2.1-or-later

// fuzz_deserialize.cc — libFuzzer harness for oreo::deserialize.
//
// Compiled only when the build was configured with -DOREO_BUILD_FUZZERS=ON
// AND a Clang toolchain is selected. The harness must NEVER abort or
// throw — it asserts only that the kernel handles arbitrary byte input
// gracefully (returns failure + diagnostic, never UB).
//
// Build (Linux/Clang):
//   cmake -B build_fuzz -DOREO_BUILD_FUZZERS=ON \
//         -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
//         -DCMAKE_BUILD_TYPE=RelWithDebInfo
//   cmake --build build_fuzz --target fuzz_deserialize
// Run:
//   build_fuzz/fuzzers/fuzz_deserialize -max_total_time=60 corpora/serialize/

#include "core/kernel_context.h"
#include "io/oreo_serialize.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Each invocation gets a fresh context so the diagnostic collector
    // and tag allocator state cannot leak between fuzz iterations.
    auto ctx = oreo::KernelContext::create();
    auto result = oreo::deserialize(*ctx, data, size);
    // Whatever happened, the kernel must not crash. result.ok() may be
    // either true or false — we don't care which. The interesting
    // invariant is "no SIGABRT, no UB, no leaked exception".
    (void)result;
    return 0;
}
