// tag_allocator.h — Document-derived deterministic tag allocation.
//
// Tags uniquely identify geometry operations for topological naming.
// They encode into element names via the FreeCAD element-map algorithm.
//
// Key properties:
//   - Deterministic within a document: reset() + same operations = same tags
//   - Document-scoped: two documents with different IDs never collide
//     WITHIN THE ENCODED-TAG SPACE (see "Encoding limits" below)
//   - Persistable: state can be saved and restored
//   - Tag format: (low32(documentId) << 32) | operationCounter (multi-doc)
//                 sequential 1,2,3...                          (single-doc)
//
// ── Encoding limits (read before claiming cross-doc uniqueness) ──
//
// The on-the-wire tag is a 64-bit signed integer carrying 32 bits of
// documentId and 32 bits of sequence counter. The FULL 64-bit documentId
// is stored in documentId_ for identity/debug, but ONLY its low 32 bits
// participate in the encoded tag. Two distinct 64-bit documentIds that
// share low-32 bits therefore produce identical encoded tags for equal
// sequence numbers.
//
// To detect this at the source rather than at name-lookup time, every
// non-zero documentId passed to the constructor or setDocumentId() is
// registered in a process-global map (low32 -> full documentId). A second
// registration of a DIFFERENT full documentId with the same low 32 bits
// throws std::logic_error immediately. Re-registering the same full
// documentId is idempotent and always succeeds.
//
// This check is process-wide. If tests create transient allocators with
// deliberately colliding IDs, call clearDocumentIdRegistry() between them.
//
// Thread safety:
//   The counter is std::atomic<int64_t>. nextTag() / peek() / allocateRange()
//   are safe to call concurrently. Configuration mutators (seed,
//   setDocumentId, reset, resetCounterOnly, restore) must NOT be called
//   concurrently with allocation — they throw std::logic_error if the
//   counter has already advanced (TA-5).
//
// For single-document use (most cases), documentId defaults to 0, and tags
// are simply 1, 2, 3... (backward compatible).

#ifndef OREO_TAG_ALLOCATOR_H
#define OREO_TAG_ALLOCATOR_H

#include "thread_safety.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

// TA-4: SipHash requires a 64-bit build for both performance and for the
// uint64_t widening we apply to documentId_.
static_assert(sizeof(void*) >= 8, "64-bit build required");

namespace oreo {

class OREO_CONTEXT_BOUND TagAllocator {
public:
    // Snapshot of allocator state — used for cloning / rollback (TA-7).
    struct Snapshot {
        int64_t  counter;
        uint64_t documentId;
    };

    TagAllocator() : documentId_(0), counter_(0), nextOcctTag_(1) {}

    // Create with a document ID for cross-document uniqueness.
    // documentId should be unique per document (e.g., SipHash of document UUID).
    // Throws std::logic_error if a DIFFERENT 64-bit documentId with the
    // same low 32 bits has already been registered in this process.
    explicit TagAllocator(uint64_t documentId, int64_t startCounter = 0)
        : documentId_(documentId), counter_(startCounter), nextOcctTag_(1) {
        if (startCounter < 0) {
            throw std::invalid_argument("TagAllocator: negative startCounter");
        }
        if (documentId != 0) {
            registerDocumentId(documentId);
        }
    }

    // Non-copyable, non-movable — atomics and the OCCT tag map have no
    // well-defined copy semantics, and allocator identity matters.
    TagAllocator(const TagAllocator&) = delete;
    TagAllocator& operator=(const TagAllocator&) = delete;
    TagAllocator(TagAllocator&&) = delete;
    TagAllocator& operator=(TagAllocator&&) = delete;

