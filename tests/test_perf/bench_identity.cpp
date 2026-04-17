// SPDX-License-Identifier: LGPL-2.1-or-later

// bench_identity.cpp — Phase 7 benchmark harness for identity hot paths.
//
// Measures four workloads on 10k-op shapes:
//   1. ShapeIdentity hash (for unordered_map keys)
//   2. MappedName compare (for std::map<MappedName, IndexedName> inner nodes)
//   3. ElementMap insert (setElementName)
//   4. ElementMap::serialize
//
// The goal is to detect *catastrophic* regressions from the v1→v2 size
// increase (v2 names are ~1.9x longer than v1 worst-case — see
// docs/identity-model.md §4.4). The advisor set a 20% regression gate,
// but this harness just prints numbers — a CI job is expected to diff
// them against a recorded baseline. Keeping the harness test-framework
// shaped so it participates in the normal ctest run.

#include "core/shape_identity.h"
#include "naming/element_map.h"
#include "naming/mapped_name.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <vector>

using oreo::ShapeIdentity;
using oreo::MappedName;
using oreo::IndexedName;
using oreo::ElementMap;

namespace {

constexpr int kWorkloadSize = 10'000;

double elapsedMs(std::chrono::steady_clock::time_point start) {
    auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

std::vector<ShapeIdentity> generateIdentities(int n, std::uint64_t docId) {
    std::vector<ShapeIdentity> out;
    out.reserve(n);
    for (int i = 1; i <= n; ++i) {
        out.push_back(ShapeIdentity{docId, static_cast<std::uint64_t>(i)});
    }
    return out;
}

std::vector<MappedName> generateNames(const std::vector<ShapeIdentity>& ids,
                                      int hopsPerName) {
    std::vector<MappedName> out;
    out.reserve(ids.size());
    for (auto id : ids) {
        MappedName n("Face" + std::to_string(id.counter));
        for (int hop = 0; hop < hopsPerName; ++hop) {
            n.appendShapeIdentity(id);
            n.appendPostfix(";:M");
        }
        out.push_back(std::move(n));
    }
    return out;
}

}  // namespace

// ─── 1. ShapeIdentity hashing ───────────────────────────────────────

TEST(BenchIdentity, HashThroughput) {
    auto ids = generateIdentities(kWorkloadSize, 0xDEADBEEFCAFEBABEull);
    std::hash<ShapeIdentity> h;
    auto start = std::chrono::steady_clock::now();
    std::size_t acc = 0;
    for (const auto& id : ids) {
        acc ^= h(id);
    }
    const double ms = elapsedMs(start);
    std::fprintf(stderr,
                 "[BenchIdentity.HashThroughput] %d hashes in %.3f ms "
                 "(%.1f ns/hash, acc=0x%zx)\n",
                 kWorkloadSize, ms, ms * 1'000'000.0 / kWorkloadSize, acc);
    SUCCEED();
}

// ─── 2. MappedName compare ──────────────────────────────────────────

TEST(BenchIdentity, MappedNameCompare) {
    auto ids = generateIdentities(kWorkloadSize, 0xDEADBEEFCAFEBABEull);
    auto names = generateNames(ids, /*hops=*/3);
    auto start = std::chrono::steady_clock::now();
    int lessCount = 0;
    for (size_t i = 1; i < names.size(); ++i) {
        if (names[i - 1] < names[i]) ++lessCount;
    }
    const double ms = elapsedMs(start);
    std::fprintf(stderr,
                 "[BenchIdentity.MappedNameCompare] %zu compares in %.3f ms "
                 "(%.1f ns/compare, lessCount=%d)\n",
                 names.size() - 1, ms,
                 ms * 1'000'000.0 / (names.size() - 1), lessCount);
    SUCCEED();
}

// ─── 3. ElementMap insert ───────────────────────────────────────────

TEST(BenchIdentity, ElementMapInsert) {
    auto ids = generateIdentities(kWorkloadSize, 0xDEADBEEFCAFEBABEull);
    auto names = generateNames(ids, /*hops=*/1);
    ElementMap map;
    auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < names.size(); ++i) {
        map.setElementName(IndexedName("Face", static_cast<int>(i + 1)),
                           names[i], /*tag=*/0);
    }
    const double ms = elapsedMs(start);
    std::fprintf(stderr,
                 "[BenchIdentity.ElementMapInsert] %zu inserts in %.3f ms "
                 "(%.1f ns/insert)\n",
                 names.size(), ms,
                 ms * 1'000'000.0 / names.size());
    SUCCEED();
}

// ─── 4. ElementMap serialize ────────────────────────────────────────

TEST(BenchIdentity, ElementMapSerialize) {
    auto ids = generateIdentities(kWorkloadSize, 0xDEADBEEFCAFEBABEull);
    auto names = generateNames(ids, /*hops=*/1);
    ElementMap map;
    for (size_t i = 0; i < names.size(); ++i) {
        map.setElementName(IndexedName("Face", static_cast<int>(i + 1)),
                           names[i], /*tag=*/0);
    }
    auto start = std::chrono::steady_clock::now();
    auto buf = map.serialize();
    const double ms = elapsedMs(start);
    std::fprintf(stderr,
                 "[BenchIdentity.ElementMapSerialize] %zu entries → %zu bytes "
                 "in %.3f ms (%.1f ns/entry)\n",
                 names.size(), buf.size(), ms,
                 ms * 1'000'000.0 / names.size());
    SUCCEED();
}
