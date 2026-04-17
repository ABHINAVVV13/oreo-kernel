// oreo_serialize.cpp — Binary serialization implementation.

#include "oreo_serialize.h"
#include "core/oreo_error.h"
#include "core/operation_result.h"
#include "core/diagnostic_scope.h"
#include "core/occt_try.h"
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

    // Serialize element map
    std::vector<std::uint8_t> mapData;
    if (shape.elementMap()) {
        mapData = shape.elementMap()->serialize();
    }

    // Write: [version:u8][brep_len:u32][brep][map_len:u32][map][tag:i64]
    writeU8(buf, OREO_SERIALIZE_FORMAT_VERSION);
    writeU32(buf, static_cast<std::uint32_t>(brepData.size()));
    buf.insert(buf.end(), brepData.begin(), brepData.end());
    writeU32(buf, static_cast<std::uint32_t>(mapData.size()));
    buf.insert(buf.end(), mapData.begin(), mapData.end());
    writeI64(buf, shape.tag());

    return scope.makeResult(std::move(buf));
}

OperationResult<NamedShape> deserialize(KernelContext& ctx, const std::uint8_t* data, std::size_t len) {
    DiagnosticScope scope(ctx);

    // Minimum size: version(1) + brep_len(4) + map_len(4) + tag(8) = 17 bytes.
    // An empty BRep is rejected downstream, so the BRep portion is non-zero
    // in practice, but we only need to guarantee the header is readable.
    if (!data || len < 1 + 4 + 4 + 8) {
        ctx.diag().error(ErrorCode::DESERIALIZE_FAILED, "Buffer too small for deserialization");
        return scope.makeFailure<NamedShape>();
    }

    OREO_OCCT_TRY
    std::size_t pos = 0;

    // Version gate — this is the fix for audit finding M4. Buffers without
    // a version byte (or with an unknown version) are rejected so that the
    // pre-audit truncated-tag format is not silently accepted.
    std::uint8_t version = readU8(data, pos, len);
    if (version != OREO_SERIALIZE_FORMAT_VERSION) {
        ctx.diag().error(ErrorCode::DESERIALIZE_FAILED,
            "Unsupported serialization format version: got "
            + std::to_string(static_cast<int>(version))
            + ", expected "
            + std::to_string(static_cast<int>(OREO_SERIALIZE_FORMAT_VERSION)));
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

    // Read element map
    std::uint32_t mapLen = readU32(data, pos, len);
    ElementMapPtr map;
    if (mapLen > 0 && pos + mapLen <= len) {
        map = ElementMap::deserialize(data + pos, mapLen);
        pos += mapLen;
    }

    // Read tag — 64-bit throughout. This was previously cast to `long` which
    // truncated the high 32 bits on Windows MSVC. NamedShape's constructor
    // accepts int64_t directly, so no cast is needed.
    std::int64_t tag = readI64(data, pos, len);

    return scope.makeResult(NamedShape(shape, map, tag));
    OREO_OCCT_CATCH_NS(scope, ctx, "deserialize")
}

} // namespace oreo
