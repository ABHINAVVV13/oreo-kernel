// SPDX-License-Identifier: LGPL-2.1-or-later

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

#include "core/shape_identity.h"
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
//
// Phase 3 migration (identity-model v2): the `tag` field (int64) has
// been replaced with `id` (ShapeIdentity). Callers that previously did
// `child.tag = opTag` must switch to `child.id = ShapeIdentity{doc, ctr}`.
// This is a breaking struct-field change with no compat shim — the v1
// field could only carry 32 bits of documentId.
struct ChildElementMap {
    ElementMapPtr map;
    ShapeIdentity id;        // Operation identity — full 64+64. (Phase 3, was int64 tag.)
    std::string postfix;     // Disambiguation postfix (for multi-input ops)
};

// History trace item — one step in the derivation chain of an element name
struct HistoryTraceItem {
    MappedName name;
    ShapeIdentity id;        // Phase 3: full ShapeIdentity (was int64 tag).
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
    //
    // Phase 3 migration: childTag (int64) → childId (ShapeIdentity).
    void importChildNames(const ElementMap& childMap,
                          ShapeIdentity childId,
                          const std::string& childPostfix);

    // ─── History encoding ────────────────────────────────────

    // Encode a new element name from an input name + operation identity
    // + postfix. This is the main entry point for building history
    // chains.
    //
    // Phase 3 migration: emits ";:P<16hex>.<16hex>" via
    // appendShapeIdentity() rather than the legacy ";:H<hex>" carrier.
    // Callers that want to WRITE the legacy form should first encode
    // the identity with encodeV1Scalar() and build the postfix manually
    // — there is no longer a convenience for that.
    //
    // Example: encodeElementName("Face", inputName, id, ";:M")
    //   produces: inputName + ";:P<16hex>.<16hex>" + ";:M"
    static MappedName encodeElementName(
        const char* type,
        const MappedName& inputName,
        ShapeIdentity id,
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
    //
    // v2 compat: internally calls extractShapeIdentity() and then encodeV1Scalar().
    // Throws std::overflow_error via encodeV1Scalar when a v2 name's
    // counter > UINT32_MAX (multi-doc) — see identity-model.md §4.3 rule 5.
    // Callers on the legacy writer path must wrap this in try/catch and
    // emit V2_IDENTITY_NOT_REPRESENTABLE on failure.
    static std::int64_t extractTag(const MappedName& name);

    // Extract the full 64-bit documentId from a mapped name's ;:Q postfix.
    // Returns 0 if the name has no ;:Q postfix (single-document mode).
    static std::uint64_t extractDocumentId(const MappedName& name);

    // Extract the v2 ShapeIdentity from a mapped name using the
    // rightmost-marker algorithm (docs/identity-model.md §4.3 rule 4):
    //   1. Find rightmost of {;:P, ;:H, ;:T}.
    //   2. ;:P wins: parse 32hex directly.
    //   3. ;:H / ;:T wins: parse scalar, decodeV1Scalar with ambient
    //      documentId from ;:Q (or 0 if absent). An Error Case mismatch
    //      from decodeV1Scalar is caught and funneled through the
    //      malformed-payload path — extractShapeIdentity never throws.
    //   4. No marker / malformed payload: returns ShapeIdentity{0, 0};
    //      emits MALFORMED_ELEMENT_NAME via `diag` (if supplied) ONLY
    //      for the malformed-payload case, not for the no-marker case.
    //      A name with no identity markers is a bare base name, which
    //      is legitimate (e.g. freshly imported STEP); a name with a
    //      marker whose payload fails to parse is genuinely corrupt.
    //
    // No LEGACY_IDENTITY_DOWNGRADE emission from this overload — the
    // ;:H/;:T branch uses a non-diag decodeV1Scalar to keep this a
    // pure parser. Callers that need the downgrade warning should
    // decode the scalar themselves via decodeV1Scalar with a sink.
    static ShapeIdentity extractShapeIdentity(const MappedName& name,
                                              class DiagnosticCollector* diag = nullptr);

    // ─── Serialization ───────────────────────────────────────

    // Binary format version (bump when format changes).
    //   v1 — pre-audit; tag was truncated on Windows. REJECTED.
    //   v2 — 64-bit tag, but still a squeezed v1 scalar (int64 with
    //        low-32 of documentId shifted into high 32 bits). Reader
    //        kept for backward compat; writer removed in v3.
    //   v3 — full 16-byte ShapeIdentity per entry / per child.
    //        (Phase 3 / identity-model v2 hardening.)
    static constexpr std::uint32_t FORMAT_VERSION = 3;
    static constexpr std::uint32_t FORMAT_VERSION_V2_LEGACY = 2;

    // Serialize to binary buffer (always writes FORMAT_VERSION).
    std::vector<uint8_t> serialize() const;

    // Deserialize from binary buffer. Returns nullptr on malformed or
    // unsupported-version input.
    //
    // The `rootHint` is used to reconstruct full 64-bit documentIds when
    // reading v2-format buffers (the v2 format still stores squeezed int64
    // tags). Pass the root ShapeIdentity from the outer deserialize call
    // so v2→v3 upgrades are lossless for buffers produced by the source
    // document. See docs/identity-model.md §5.4.
    //
    // The `diag` sink (if non-null) receives LEGACY_IDENTITY_DOWNGRADE
    // warnings on Case B decodes; it is also used for the dedup flag
    // described in §5.4 (only one warning per top-level read).
    static ElementMapPtr deserialize(const uint8_t* data, size_t len,
                                     ShapeIdentity rootHint = {},
                                     class DiagnosticCollector* diag = nullptr);

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
