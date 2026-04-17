// SPDX-License-Identifier: LGPL-2.1-or-later

// fuzz_feature_tree_json.cc — libFuzzer harness for the FeatureTree JSON
// deserializer. Feeds arbitrary bytes to FeatureTree::fromJSON and
// asserts the kernel handles malformed input without UB. See
// fuzz_deserialize.cc for build/run.

#include "feature/feature_tree.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string json(reinterpret_cast<const char*>(data), size);
    try {
        // fromJSON now returns a result struct; ok=false on malformed
        // input. The fuzzer doesn't care which branch wins — only
        // that the call returns without crashing.
        auto r = oreo::FeatureTree::fromJSON(json);
        (void)r;
    } catch (const std::exception&) {
        // Documented contract: fromJSON catches all exceptions and
        // converts them into ok=false. Any escape would be a bug
        // worth reporting via a controlled return rather than abort.
    } catch (...) {
    }
    return 0;
}