    // Allocate the next tag. Combines documentId with sequential counter.
    // Single-document mode (documentId=0): returns 1, 2, 3...
    // Multi-document mode: upper 32 bits = documentId (low 32 bits thereof),
    //                      lower 32 bits = seq.
    //
    // TA-1: counter is atomic; fetch_add with relaxed ordering is sufficient
    //       because documentId_ is set at construction (or pre-allocation via
    //       setDocumentId, guarded by TA-5) and is never mutated concurrently
    //       with allocation — so a plain read after the atomic increment is
    //       race-free.
    // TA-2: single-doc mode now checks for signed overflow against INT64_MAX.
    int64_t nextTag() {
        int64_t prev = counter_.fetch_add(1, std::memory_order_relaxed);
        // Signed overflow detection: prev must be non-negative, and prev+1
        // must still be non-negative. If prev == INT64_MAX, the stored
        // value wrapped to INT64_MIN — we cannot safely roll back under
        // concurrency, so we surface the error and leave the counter as-is.
        // Subsequent callers will see the negative value and also throw.
        if (prev < 0 || prev == std::numeric_limits<int64_t>::max()) {
            throw std::overflow_error("TagAllocator: counter overflow (int64)");
        }
        int64_t seq = prev + 1;

        const uint64_t docId = documentId_; // plain read; see TA-1 comment
        if (docId == 0) {
            return seq;
        }
        if (seq > static_cast<int64_t>(UINT32_MAX)) {
            throw std::overflow_error(
                "TagAllocator: counter overflow in multi-document mode "
                "(>2^32 ops per doc)");
        }
        // Only the low 32 bits of docId participate in the encoding — that's
        // the on-the-wire slot width we have. The full 64-bit docId is kept
        // in documentId_ for identity/debug; callers needing full-precision
        // cross-doc uniqueness should use snapshot().documentId.
        return (static_cast<int64_t>(docId & 0xFFFFFFFFu) << 32)
             | (seq & 0xFFFFFFFFll);
    }

    // TA-7: Peek at what nextTag() would return, without advancing.
    // In multi-doc mode this returns the *encoded* tag for convenience.
    int64_t peek() const {
        int64_t cur = counter_.load(std::memory_order_relaxed);
        if (cur < 0 || cur == std::numeric_limits<int64_t>::max()) {
            throw std::overflow_error("TagAllocator: counter overflow");
        }
        int64_t seq = cur + 1;
        const uint64_t docId = documentId_;
        if (docId == 0) return seq;
        if (seq > static_cast<int64_t>(UINT32_MAX)) {
            throw std::overflow_error(
                "TagAllocator: counter overflow in multi-document mode");
        }
        return (static_cast<int64_t>(docId & 0xFFFFFFFFu) << 32)
             | (seq & 0xFFFFFFFFll);
    }

    // TA-7: Atomically reserve n sequential tags. Returns [start, end) — a
    // half-open range of *sequence* numbers; callers can encode each with
    // encodeTag() below if they need the docId-combined form.
    // n must be > 0.
    std::pair<int64_t, int64_t> allocateRange(int64_t n) {
        if (n <= 0) {
            throw std::invalid_argument(
                "TagAllocator::allocateRange: n must be positive");
        }
        int64_t prev = counter_.fetch_add(n, std::memory_order_relaxed);
        // Overflow guard — detect signed wrap. We need prev+n+1 to not
        // overflow, i.e. prev <= INT64_MAX - n - 1, AND prev must be
        // non-negative (a prior overflow would leave it negative).
        if (prev < 0 || prev > std::numeric_limits<int64_t>::max() - n - 1) {
            // Cannot reliably roll back under concurrency — other threads
            // may have advanced further. Leave counter as-is and fail.
            throw std::overflow_error(
                "TagAllocator::allocateRange: counter overflow");
        }
        int64_t start = prev + 1;
        int64_t end   = prev + n + 1; // half-open
        const uint64_t docId = documentId_;
        if (docId != 0 &&
            (end - 1) > static_cast<int64_t>(UINT32_MAX)) {
            throw std::overflow_error(
                "TagAllocator::allocateRange: multi-doc 32-bit overflow");
        }
        return {start, end};
    }

    // Encode a raw sequence number as a tag in the current documentId space.
    // Exposed for callers of allocateRange().
    int64_t encodeTag(int64_t seq) const {
        if (seq <= 0) {
            throw std::invalid_argument("TagAllocator::encodeTag: seq must be > 0");
        }
        const uint64_t docId = documentId_;
        if (docId == 0) return seq;
        if (seq > static_cast<int64_t>(UINT32_MAX)) {
            throw std::overflow_error(
                "TagAllocator::encodeTag: 32-bit overflow in multi-doc mode");
        }
        return (static_cast<int64_t>(docId & 0xFFFFFFFFu) << 32)
             | (seq & 0xFFFFFFFFll);
    }

