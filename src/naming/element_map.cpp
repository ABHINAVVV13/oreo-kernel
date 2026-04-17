// element_map.cpp — Production-grade ElementMap implementation.

#include "element_map.h"

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

void ElementMap::importChildNames(const ElementMap& childMap, std::int64_t childTag,
                                  const std::string& childPostfix) {
    // Import all mappings from child, re-encoding with child tag + postfix
    for (auto& [idx, name] : childMap.indexToMapped_) {
        MappedName importedName = encodeElementName(
            idx.type().c_str(), name, childTag,
            childPostfix.empty() ? ElementCodes::POSTFIX_CHILD : childPostfix.c_str());
        // Don't overwrite existing names — child names are lower priority
        if (!hasIndexedName(idx)) {
            setElementName(idx, importedName, childTag);
        }
    }
}

// ─── History encoding ────────────────────────────────────────

MappedName ElementMap::encodeElementName(
    const char* /*type*/,
    const MappedName& inputName,
    std::int64_t tag,
    const char* postfix)
{
    // Build: inputName + ";:H<tag_hex>" + postfix
    MappedName result(inputName);
    result.appendTag(tag);
    if (postfix && *postfix) {
        result.appendPostfix(postfix);
    }
    return result;
}

// ─── History tracing ─────────────────────────────────────────

std::vector<HistoryTraceItem> ElementMap::traceHistory(const MappedName& name) {
    std::vector<HistoryTraceItem> chain;
    const std::string& data = name.data();

    // Parse postfixes from right to left
    // Each segment is: ";:H<hex_tag>" followed by ";:M" or ";:G" etc.
    size_t pos = data.size();
    int maxDepth = 50;  // Prevent infinite loops on malformed names

    while (pos > 0 && maxDepth-- > 0) {
        // Find the last ";:H" or ";:T" tag marker
        size_t tagPos = data.rfind(ElementCodes::POSTFIX_TAG, pos - 1);
        size_t decTagPos = data.rfind(ElementCodes::POSTFIX_DECIMAL_TAG, pos - 1);

        size_t bestTagPos = std::string::npos;
        bool isHex = true;
        if (tagPos != std::string::npos && (decTagPos == std::string::npos || tagPos > decTagPos)) {
            bestTagPos = tagPos;
            isHex = true;
        } else if (decTagPos != std::string::npos) {
            bestTagPos = decTagPos;
            isHex = false;
        }

        if (bestTagPos == std::string::npos) break;

        // Extract the tag value
        size_t valueStart = bestTagPos + (isHex ? 3 : 3);  // Skip ";:H" or ";:T"
        std::int64_t tagValue = 0;
        size_t valueEnd = valueStart;

        if (isHex) {
            // Parse hex digits
            while (valueEnd < data.size() && std::isxdigit(data[valueEnd])) {
                ++valueEnd;
            }
            if (valueEnd > valueStart) {
                // strtoll to preserve a full 64-bit tag on Windows (where
                // strtol returns 32-bit).
                tagValue = std::strtoll(
                    data.substr(valueStart, valueEnd - valueStart).c_str(),
                    nullptr, 16);
            }
        } else {
            // Parse decimal digits
            while (valueEnd < data.size() && std::isdigit(data[valueEnd])) {
                ++valueEnd;
            }
            if (valueEnd > valueStart) {
                tagValue = std::strtoll(
                    data.substr(valueStart, valueEnd - valueStart).c_str(),
                    nullptr, 10);
            }
        }

        // Determine the operation type from the postfix after the tag
        std::string opType = "Unknown";
        size_t afterTag = valueEnd;
        if (data.compare(afterTag, 3, ";:M") == 0) opType = "Modified";
        else if (data.compare(afterTag, 3, ";:G") == 0) opType = "Generated";
        else if (data.compare(afterTag, 4, ";:MG") == 0) opType = "ModifiedGenerated";
        else if (data.compare(afterTag, 3, ";:C") == 0) opType = "Child";

        HistoryTraceItem item;
        item.name = MappedName(data.substr(0, bestTagPos));
        item.tag = tagValue;
        item.operation = opType;
        chain.push_back(item);

        pos = bestTagPos;
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
    const std::string& data = name.data();
    // Find the last ";:H" tag. Note: the ;:Q document-id postfix uses a
    // distinct marker and does not participate in tag extraction, so rfind
    // on ";:H" is unambiguous.
    size_t tagPos = data.rfind(ElementCodes::POSTFIX_TAG);
    if (tagPos == std::string::npos) return 0;

    size_t valueStart = tagPos + 3;
    size_t valueEnd = valueStart;
    while (valueEnd < data.size() && std::isxdigit(data[valueEnd])) {
        ++valueEnd;
    }
    if (valueEnd == valueStart) return 0;
    // strtoll → 64-bit parse; fixes Windows truncation where strtol returned
    // only the low 32 bits of a multi-document tag.
    return std::strtoll(
        data.substr(valueStart, valueEnd - valueStart).c_str(),
        nullptr, 16);
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

// Wire format (FORMAT_VERSION == 2):
//   [4 bytes] FORMAT_VERSION
//   [4 bytes] entry count
//   For each entry:
//     [2 bytes] type string length
//     [N bytes] type string
//     [4 bytes] index (int32)
//     [4 bytes] mapped name length
//     [M bytes] mapped name data
//     [8 bytes] tag (int64)     ← full 64-bit, no Windows truncation
//   [4 bytes] child count
//   For each child:
//     [8 bytes] child tag (int64)
//     [4 bytes] postfix length
//     [N bytes] postfix data
//     [4 bytes] child map data length
//     [M bytes] child map serialized data (recursive)

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
    auto writeI64 = [&](std::int64_t v) {
        // Little-endian 64-bit — stable across MSVC / GCC / Clang. Treat as
        // unsigned to avoid implementation-defined right-shift on negative
        // values (C++20 actually mandates arithmetic shift, but the cast is
        // clearer and keeps behavior obvious to future readers).
        std::uint64_t u = static_cast<std::uint64_t>(v);
        for (int i = 0; i < 8; ++i)
            buf.push_back(static_cast<std::uint8_t>((u >> (i * 8)) & 0xFF));
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
        // Retrieve tag: extract from the name's encoded postfixes
        writeI64(extractTag(name));
    }

    // Children
    writeU32(static_cast<std::uint32_t>(children_.size()));
    for (auto& child : children_) {
        writeI64(child.tag);
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

ElementMapPtr ElementMap::deserialize(const std::uint8_t* data, size_t len) {
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
    auto readI64 = [&]() -> std::int64_t {
        if (pos + 8 > len) return 0;
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<std::uint64_t>(data[pos + i]) << (i * 8);
        pos += 8;
        return static_cast<std::int64_t>(v);
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
    if (version != FORMAT_VERSION) return nullptr;

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
        // Tag is already 64-bit — no narrowing cast, which was the Windows
        // truncation bug before this audit.
        std::int64_t tag = readI64();
        map->setElementName(IndexedName(type, index), MappedName(nameData), tag);
    }

    // Children
    std::uint32_t childCount = readU32();
    for (std::uint32_t i = 0; i < childCount && pos < len; ++i) {
        ChildElementMap child;
        // 64-bit read, stored directly into the now-int64 tag field.
        child.tag = readI64();
        child.postfix = readStr();
        std::uint32_t childDataLen = readU32();
        if (childDataLen > 0 && pos + childDataLen <= len) {
            child.map = ElementMap::deserialize(data + pos, childDataLen);
            pos += childDataLen;
        }
        map->addChild(child);
    }

    return map;
}

} // namespace oreo
