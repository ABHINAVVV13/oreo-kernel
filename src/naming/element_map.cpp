// element_map.cpp — Production-grade ElementMap implementation.

#include "element_map.h"

#include "core/diagnostic.h"
#include "core/shape_identity_v1.h"

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace oreo {

// ─── Core mapping ────────────────────────────────────────────

MappedName ElementMap::setElementName(const IndexedName& index, const MappedName& name,
                                      std::int64_t tag, bool overwrite) {
    if (index.isNull() || name.empty()) return {};

    // Check for duplicate mapped name
    auto existing = mappedToIndex_.find(name);
    if (existing != mappedToIndex_.end()) {
        if (existing->second == index) {
            return name;  // Same mapping already exists
        }
        if (!overwrite) {
            // Disambiguate: append ";D<n>" suffix up to 100 attempts
            for (int attempt = 1; attempt <= 100; ++attempt) {
                MappedName disambiguated(name.data() + ElementCodes::POSTFIX_DUPLICATE
                                         + std::to_string(attempt));
                if (mappedToIndex_.find(disambiguated) == mappedToIndex_.end()) {
                    return setElementName(index, disambiguated, tag, false);
                }
            }
            // All 100 disambiguation attempts exhausted — overwrite
            return setElementName(index, name, tag, true);
        }
        // Overwrite: remove old reverse mapping
        mappedToIndex_.erase(existing);
    }

    // Remove any old mapping for this index
    auto oldMapping = indexToMapped_.find(index);
    if (oldMapping != indexToMapped_.end()) {
        mappedToIndex_.erase(oldMapping->second);
    }

    // Set the new mapping
    indexToMapped_[index] = name;
    mappedToIndex_[name] = index;

    // Update type counter
    auto& counter = typeCounters_[index.type()];
    if (index.index() > counter) {
        counter = index.index();
    }

    return name;
}

MappedName ElementMap::getMappedName(const IndexedName& index) const {
    auto it = indexToMapped_.find(index);
    return (it != indexToMapped_.end()) ? it->second : MappedName();
}

IndexedName ElementMap::getIndexedName(const MappedName& name) const {
    auto it = mappedToIndex_.find(name);
    return (it != mappedToIndex_.end()) ? it->second : IndexedName();
}

bool ElementMap::hasIndexedName(const IndexedName& index) const {
    return indexToMapped_.count(index) > 0;
}

bool ElementMap::hasMappedName(const MappedName& name) const {
    return mappedToIndex_.count(name) > 0;
}

void ElementMap::removeElement(const IndexedName& index) {
    auto it = indexToMapped_.find(index);
    if (it != indexToMapped_.end()) {
        mappedToIndex_.erase(it->second);
        indexToMapped_.erase(it);
    }
}

// ─── Bulk operations ─────────────────────────────────────────

std::vector<std::pair<IndexedName, MappedName>> ElementMap::getAll(const std::string& type) const {
    std::vector<std::pair<IndexedName, MappedName>> result;
    result.reserve(indexToMapped_.size());
    for (auto& [idx, name] : indexToMapped_) {
        if (type.empty() || idx.type() == type) {
            result.emplace_back(idx, name);
        }
    }
    return result;
}

int ElementMap::count(const std::string& type) const {
    if (type.empty()) return static_cast<int>(indexToMapped_.size());
    int n = 0;
    for (auto& [idx, _] : indexToMapped_) {
        if (idx.type() == type) ++n;
    }
    return n;
}

void ElementMap::clear() {
    indexToMapped_.clear();
    mappedToIndex_.clear();
    typeCounters_.clear();
    children_.clear();
    duplicateCounter_ = 0;
}

// ─── Child element maps ──────────────────────────────────────

void ElementMap::addChild(const ChildElementMap& child) {
    children_.push_back(child);
}

void ElementMap::importChildNames(const ElementMap& childMap,
                                  ShapeIdentity childId,
                                  const std::string& childPostfix) {
    // Import all mappings from child, re-encoding with child identity + postfix
    for (auto& [idx, name] : childMap.indexToMapped_) {
        MappedName importedName = encodeElementName(
            idx.type().c_str(), name, childId,
            childPostfix.empty() ? ElementCodes::POSTFIX_CHILD : childPostfix.c_str());
        // Don't overwrite existing names — child names are lower priority.
        // setElementName stores the mapping; the int64 tag parameter is
        // vestigial (not stored) so we pass 0 rather than encoding the v2
        // identity back to v1.
        if (!hasIndexedName(idx)) {
            setElementName(idx, importedName, /*tag=*/0);
        }
    }
}

// ─── History encoding ────────────────────────────────────────

MappedName ElementMap::encodeElementName(
    const char* /*type*/,
    const MappedName& inputName,
    ShapeIdentity id,
    const char* postfix)
{
    // Build: inputName + ";:P<16hex>.<16hex>" + postfix. Phase 3 switches
    // from the legacy ;:H carrier to the full v2 ;:P carrier. Readers
    // handle mixed chains via the rightmost-marker rule.
    MappedName result(inputName);
    result.appendShapeIdentity(id);
    if (postfix && *postfix) {
        result.appendPostfix(postfix);
    }
    return result;
}

// ─── History tracing ─────────────────────────────────────────

std::vector<HistoryTraceItem> ElementMap::traceHistory(const MappedName& name) {
    std::vector<HistoryTraceItem> chain;
    const std::string& data = name.data();

    // Parse postfixes from right to left. Each hop is one of:
    //   ";:P<16hex>.<16hex>" (v2)
    //   ";:H<hex>"           (v1 hex)
    //   ";:T<dec>"           (v1 decimal)
    // followed by an op type postfix (;:M, ;:G, ;:MG, ;:C, ...).
    //
    // Ambient documentId (from ;:Q, if present) is used as the v1 decode
    // hint to reconstruct full 64-bit docIds. An Error Case mismatch
    // from decodeV1Scalar is caught and the chain stops at that point —
    // we never throw from a trace parser.
    const std::uint64_t ambientDoc = extractDocumentId(name);

    size_t pos = data.size();
    int maxDepth = 50;  // Prevent infinite loops on malformed names

    while (pos > 0 && maxDepth-- > 0) {
        size_t posP = (pos == 0) ? std::string::npos
                                 : data.rfind(ElementCodes::POSTFIX_IDENTITY, pos - 1);
        size_t posH = (pos == 0) ? std::string::npos
                                 : data.rfind(ElementCodes::POSTFIX_TAG, pos - 1);
        size_t posT = (pos == 0) ? std::string::npos
                                 : data.rfind(ElementCodes::POSTFIX_DECIMAL_TAG, pos - 1);

        // Rightmost-marker selection.
        size_t bestPos = std::string::npos;
        enum { None, IdentP, HexH, DecT } kind = None;
        auto considerMarker = [&](size_t p, auto k) {
            if (p != std::string::npos && (bestPos == std::string::npos || p > bestPos)) {
                bestPos = p;
                kind = k;
            }
        };
        considerMarker(posP, IdentP);
        considerMarker(posH, HexH);
        considerMarker(posT, DecT);
        if (bestPos == std::string::npos) break;

        ShapeIdentity idValue{};
        size_t valueEnd = bestPos + 3;  // length of ";:X" marker
        if (kind == IdentP) {
            // 32 hex chars (16 docId + 16 counter, no separator).
            if (valueEnd + ElementCodes::POSTFIX_IDENTITY_HEX_LEN > data.size()) break;
            auto parseHex16 = [&](size_t off, std::uint64_t& out) -> bool {
                std::uint64_t v = 0;
                for (size_t i = 0; i < 16; ++i) {
                    char c = data[off + i];
                    std::uint64_t d;
                    if      (c >= '0' && c <= '9') d = static_cast<std::uint64_t>(c - '0');
                    else if (c >= 'a' && c <= 'f') d = static_cast<std::uint64_t>(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') d = static_cast<std::uint64_t>(c - 'A' + 10);
                    else return false;
                    v = (v << 4) | d;
                }
                out = v;
                return true;
            };
            ShapeIdentity parsed;
            if (!parseHex16(valueEnd,      parsed.documentId)) break;
            if (!parseHex16(valueEnd + 16, parsed.counter))    break;
            if (!parsed.isValid() && parsed.documentId == 0) break;
            idValue = parsed;
            valueEnd += ElementCodes::POSTFIX_IDENTITY_HEX_LEN;
        } else {
            // v1 hex or decimal scalar
            size_t parseEnd = valueEnd;
            const int base = (kind == HexH) ? 16 : 10;
            while (parseEnd < data.size()) {
                char c = data[parseEnd];
                bool ok = (base == 16) ? std::isxdigit(static_cast<unsigned char>(c))
                                       : std::isdigit(static_cast<unsigned char>(c));
                if (!ok) break;
                ++parseEnd;
            }
            if (parseEnd == valueEnd) break;
            // See extractShapeIdentity comment on the hex vs. decimal
            // split — hex scalars must parse through strtoull to
            // preserve the full 64-bit bit pattern for multi-doc
            // encodings with low32(docId) >= 0x80000000.
            std::int64_t scalar = 0;
            if (base == 16) {
                scalar = static_cast<std::int64_t>(std::strtoull(
                    data.substr(valueEnd, parseEnd - valueEnd).c_str(), nullptr, 16));
            } else {
                scalar = std::strtoll(
                    data.substr(valueEnd, parseEnd - valueEnd).c_str(), nullptr, 10);
            }
            try {
                idValue = oreo::decodeV1Scalar(scalar, ambientDoc, nullptr);
            } catch (const std::invalid_argument&) {
                break;  // Error Case: bail out of the trace silently.
            }
            valueEnd = parseEnd;
        }

        // Determine the operation type from the postfix after the tag
        std::string opType = "Unknown";
        size_t afterTag = valueEnd;
        if (data.compare(afterTag, 3, ";:M") == 0) opType = "Modified";
        else if (data.compare(afterTag, 3, ";:G") == 0) opType = "Generated";
        else if (data.compare(afterTag, 4, ";:MG") == 0) opType = "ModifiedGenerated";
        else if (data.compare(afterTag, 3, ";:C") == 0) opType = "Child";

        HistoryTraceItem item;
        item.name = MappedName(data.substr(0, bestPos));
        item.id = idValue;
        item.operation = opType;
        chain.push_back(item);

        pos = bestPos;
    }

    return chain;
}

std::string ElementMap::extractBaseName(const MappedName& name) {
    const std::string& data = name.data();
    // Base name is everything before the first ";:"
    size_t pos = data.find(";:");
    if (pos == std::string::npos) return data;
    return data.substr(0, pos);
}

std::int64_t ElementMap::extractTag(const MappedName& name) {
    // Phase 3: v1-compat shim. Route through extractShapeIdentity +
    // encodeV1Scalar so v2 names with ;:P postfixes map back to the
    // scalar form correctly. encodeV1Scalar may throw overflow_error
    // for v2-only counters (> UINT32_MAX multi-doc); callers on the
    // legacy-serializer path must wrap this in try/catch per §4.3
    // rule 5.
    ShapeIdentity id = extractShapeIdentity(name);
    if (!id.isValid()) return 0;  // bare base name or malformed payload
    return encodeV1Scalar(id);
}

ShapeIdentity ElementMap::extractShapeIdentity(const MappedName& name,
                                               DiagnosticCollector* diag) {
    // Rightmost-marker algorithm — see docs/identity-model.md §4.3 rule 4.
    // Never throws; malformed payloads return {0,0} AND emit
    // MALFORMED_ELEMENT_NAME via `diag` when a marker is present but
    // its payload fails to parse. A bare base name (no marker at all)
    // is legitimate and does NOT trigger the warning.
    const std::string& data = name.data();
    const std::uint64_t hDoc = extractDocumentId(name);

    const size_t posP = data.rfind(ElementCodes::POSTFIX_IDENTITY);
    const size_t posH = data.rfind(ElementCodes::POSTFIX_TAG);
    const size_t posT = data.rfind(ElementCodes::POSTFIX_DECIMAL_TAG);

    // Find the LARGEST position — ties cannot occur (distinct markers).
    size_t bestPos = std::string::npos;
    enum { None, IdentP, HexH, DecT } kind = None;
    auto consider = [&](size_t p, auto k) {
        if (p != std::string::npos && (bestPos == std::string::npos || p > bestPos)) {
            bestPos = p;
            kind = k;
        }
    };
    consider(posP, IdentP);
    consider(posH, HexH);
    consider(posT, DecT);
    if (bestPos == std::string::npos) {
        // No marker at all — bare base name. Legitimate; no warning.
        return {};
    }

    // Helper: emit MALFORMED_ELEMENT_NAME for the current name. Called
    // at every site where a marker is present but its payload fails
    // to parse (rule 4d) or a v1 decode throws Case Error (rule 4e).
    const char* markerStr = (kind == IdentP) ? ";:P"
                         : (kind == HexH)   ? ";:H"
                                            : ";:T";
    auto emitMalformed = [&](const char* reason) {
        if (!diag) return;
        std::ostringstream msg;
        msg << "Malformed " << markerStr << " payload in element name \""
            << data << "\": " << reason;
        diag->warning(ErrorCode::MALFORMED_ELEMENT_NAME, msg.str());
    };

    const size_t valueStart = bestPos + 3;  // skip ";:P"/";:H"/";:T"

    if (kind == IdentP) {
        // 32 hex chars: 16 for documentId, then 16 for counter. No
        // separator (FreeCAD's validator rejects '.' in mapped names).
        if (valueStart + ElementCodes::POSTFIX_IDENTITY_HEX_LEN > data.size()) {
            emitMalformed("truncated payload (expected 32 hex chars)");
            return {};
        }
        auto parseHex16 = [&](size_t off, std::uint64_t& out) -> bool {
            std::uint64_t v = 0;
            for (size_t i = 0; i < 16; ++i) {
                char c = data[off + i];
                std::uint64_t d;
                if      (c >= '0' && c <= '9') d = static_cast<std::uint64_t>(c - '0');
                else if (c >= 'a' && c <= 'f') d = static_cast<std::uint64_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') d = static_cast<std::uint64_t>(c - 'A' + 10);
                else return false;
                v = (v << 4) | d;
            }
            out = v;
            return true;
        };
        ShapeIdentity id;
        if (!parseHex16(valueStart, id.documentId)) {
            emitMalformed("non-hex character in documentId field");
            return {};
        }
        if (!parseHex16(valueStart + 16, id.counter)) {
            emitMalformed("non-hex character in counter field");
            return {};
        }
        return id;
    }

    // v1 hex or decimal scalar.
    const int base = (kind == HexH) ? 16 : 10;
    size_t valueEnd = valueStart;
    while (valueEnd < data.size()) {
        const char c = data[valueEnd];
        const bool ok = (base == 16)
            ? std::isxdigit(static_cast<unsigned char>(c))
            : std::isdigit(static_cast<unsigned char>(c));
        if (!ok) break;
        ++valueEnd;
    }
    if (valueEnd == valueStart) {
        emitMalformed("empty numeric payload after marker");
        return {};
    }

    // Hex path: parse as unsigned so 64-bit patterns with high bit set
    // (multi-doc encoding where low32(docId) >= 0x80000000) round-trip
    // through strtoull without saturating at INT64_MAX. Reinterpret the
    // bit pattern as signed for decodeV1Scalar, which takes int64_t.
    // Decimal path stays signed — v1 ;:T scalars were always non-negative.
    std::int64_t scalar = 0;
    if (base == 16) {
        scalar = static_cast<std::int64_t>(std::strtoull(
            data.substr(valueStart, valueEnd - valueStart).c_str(),
            nullptr, 16));
    } else {
        scalar = std::strtoll(
            data.substr(valueStart, valueEnd - valueStart).c_str(),
            nullptr, 10);
    }
    // rule 4e: catch Error Case mismatches and funnel through the
    // malformed path. The parser never throws.
    try {
        return oreo::decodeV1Scalar(scalar, hDoc, nullptr);
    } catch (const std::invalid_argument&) {
        emitMalformed("hint/scalar mismatch between ;:H/;:T and ambient ;:Q");
        return {};
    }
}

std::uint64_t ElementMap::extractDocumentId(const MappedName& name) {
    const std::string& data = name.data();
    size_t docPos = data.rfind(ElementCodes::POSTFIX_DOC);
    if (docPos == std::string::npos) return 0;
    size_t valueStart = docPos + std::strlen(ElementCodes::POSTFIX_DOC);
    size_t valueEnd = valueStart;
    while (valueEnd < data.size() && std::isxdigit(data[valueEnd])) {
        ++valueEnd;
    }
    if (valueEnd == valueStart) return 0;
    return std::strtoull(
        data.substr(valueStart, valueEnd - valueStart).c_str(),
        nullptr, 16);
}

// ─── Serialization ───────────────────────────────────────────
//
// Wire format (FORMAT_VERSION == 3 — Phase 3 identity-model hardening):
//   [4 bytes] FORMAT_VERSION
//   [4 bytes] entry count
//   For each entry:
//     [ 2 bytes] type string length
//     [ N bytes] type string
//     [ 4 bytes] index (int32)
//     [ 4 bytes] mapped name length
//     [ M bytes] mapped name data
//     [16 bytes] identity { u64 documentId, u64 counter }   ← v3 change
//   [4 bytes] child count
//   For each child:
//     [16 bytes] child identity                              ← v3 change
//     [ 4 bytes] postfix length
//     [ N bytes] postfix data
//     [ 4 bytes] child map data length
//     [ M bytes] child map serialized data (recursive)
//
// FORMAT_VERSION == 2 is accepted for read-only compatibility: entries
// carry an 8-byte int64 tag which is decoded via decodeV1Scalar(scalar,
// rootHint) to reconstruct a ShapeIdentity. A LEGACY_IDENTITY_DOWNGRADE
// warning fires at most once per top-level deserialize (§5.4 dedup).

std::vector<std::uint8_t> ElementMap::serialize() const {
    std::vector<std::uint8_t> buf;

    auto writeU16 = [&](std::uint16_t v) {
        buf.push_back(static_cast<std::uint8_t>(v & 0xFF));
        buf.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    };
    auto writeU32 = [&](std::uint32_t v) {
        buf.push_back(static_cast<std::uint8_t>(v & 0xFF));
        buf.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
        buf.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
        buf.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
    };
    auto writeU64 = [&](std::uint64_t u) {
        for (int i = 0; i < 8; ++i)
            buf.push_back(static_cast<std::uint8_t>((u >> (i * 8)) & 0xFF));
    };
    auto writeIdentity = [&](ShapeIdentity id) {
        writeU64(id.documentId);
        writeU64(id.counter);
    };
    auto writeStr = [&](const std::string& s) {
        writeU32(static_cast<std::uint32_t>(s.size()));
        buf.insert(buf.end(), s.begin(), s.end());
    };

    // Header
    writeU32(FORMAT_VERSION);

    // Entries
    writeU32(static_cast<std::uint32_t>(indexToMapped_.size()));
    for (auto& [idx, name] : indexToMapped_) {
        writeU16(static_cast<std::uint16_t>(idx.type().size()));
        buf.insert(buf.end(), idx.type().begin(), idx.type().end());
        writeU32(static_cast<std::uint32_t>(idx.index()));
        writeStr(name.data());
        // Per-entry identity: extract from the encoded postfixes. No
        // scalar squeeze — the full 16 bytes go to disk.
        writeIdentity(extractShapeIdentity(name));
    }

    // Children
    writeU32(static_cast<std::uint32_t>(children_.size()));
    for (auto& child : children_) {
        writeIdentity(child.id);
        writeStr(child.postfix);
        if (child.map) {
            auto childData = child.map->serialize();
            writeU32(static_cast<std::uint32_t>(childData.size()));
            buf.insert(buf.end(), childData.begin(), childData.end());
        } else {
            writeU32(0);
        }
    }

    return buf;
}

ElementMapPtr ElementMap::deserialize(const std::uint8_t* data, size_t len,
                                      ShapeIdentity rootHint,
                                      DiagnosticCollector* diag) {
    if (!data || len < 8) return nullptr;

    size_t pos = 0;

    auto readU16 = [&]() -> std::uint16_t {
        if (pos + 2 > len) return 0;
        std::uint16_t v = static_cast<std::uint16_t>(data[pos])
                   | (static_cast<std::uint16_t>(data[pos+1]) << 8);
        pos += 2;
        return v;
    };
    auto readU32 = [&]() -> std::uint32_t {
        if (pos + 4 > len) return 0;
        std::uint32_t v = static_cast<std::uint32_t>(data[pos])
                   | (static_cast<std::uint32_t>(data[pos+1]) << 8)
                   | (static_cast<std::uint32_t>(data[pos+2]) << 16)
                   | (static_cast<std::uint32_t>(data[pos+3]) << 24);
        pos += 4;
        return v;
    };
    auto readU64 = [&]() -> std::uint64_t {
        if (pos + 8 > len) return 0;
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<std::uint64_t>(data[pos + i]) << (i * 8);
        pos += 8;
        return v;
    };
    auto readI64 = [&]() -> std::int64_t {
        return static_cast<std::int64_t>(readU64());
    };
    auto readIdentity = [&]() -> ShapeIdentity {
        ShapeIdentity id;
        id.documentId = readU64();
        id.counter    = readU64();
        return id;
    };
    auto readStr = [&]() -> std::string {
        std::uint32_t slen = readU32();
        if (pos + slen > len) return {};
        std::string s(reinterpret_cast<const char*>(data + pos), slen);
        pos += slen;
        return s;
    };

    // Header
    std::uint32_t version = readU32();
    if (version != FORMAT_VERSION && version != FORMAT_VERSION_V2_LEGACY) {
        // v1 (== 1) and anything else: rejected. v2 stays as a read-only
        // compat path; the writer always produces v3.
        return nullptr;
    }

    // Resolve the legacy-decode sink lazily — we only hand it to
    // decodeV1Scalar on v2 reads; v3 reads don't need it at all.
    // §5.4 dedup is implemented in the caller (oreo_serialize.cpp
    // manages the thread-local "already warned" flag); at the
    // ElementMap level we just always pass the sink down.
    DiagnosticCollector* v2Diag = (version == FORMAT_VERSION_V2_LEGACY) ? diag : nullptr;

    auto map = std::make_shared<ElementMap>();

    // Entries
    std::uint32_t entryCount = readU32();
    for (std::uint32_t i = 0; i < entryCount && pos < len; ++i) {
        std::uint16_t typeLen = readU16();
        if (pos + typeLen > len) break;
        std::string type(reinterpret_cast<const char*>(data + pos), typeLen);
        pos += typeLen;
        int index = static_cast<int>(readU32());
        std::string nameData = readStr();

        if (version == FORMAT_VERSION) {
            (void)readIdentity();  // consume 16 bytes; identity lives in the name
        } else {
            // v2: 8-byte int64 scalar. Decode with the root hint.
            std::int64_t scalar = readI64();
            try {
                (void)oreo::decodeV1Scalar(scalar, rootHint.documentId, v2Diag);
            } catch (const std::invalid_argument&) {
                // Error Case mid-buffer: hint/scalar mismatch means the
                // buffer is not this context's document. Bail out cleanly
                // — caller sees nullptr and treats it as DESERIALIZE_FAILED.
                return nullptr;
            }
        }
        map->setElementName(IndexedName(type, index), MappedName(nameData), /*tag=*/0);
    }

    // Children
    std::uint32_t childCount = readU32();
    for (std::uint32_t i = 0; i < childCount && pos < len; ++i) {
        ChildElementMap child;
        if (version == FORMAT_VERSION) {
            child.id = readIdentity();
        } else {
            std::int64_t scalar = readI64();
            try {
                child.id = oreo::decodeV1Scalar(scalar, rootHint.documentId, v2Diag);
            } catch (const std::invalid_argument&) {
                return nullptr;
            }
        }
        child.postfix = readStr();
        std::uint32_t childDataLen = readU32();
        if (childDataLen > 0 && pos + childDataLen <= len) {
            child.map = ElementMap::deserialize(data + pos, childDataLen,
                                                rootHint, v2Diag);
            pos += childDataLen;
        }
        map->addChild(child);
    }

    return map;
}

} // namespace oreo
