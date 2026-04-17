// SPDX-License-Identifier: LGPL-2.1-or-later

// memory_pool.h — MISSING-1: header-only bump arena.
//
// Arena is a simple growing-block bump allocator intended for short-lived,
// same-lifetime allocations such as naming strings produced during a single
// feature operation. It is deliberately minimal:
//
//   - NOT thread-safe. Use one Arena per operation / per thread.
//   - Only supports trivially-destructible types (no dtor runs).
//   - Memory is freed in bulk via reset() or when the Arena destructs.
//   - Grows by doubling the base block size; large single allocations that
//     exceed the current block size get their own dedicated block.
//
// This module is header-only so that it can be included from any translation
// unit (including other headers) without a link-time dependency.

#ifndef OREO_MEMORY_POOL_H
#define OREO_MEMORY_POOL_H

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace oreo {

class Arena {
    struct Block {
        std::unique_ptr<std::byte[]> data;
        std::size_t size = 0;
        std::size_t used = 0;
    };

    std::vector<Block> blocks_;
    std::size_t blockSize_;
    std::size_t totalAllocated_ = 0;

    // Allocate a fresh block whose usable capacity is at least `atLeast`.
    // Doubles blockSize_ monotonically so repeated allocations don't
    // re-enter the slow path.
    Block& grow_(std::size_t atLeast) {
        const std::size_t want = std::max(blockSize_, atLeast);
        Block b;
        b.data.reset(new std::byte[want]);
        b.size = want;
        b.used = 0;
        blocks_.push_back(std::move(b));
        // Only double the base stride; large one-off blocks don't bloat the
        // default size for subsequent small allocations.
        if (want == blockSize_ && blockSize_ < (std::size_t{1} << 24)) {
            blockSize_ *= 2;
        }
        return blocks_.back();
    }

public:
    explicit Arena(std::size_t blockSize = 4096)
        : blockSize_(blockSize == 0 ? 4096 : blockSize) {}

    Arena(const Arena&)            = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) noexcept        = default;
    Arena& operator=(Arena&&) noexcept = default;

    // Allocate `bytes` with the given alignment. Returns a pointer into an
    // internal block. Throws std::bad_alloc on allocation failure.
    //
    // Callers MUST treat the returned storage as raw bytes until they
    // placement-new a trivially-destructible object into it.
    void* allocate(std::size_t bytes, std::size_t align = alignof(std::max_align_t)) {
        if (bytes == 0) bytes = 1;  // Return a unique pointer even for zero-sized asks.

        // Power-of-two check — required for the bit-twiddling alignment path.
        if (align == 0 || (align & (align - 1)) != 0) {
            align = alignof(std::max_align_t);
        }

        // Try to fit into the current last block.
        if (!blocks_.empty()) {
            Block& b = blocks_.back();
            const auto base = reinterpret_cast<std::uintptr_t>(b.data.get()) + b.used;
            const auto alignedBase = (base + (align - 1)) & ~static_cast<std::uintptr_t>(align - 1);
            const std::size_t pad = static_cast<std::size_t>(alignedBase - base);
            if (pad + bytes <= b.size - b.used) {
                b.used += pad + bytes;
                totalAllocated_ += bytes;
                return reinterpret_cast<void*>(alignedBase);
            }
        }

        // Slow path — need a new block big enough to guarantee the request
        // fits even after worst-case alignment padding.
        Block& nb = grow_(bytes + align);
        const auto base = reinterpret_cast<std::uintptr_t>(nb.data.get());
        const auto alignedBase = (base + (align - 1)) & ~static_cast<std::uintptr_t>(align - 1);
        const std::size_t pad = static_cast<std::size_t>(alignedBase - base);
        nb.used = pad + bytes;
        totalAllocated_ += bytes;
        return reinterpret_cast<void*>(alignedBase);
    }

    // Construct T in arena-owned storage. T MUST be trivially destructible
    // — Arena will not invoke any destructor. If you need non-trivial types,
    // use a different allocator.
    template <typename T, typename... Args>
    T* construct(Args&&... args) {
        static_assert(std::is_trivially_destructible_v<T>,
            "Arena only supports trivially-destructible types (no dtor will run)");
        void* mem = allocate(sizeof(T), alignof(T));
        return ::new (mem) T(std::forward<Args>(args)...);
    }

    // Release every block. All pointers handed out by this Arena become
    // invalid. O(N) in the number of blocks.
    void reset() {
        blocks_.clear();
        totalAllocated_ = 0;
    }

    // Total bytes handed out via allocate() since the last reset(). Does
    // NOT include per-block padding or headroom — use it as a proxy for
    // the logical footprint, not the physical one.
    std::size_t totalAllocated() const noexcept { return totalAllocated_; }

    // Inspection helpers — useful for telemetry and tests.
    std::size_t blockCount() const noexcept { return blocks_.size(); }
    std::size_t blockSize()  const noexcept { return blockSize_; }
};

} // namespace oreo

#endif // OREO_MEMORY_POOL_H
