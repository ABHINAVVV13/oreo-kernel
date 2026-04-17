// oreo_serialize.cpp — Binary serialization implementation.

#include "oreo_serialize.h"
#include "core/oreo_error.h"
#include "core/operation_result.h"
#include "core/diagnostic_scope.h"
#include "core/occt_try.h"
#include "core/shape_identity.h"
#include "core/shape_identity_v1.h"
#include "naming/element_map.h"

#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Shape.hxx>

#include <cstdint>
#include <sstream>

namespace oreo {

namespace {

void writeU8(std::vector<std::uint8_t>& buf, std::uint8_t v) {
    buf.push_back(v);
}

void writeU32(std::vector<std::uint8_t>& buf, std::uint32_t v) {
    buf.push_back(static_cast<std::uint8_t>(v & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

void writeI64(std::vector<std::uint8_t>& buf, std::int64_t v) {
    // Cast to unsigned before shifting: right-shift on a signed value is
    // implementation-defined prior to C++20 and the unsigned form makes the
    // intent obvious even to readers not tracking the standard.
    std::uint64_t u = static_cast<std::uint64_t>(v);
    for (int i = 0; i < 8; ++i) {
        buf.push_back(static_cast<std::uint8_t>((u >> (i * 8)) & 0xFF));
    }
}

void writeU64(std::vector<std::uint8_t>& buf, std::uint64_t u) {
    for (int i = 0; i < 8; ++i) {
        buf.push_back(static_cast<std::uint8_t>((u >> (i * 8)) & 0xFF));
    }
}

void writeIdentity(std::vector<std::uint8_t>& buf, ShapeIdentity id) {
    writeU64(buf, id.documentId);
    writeU64(buf, id.counter);
}

std::uint8_t readU8(const std::uint8_t* data, std::size_t& pos, std::size_t len) {
    if (pos + 1 > len) return 0;
    return data[pos++];
}

std::uint32_t readU32(const std::uint8_t* data, std::size_t& pos, std::size_t len) {
    if (pos + 4 > len) return 0;
    std::uint32_t v = static_cast<std::uint32_t>(data[pos])
               | (static_cast<std::uint32_t>(data[pos+1]) << 8)
               | (static_cast<std::uint32_t>(data[pos+2]) << 16)
               | (static_cast<std::uint32_t>(data[pos+3]) << 24);
    pos += 4;
    return v;
}

std::int64_t readI64(const std::uint8_t* data, std::size_t& pos, std::size_t len) {
    if (pos + 8 > len) return 0;
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(data[pos + i]) << (i * 8);
    }
    pos += 8;
    return static_cast<std::int64_t>(v);
}

std::uint64_t readU64(const std::uint8_t* data, std::size_t& pos, std::size_t len) {
    if (pos + 8 > len) return 0;
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(data[pos + i]) << (i * 8);
    }
    pos += 8;
    return v;
}

ShapeIdentity readIdentity(const std::uint8_t* data, std::size_t& pos, std::size_t len) {
    ShapeIdentity id;
    id.documentId = readU64(data, pos, len);
    id.counter    = readU64(data, pos, len);
    return id;
}

} // anonymous namespace

OperationResult<std::vector<std::uint8_t>> serialize(KernelContext& ctx, const NamedShape& shape) {
    DiagnosticScope scope(ctx);
    std::vector<std::uint8_t> buf;

    if (shape.isNull()) {
        ctx.diag().error(ErrorCode::SERIALIZE_FAILED, "Cannot serialize null shape");
        return scope.makeFailure<std::vector<std::uint8_t>>();
    }

    // Serialize BRep
    std::ostringstream brepStream;
    BRepTools::Write(shape.shape(), brepStream);
    std::string brepData = brepStream.str();

    // Serialize element map (writes with ElementMap::FORMAT_VERSION=3).
    std::vector<std::uint8_t> mapData;
    if (shape.elementMap()) {
        mapData = shape.elementMap()->serialize();
    }

    // Native v2: NamedShape stores ShapeIdentity directly, so the
    // root identity goes to disk without any v1 scalar round-trip.
    // No squeeze, no Case B warnings, no UINT32_MAX ceiling on the
    // write path.
    const ShapeIdentity rootId = shape.shapeId();

    // Write: [version=3:u8][brep_len:u32][brep][map_len:u32][map][identity:16]
    writeU8(buf, OREO_SERIALIZE_FORMAT_VERSION);
    writeU32(buf, static_cast<std::uint32_t>(brepData.size()));
    buf.insert(buf.end(), brepData.begin(), brepData.end());
    writeU32(buf, static_cast<std::uint32_t>(mapData.size()));
    buf.insert(buf.end(), mapData.begin(), mapData.end());
    writeIdentity(buf, rootId);

    return scope.makeResult(std::move(buf));
}

OperationResult<NamedShape> deserialize(KernelContext& ctx, const std::uint8_t* data, std::size_t len) {
    DiagnosticScope scope(ctx);

    // Coalesce LEGACY_IDENTITY_DOWNGRADE warnings for this top-level
    // call: inner decodeV1Scalar sites (including the ones inside
    // ElementMap::deserialize) share the scope and emit at most one
    // warning total. See docs/identity-model.md §5.4.
    oreo::internal::LegacyDowngradeDedupScope dedupScope;

    // Minimum size — depends on version. For v3 (current): version(1) +
    // brep_len(4) + map_len(4) + identity(16) = 25. For v1 (legacy): 17.
    // Accept the smaller of the two here; version-specific sizing is
    // enforced below once the version byte is read.
    if (!data || len < 1 + 4 + 4 + 8) {
        ctx.diag().error(ErrorCode::DESERIALIZE_FAILED, "Buffer too small for deserialization");
        return scope.makeFailure<NamedShape>();
    }

    OREO_OCCT_TRY
    std::size_t pos = 0;

    // Version gate — Phase 4 accepts v3 (current) and v1 (legacy compat).
    // v2 is deliberately illegal at the outer layer and is rejected.
    std::uint8_t version = readU8(data, pos, len);
    const bool isV3 = (version == OREO_SERIALIZE_FORMAT_VERSION);
    const bool isV1 = (version == OREO_SERIALIZE_FORMAT_VERSION_V1_LEGACY);
    if (!isV3 && !isV1) {
        ctx.diag().error(ErrorCode::DESERIALIZE_FAILED,
            "Unsupported serialization format version: got "
            + std::to_string(static_cast<int>(version))
            + ", expected "
            + std::to_string(static_cast<int>(OREO_SERIALIZE_FORMAT_VERSION))
            + " or " + std::to_string(static_cast<int>(OREO_SERIALIZE_FORMAT_VERSION_V1_LEGACY)));
        return scope.makeFailure<NamedShape>();
    }

    // Read BRep
    std::uint32_t brepLen = readU32(data, pos, len);
    if (pos + brepLen > len) {
        ctx.diag().error(ErrorCode::DESERIALIZE_FAILED, "BRep data truncated");
        return scope.makeFailure<NamedShape>();
    }

    std::string brepData(reinterpret_cast<const char*>(data + pos), brepLen);
    pos += brepLen;

    std::istringstream brepStream(brepData);
    BRep_Builder builder;
    TopoDS_Shape shape;
    // BRepTools::Read can throw Standard_Failure on malformed BRep — caught by
    // OREO_OCCT_CATCH_NS below.
    BRepTools::Read(shape, brepStream, builder);
    if (shape.IsNull()) {
        ctx.diag().error(ErrorCode::DESERIALIZE_FAILED, "BRep deserialization produced null shape");
        return scope.makeFailure<NamedShape>();
    }

    // Read element map. v3 inner reader needs no hint (identities are
    // carried inline). v1 buffers carry a v2 inner map whose per-entry
    // scalars need the context's documentId as hint — pass it through.
    std::uint32_t mapLen = readU32(data, pos, len);
    ElementMapPtr map;
    if (mapLen > 0 && pos + mapLen <= len) {
        const ShapeIdentity rootHintForInner{ctx.tags().documentId(), 0};
        map = ElementMap::deserialize(data + pos, mapLen,
                                      /*rootHint=*/rootHintForInner,
                                      /*diag=*/&ctx.diag());
        pos += mapLen;
    }

    // Read root identity. Native v2: NamedShape stores a ShapeIdentity
    // directly, so the v3 path doesn't encode-then-fail — it just
    // hands the identity to the constructor. The v2-only overflow
    // path (counter > UINT32_MAX) that used to surface
    // V2_IDENTITY_NOT_REPRESENTABLE here is gone: the wire format
    // and in-memory form agree on the same 16 bytes.
    ShapeIdentity rootId{};
    if (isV3) {
        if (pos + 16 > len) {
            ctx.diag().error(ErrorCode::DESERIALIZE_FAILED,
                "Identity tail truncated (v3 expects 16 bytes)");
            return scope.makeFailure<NamedShape>();
        }
        rootId = readIdentity(data, pos, len);
    } else {
        // v1: 8-byte int64 scalar at the tail. Decode with the context's
        // documentId as hint. Error Case (mismatched hint) is caught
        // and translated to DESERIALIZE_FAILED — this is a genuine
        // cross-document paste or tampered buffer, not a recoverable
        // state.
        if (pos + 8 > len) {
            ctx.diag().error(ErrorCode::DESERIALIZE_FAILED,
                "Tag tail truncated (v1 expects 8 bytes)");
            return scope.makeFailure<NamedShape>();
        }
        const std::int64_t legacyScalar = readI64(data, pos, len);
        try {
            rootId = oreo::decodeV1Scalar(legacyScalar,
                                          ctx.tags().documentId(),
                                          &ctx.diag());
        } catch (const std::invalid_argument& e) {
            ctx.diag().error(ErrorCode::DESERIALIZE_FAILED,
                std::string("Legacy buffer docId mismatch: ") + e.what());
            return scope.makeFailure<NamedShape>();
        }
    }

    return scope.makeResult(NamedShape(shape, map, rootId));
    OREO_OCCT_CATCH_NS(scope, ctx, "deserialize")
}

} // namespace oreo