    // TA-8: Full reset to default-constructed state: counter AND documentId
    // to 0. Also clears the OCCT tag map. This is the "clean slate" reset.
    void reset() {
        counter_.store(0, std::memory_order_relaxed);
        documentId_ = 0;
        occtTagMap_.clear();
        nextOcctTag_.store(1, std::memory_order_relaxed);
    }

    // TA-8: Reset only the counter; preserves documentId and the OCCT map.
    // Use when replaying operations within the same document.
    void resetCounterOnly() {
        counter_.store(0, std::memory_order_relaxed);
    }

    // TA-5/TA-6: Seed with a specific counter value (for restoring persisted
    // state). Throws if allocation has already begun, or if startValue < 0.
    void seed(int64_t startValue) {
        if (counter_.load(std::memory_order_relaxed) != 0) {
            throw std::logic_error("TagAllocator::seed after allocation");
        }
        if (startValue < 0) {
            throw std::invalid_argument("TagAllocator::seed: negative value");
        }
        counter_.store(startValue, std::memory_order_relaxed);
    }

    // TA-5: Set document ID. Throws if allocation has already begun.
    // Also registers `id` in the process-global low-32-bit collision
    // registry; throws std::logic_error if a DIFFERENT full 64-bit
    // documentId sharing the same low 32 bits has already been seen.
    void setDocumentId(uint64_t id) {
        if (counter_.load(std::memory_order_relaxed) != 0) {
            throw std::logic_error(
                "TagAllocator::setDocumentId after allocation");
        }
        if (id != 0) {
            registerDocumentId(id);
        }
        documentId_ = id;
    }

    // Register `id` in the process-global low-32-bit collision registry.
    // Re-registering the same full id is idempotent. Registering a
    // DIFFERENT id with the same low 32 bits throws std::logic_error.
    // Exposed so tooling (tests, diagnostics) can pre-check an id.
    static void registerDocumentId(uint64_t id) {
        const uint32_t low = static_cast<uint32_t>(id & 0xFFFFFFFFu);
        std::lock_guard<std::mutex> g(docIdRegistryMutex_());
        auto& map = docIdRegistry_();
        auto it = map.find(low);
        if (it == map.end()) {
            map.emplace(low, id);
            return;
        }
        if (it->second != id) {
            throw std::logic_error(
                "TagAllocator::registerDocumentId: low-32-bit collision "
                "between two distinct 64-bit documentIds (existing=0x"
                + toHex(it->second) + ", new=0x" + toHex(id)
                + "). Encoded tags would be indistinguishable.");
        }
        // Same id already registered — idempotent, accept silently.
    }

    // Test helper: drop every registered documentId from the collision
    // registry. ONLY for test isolation — production code should never
    // need this. Not thread-safe w.r.t. concurrent TagAllocator ctors.
    static void clearDocumentIdRegistry() noexcept {
        std::lock_guard<std::mutex> g(docIdRegistryMutex_());
        docIdRegistry_().clear();
    }

    // Get document ID (full 64-bit).
    uint64_t documentId() const { return documentId_; }

    // Current counter value (for persistence).
    int64_t currentValue() const {
        return counter_.load(std::memory_order_relaxed);
    }

    // Check if any tags have been allocated.
    bool empty() const {
        return counter_.load(std::memory_order_relaxed) == 0;
    }

    // TA-7: Snapshot / restore for cloning and rollback.
    Snapshot snapshot() const {
        Snapshot s;
        s.counter    = counter_.load(std::memory_order_relaxed);
        s.documentId = documentId_;
        return s;
    }

