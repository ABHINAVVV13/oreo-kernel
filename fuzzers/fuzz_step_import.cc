// SPDX-License-Identifier: LGPL-2.1-or-later

// fuzz_step_import.cc — libFuzzer harness for oreo::importStep.
//
// STEP files are text-based but the kernel must tolerate arbitrary
// bytes without crashing. See fuzz_deserialize.cc for build/run.

#include "core/kernel_context.h"
#include "io/oreo_step.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    auto ctx = oreo::KernelContext::create();
    auto result = oreo::importStep(*ctx, data, size);
    (void)result;
    return 0;
}
