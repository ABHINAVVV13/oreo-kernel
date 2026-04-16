// tag_allocator.h — Document-derived deterministic tag allocation.
//
// Tags uniquely identify geometry operations for topological naming.
// They encode into element names via the FreeCAD element-map algorithm.
//
// Key properties:
//   - Deterministic within a document: reset() + same operations = same tags
//   - Document-scoped: two documents with different IDs never collide
//   - Persistable: state can be saved and restored
//   - Tag format: (documentId << 32) | operationCounter
//     This gives 2^32 documents × 2^32 operations per document
//
// For single-document use (most cases), documentId defaults to 0,
// and tags are simply 1, 2, 3... (backward compatible).

#ifndef OREO_TAG_ALLOCATOR_H
#define OREO_TAG_ALLOCATOR_H

#include "thread_safety.h"

#include <atomic>
#include <cstdint>
#include <limits>
#include <string>

namespace oreo {

class OREO_CONTEXT_BOUND TagAllocator {
public:
    TagAllocator() : documentId_(0), counter_(0) {}

    // Create with a document ID for cross-document uniqueness.
    // documentId should be unique per document (e.g., hash of document UUID).
    explicit TagAllocator(uint32_t documentId, int64_t startCounter = 0)
        : documentId_(documentId), counter_(startCounter) {}

    // Allocate the next tag. Combines documentId with sequential counter.
    // Single-document mode (documentId=0): returns 1, 2, 3...
    // Multi-document mode: upper 32 bits = documentId, lower 32 bits = counter
    // This gives 2^32 document spaces × 2^32 operations per document.
    int64_t nextTag() {
        int64_t seq = ++counter_;
        if (documentId_ == 0) return seq;
        return (static_cast<int64_t>(documentId_) << 32) | (static_cast<int64_t>(seq) & 0xFFFFFFFF);
    }

    // Reset counter for deterministic replay.
    // Document ID is preserved — only the operation counter resets.
    void reset() { counter_.store(0); }

    // Seed with a specific counter value (for restoring persisted state).
    void seed(int64_t startValue) { counter_.store(startValue); }

    // Set document ID (for cross-document uniqueness).
    void setDocumentId(uint32_t id) { documentId_ = id; }

    // Get document ID.
    uint32_t documentId() const { return documentId_; }

    // Current counter value (for persistence).
    int64_t currentValue() const { return counter_.load(); }

    // Check if any tags have been allocated.
    bool empty() const { return counter_.load() == 0; }

    // Extract the document ID bits from a tag (upper 32 bits).
    static uint32_t extractDocumentId(int64_t tag) {
        return static_cast<uint32_t>((tag >> 32) & 0xFFFFFFFF);
    }

    // Extract the operation counter from a tag (lower 32 bits).
    static uint32_t extractCounter(int64_t tag) {
        return static_cast<uint32_t>(tag & 0xFFFFFFFF);
    }

    // Convert an int64_t tag to a 32-bit value for OCCT APIs that require int.
    // If the tag fits in int32_t, it passes through unchanged.
    // Otherwise, the lower 31 bits are used (keeps the value positive).
    static int32_t toOcctTag(int64_t tag) {
        if (tag >= 0 && tag <= std::numeric_limits<int32_t>::max()) {
            return static_cast<int32_t>(tag);
        }
        return static_cast<int32_t>(tag & 0x7FFFFFFF);
    }

    // Generate a document ID from a string (e.g., UUID).
    // Uses a simple hash — not cryptographic, just for uniqueness.
    static uint32_t documentIdFromString(const std::string& s) {
        // FNV-1a 32-bit hash
        uint32_t hash = 2166136261u;
        for (char c : s) {
            hash ^= static_cast<uint32_t>(c);
            hash *= 16777619u;
        }
        return hash;
    }

private:
    uint32_t documentId_;
    std::atomic<int64_t> counter_;
};

} // namespace oreo

#endif // OREO_TAG_ALLOCATOR_H
