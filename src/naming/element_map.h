// element_map.h — Production-grade bidirectional element name mapping.
//
// This is the core data structure for topological naming in oreo-kernel.
// It maintains stable names for BRep sub-shapes (faces, edges, vertices)
// across parametric rebuilds.
//
// Design based on FreeCAD 1.0's ElementMap (LGPL-2.1+), clean reimplementation.
//
// Key properties:
//   1. Bidirectional: IndexedName ↔ MappedName
//   2. History-aware: MappedNames encode full operation chain
//   3. Deterministic: same topology → same names
//   4. Supports child element maps for multi-input operations
//   5. Serializable for snapshot storage

#ifndef OREO_ELEMENT_MAP_H
#define OREO_ELEMENT_MAP_H

#include "indexed_name.h"
#include "mapped_name.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace oreo {

class ElementMap;
using ElementMapPtr = std::shared_ptr<ElementMap>;

// Child element map reference — stores a reference to an input shape's
// element map along with the postfix applied during the operation.
struct ChildElementMap {
    ElementMapPtr map;
    std::int64_t tag;        // Operation tag (int64 — widened from long so
                             // high bits survive on Windows MSVC, where long
                             // is 32-bit and would silently truncate).
    std::string postfix;     // Disambiguation postfix (for multi-input ops)
};

// History trace item — one step in the derivation chain of an element name
struct HistoryTraceItem {
    MappedName name;
    std::int64_t tag;
    std::string operation;   // "Extrude", "Fillet", etc.
};

class ElementMap {
public:
    ElementMap() = default;

    // ─── Core mapping ────────────────────────────────────────

    // Set a name for an indexed element. If the mapped name already exists
    // for a different index, disambiguates by appending ";D<n>" suffix.
    // Returns the actual MappedName used (may differ from input if disambiguated).
    MappedName setElementName(const IndexedName& index, const MappedName& name,
                              std::int64_t tag = 0, bool overwrite = false);

    // Get the mapped name for an indexed element. Returns empty if not found.
    MappedName getMappedName(const IndexedName& index) const;

    // Get the indexed name for a mapped name. Returns null IndexedName if not found.
    IndexedName getIndexedName(const MappedName& name) const;

    // Check if a mapping exists
    bool hasIndexedName(const IndexedName& index) const;
    bool hasMappedName(const MappedName& name) const;

    // Remove a mapping by indexed name
    void removeElement(const IndexedName& index);

    // ─── Bulk operations ─────────────────────────────────────

    // Get all mappings, optionally filtered by type
    std::vector<std::pair<IndexedName, MappedName>> getAll(const std::string& type = {}) const;

    // Count elements of a given type (empty = all types)
    int count(const std::string& type = {}) const;

    // Clear all mappings and children
    void clear();

    // ─── Child element maps ──────────────────────────────────

    // Add a child element map (from an input shape).
    // Used by multi-input operations (booleans) to reference input element maps.
    void addChild(const ChildElementMap& child);

    // Get all child element maps
    const std::vector<ChildElementMap>& children() const { return children_; }

    // Import names from a child element map into this map.
    // Used during mapShapeElements to pull in input names.
    void importChildNames(const ElementMap& childMap, std::int64_t childTag,
                          const std::string& childPostfix);

    // ─── History encoding ────────────────────────────────────

    // Encode a new element name from an input name + operation tag + postfix.
    // This is the main entry point for building history chains.
    //
    // Example: encodeElementName("Face", inputName, tag, ";:M")
    //   produces: inputName + ";:H<tag_hex>" + ";:M"
    static MappedName encodeElementName(
        const char* type,
        const MappedName& inputName,
        std::int64_t tag,
        const char* postfix);

    // ─── History tracing ─────────────────────────────────────

    // Trace the derivation history of a mapped name.
    // Parses the encoded postfixes to reconstruct the operation chain.
    // Returns the chain from newest to oldest.
    static std::vector<HistoryTraceItem> traceHistory(const MappedName& name);

    // Extract the base (original) element name from a mapped name,
    // stripping all operation postfixes.
    static std::string extractBaseName(const MappedName& name);

    // Extract the operation tag from a mapped name postfix. Returns a signed
    // 64-bit value so that tags produced in multi-document mode (with high
    // bits derived from documentId) are preserved without truncation on
    // platforms where `long` is 32-bit (Windows MSVC).
    static std::int64_t extractTag(const MappedName& name);

    // Extract the full 64-bit documentId from a mapped name's ;:Q postfix.
    // Returns 0 if the name has no ;:Q postfix (single-document mode).
    static std::uint64_t extractDocumentId(const MappedName& name);

    // ─── Serialization ───────────────────────────────────────

    // Binary format version (bump when format changes).
    // v2 widens per-entry `tag` from 32-bit (implicit on Windows, explicit on
    // Linux-long) to a full 64-bit value and introduces an explicit layout
    // version in the element-map header. v1 files were produced before the
    // cross-platform widening audit — they are no longer accepted.
    static constexpr std::uint32_t FORMAT_VERSION = 2;

    // Serialize to binary buffer
    std::vector<uint8_t> serialize() const;

    // Deserialize from binary buffer. Returns nullptr on failure.
    static ElementMapPtr deserialize(const uint8_t* data, size_t len);

private:
    // Forward mapping: IndexedName → MappedName
    std::map<IndexedName, MappedName> indexToMapped_;

    // Reverse mapping: MappedName → IndexedName
    std::map<MappedName, IndexedName> mappedToIndex_;

    // Per-type element counters (for auto-generating IndexedName indices)
    std::unordered_map<std::string, int> typeCounters_;

    // Child element maps (from input shapes)
    std::vector<ChildElementMap> children_;

    // Duplicate counter for disambiguation
    int duplicateCounter_ = 0;
};

} // namespace oreo

#endif // OREO_ELEMENT_MAP_H