    // Restore to a prior snapshot. By default, this is only permitted if the
    // current counter has not advanced past the snapshot (prevents time
    // travel that would re-issue already-handed-out tags). Pass force=true
    // to override (e.g., discarding a failed transaction).
    void restore(const Snapshot& snap, bool force = false) {
        if (snap.counter < 0) {
            throw std::invalid_argument(
                "TagAllocator::restore: negative snapshot counter");
        }
        int64_t current = counter_.load(std::memory_order_relaxed);
        if (!force && current > snap.counter) {
            throw std::logic_error(
                "TagAllocator::restore: counter has advanced past snapshot "
                "(use force=true to discard issued tags)");
        }
        counter_.store(snap.counter, std::memory_order_relaxed);
        documentId_ = snap.documentId;
        // OCCT map is intentionally NOT restored — it's a per-session
        // mapping, not part of the logical allocator state.
    }

    // Extract the document ID bits from a tag (upper 32 bits).
    // Note: in multi-doc mode this returns the low 32 bits of documentId_.
    static uint32_t extractDocumentId(int64_t tag) {
        return static_cast<uint32_t>((static_cast<uint64_t>(tag) >> 32)
                                     & 0xFFFFFFFFu);
    }

    // Extract the operation counter from a tag (lower 32 bits).
    static uint32_t extractCounter(int64_t tag) {
        return static_cast<uint32_t>(static_cast<uint64_t>(tag)
                                     & 0xFFFFFFFFu);
    }

    // TA-3: Convert an int64_t tag to a 32-bit value for OCCT APIs that
    // require int (OCCT's Standard_Integer is 32-bit).
    //
    //   * Multi-doc tags (any tag with bits set above bit 31): use the
    //     per-allocator mapping. Returns a stable 32-bit handle that's
    //     unique within THIS allocator instance.
    //   * Single-doc tags that fit in int32_t: pass through for backward
    //     compat.
    //   * Single-doc tags that overflow int32_t: also go through the map.
    //
    // IMPORTANT: the mapping is per-allocator and does NOT persist across
    // serialization. Do not rely on it for cross-session identity.
    //
    // Throws std::overflow_error if the OCCT tag space itself overflows
    // (>2^31 - 1 distinct mappings in one allocator — effectively never).
    int32_t toOcctTag(int64_t tag) {
        if (tag <= 0) {
            throw std::invalid_argument(
                "TagAllocator::toOcctTag: non-positive tag");
        }
        // Fast path: tag fits in int32_t (no high bits set). This covers
        // single-doc tags 1..INT32_MAX. A multi-doc tag with docId==0 in the
        // encoding would be indistinguishable from a single-doc tag anyway,
        // so passing it through is correct.
        if (tag <= std::numeric_limits<int32_t>::max()) {
            return static_cast<int32_t>(tag);
        }
        // Slow path: multi-doc or >2^31. Map via per-allocator table.
        auto it = occtTagMap_.find(tag);
        if (it != occtTagMap_.end()) return it->second;
        int next = nextOcctTag_.fetch_add(1, std::memory_order_relaxed);
        if (next <= 0) {
            throw std::overflow_error(
                "TagAllocator::toOcctTag: OCCT 32-bit tag space exhausted");
        }
        occtTagMap_.emplace(tag, next);
        return next;
    }

    // TA-4: Generate a document ID from a string (e.g., UUID) using
    // SipHash-2-4 with a fixed compile-time key. Produces a 64-bit hash.
    // Not cryptographic in the authentication sense, but dramatically
    // better than FNV-1a for collision resistance on short strings.
    static uint64_t documentIdFromString(const std::string& s) {
        // Compile-time seed key. These are arbitrary but fixed — changing
        // them would invalidate all persisted documentIds. Chosen as two
        // 64-bit constants with good bit balance.
        constexpr uint64_t k0 = 0x0f1e2d3c4b5a6978ull;
        constexpr uint64_t k1 = 0x8796a5b4c3d2e1f0ull;
        return siphash_2_4(s.data(), s.size(), k0, k1);
    }

private:
    // ---------- SipHash-2-4 (64-bit output) ----------
    // Reference: Aumasson & Bernstein, "SipHash: a fast short-input PRF",
    // https://www.aumasson.jp/siphash/siphash.pdf
    // This is a direct transcription of the reference algorithm.
    static inline uint64_t rotl64(uint64_t x, int b) {
        return (x << b) | (x >> (64 - b));
    }

    static inline uint64_t read_u64_le(const uint8_t* p) {
        uint64_t v;
        std::memcpy(&v, p, 8);
        // Assume little-endian host (x86/x64/ARM64 in LE mode — covered by
        // our 64-bit MSVC target).
        return v;
    }

    static inline void sip_round(uint64_t& v0, uint64_t& v1,
                                 uint64_t& v2, uint64_t& v3) {
        v0 += v1; v1 = rotl64(v1, 13); v1 ^= v0; v0 = rotl64(v0, 32);
        v2 += v3; v3 = rotl64(v3, 16); v3 ^= v2;
        v0 += v3; v3 = rotl64(v3, 21); v3 ^= v0;
        v2 += v1; v1 = rotl64(v1, 17); v1 ^= v2; v2 = rotl64(v2, 32);
    }

    static uint64_t siphash_2_4(const void* in, size_t inlen,
                                uint64_t k0, uint64_t k1) {
        uint64_t v0 = 0x736f6d6570736575ull ^ k0;
        uint64_t v1 = 0x646f72616e646f6dull ^ k1;
        uint64_t v2 = 0x6c7967656e657261ull ^ k0;
        uint64_t v3 = 0x7465646279746573ull ^ k1;

        const uint8_t* p   = static_cast<const uint8_t*>(in);
        const size_t  left = inlen & 7;
        const uint8_t* end = p + inlen - left;

        for (; p != end; p += 8) {
            uint64_t m = read_u64_le(p);
            v3 ^= m;
            sip_round(v0, v1, v2, v3); // c = 2
            sip_round(v0, v1, v2, v3);
            v0 ^= m;
        }

        uint64_t b = static_cast<uint64_t>(inlen) << 56;
        switch (left) {
            case 7: b |= static_cast<uint64_t>(p[6]) << 48; // fallthrough
            case 6: b |= static_cast<uint64_t>(p[5]) << 40; // fallthrough
            case 5: b |= static_cast<uint64_t>(p[4]) << 32; // fallthrough
            case 4: b |= static_cast<uint64_t>(p[3]) << 24; // fallthrough
            case 3: b |= static_cast<uint64_t>(p[2]) << 16; // fallthrough
            case 2: b |= static_cast<uint64_t>(p[1]) << 8;  // fallthrough
            case 1: b |= static_cast<uint64_t>(p[0]);       // fallthrough
            case 0: break;
        }

        v3 ^= b;
        sip_round(v0, v1, v2, v3); // c = 2
        sip_round(v0, v1, v2, v3);
        v0 ^= b;

        v2 ^= 0xff;
        sip_round(v0, v1, v2, v3); // d = 4
        sip_round(v0, v1, v2, v3);
        sip_round(v0, v1, v2, v3);
        sip_round(v0, v1, v2, v3);

        return v0 ^ v1 ^ v2 ^ v3;
    }
    // ---------- end SipHash ----------

    uint64_t                              documentId_;
    std::atomic<int64_t>                  counter_;
    // OCCT per-allocator mapping (TA-3). Not thread-safe w.r.t. concurrent
    // inserts — this class is OREO_CONTEXT_BOUND, so single-thread access
    // is the contract. If toOcctTag ever needs concurrent use, wrap
    // occtTagMap_ in a mutex.
    std::unordered_map<int64_t, int32_t> occtTagMap_;
    std::atomic<int32_t>                  nextOcctTag_;

    // Process-global registry for the low-32-bit collision check.
    // Function-local statics so the initializer order across translation
    // units doesn't matter.
    static std::unordered_map<uint32_t, uint64_t>& docIdRegistry_() {
        static std::unordered_map<uint32_t, uint64_t> m;
        return m;
    }
    static std::mutex& docIdRegistryMutex_() {
        static std::mutex m;
        return m;
    }
    static std::string toHex(uint64_t v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%016llx",
                      static_cast<unsigned long long>(v));
        return std::string(buf);
    }
};

} // namespace oreo

#endif // OREO_TAG_ALLOCATOR_H
