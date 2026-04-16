// oreo_serialize.cpp — Binary serialization implementation.

#include "oreo_serialize.h"
#include "core/oreo_error.h"
#include "core/operation_result.h"
#include "core/diagnostic_scope.h"
#include "naming/element_map.h"

#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Shape.hxx>

#include <sstream>

namespace oreo {

namespace {

void writeU32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void writeI64(std::vector<uint8_t>& buf, int64_t v) {
    for (int i = 0; i < 8; ++i) {
        buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    }
}

uint32_t readU32(const uint8_t* data, size_t& pos, size_t len) {
    if (pos + 4 > len) return 0;
    uint32_t v = static_cast<uint32_t>(data[pos])
               | (static_cast<uint32_t>(data[pos+1]) << 8)
               | (static_cast<uint32_t>(data[pos+2]) << 16)
               | (static_cast<uint32_t>(data[pos+3]) << 24);
    pos += 4;
    return v;
}

int64_t readI64(const uint8_t* data, size_t& pos, size_t len) {
    if (pos + 8 > len) return 0;
    int64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<int64_t>(data[pos + i]) << (i * 8);
    }
    pos += 8;
    return v;
}

} // anonymous namespace

OperationResult<std::vector<uint8_t>> serialize(KernelContext& ctx, const NamedShape& shape) {
    DiagnosticScope scope(ctx);
    std::vector<uint8_t> buf;

    if (shape.isNull()) {
        ctx.diag.error(ErrorCode::SERIALIZE_FAILED, "Cannot serialize null shape");
        return scope.makeFailure<std::vector<uint8_t>>();
    }

    // Serialize BRep
    std::ostringstream brepStream;
    BRepTools::Write(shape.shape(), brepStream);
    std::string brepData = brepStream.str();

    // Serialize element map
    std::vector<uint8_t> mapData;
    if (shape.elementMap()) {
        mapData = shape.elementMap()->serialize();
    }

    // Write: [brep_len][brep_data][map_len][map_data][tag]
    writeU32(buf, static_cast<uint32_t>(brepData.size()));
    buf.insert(buf.end(), brepData.begin(), brepData.end());
    writeU32(buf, static_cast<uint32_t>(mapData.size()));
    buf.insert(buf.end(), mapData.begin(), mapData.end());
    writeI64(buf, shape.tag());

    return scope.makeResult(std::move(buf));
}

OperationResult<NamedShape> deserialize(KernelContext& ctx, const uint8_t* data, size_t len) {
    DiagnosticScope scope(ctx);

    if (!data || len < 12) {
        ctx.diag.error(ErrorCode::DESERIALIZE_FAILED, "Buffer too small for deserialization");
        return scope.makeFailure<NamedShape>();
    }

    size_t pos = 0;

    // Read BRep
    uint32_t brepLen = readU32(data, pos, len);
    if (pos + brepLen > len) {
        ctx.diag.error(ErrorCode::DESERIALIZE_FAILED, "BRep data truncated");
        return scope.makeFailure<NamedShape>();
    }

    std::string brepData(reinterpret_cast<const char*>(data + pos), brepLen);
    pos += brepLen;

    std::istringstream brepStream(brepData);
    BRep_Builder builder;
    TopoDS_Shape shape;
    BRepTools::Read(shape, brepStream, builder);
    if (shape.IsNull()) {
        ctx.diag.error(ErrorCode::DESERIALIZE_FAILED, "BRep deserialization produced null shape");
        return scope.makeFailure<NamedShape>();
    }

    // Read element map
    uint32_t mapLen = readU32(data, pos, len);
    ElementMapPtr map;
    if (mapLen > 0 && pos + mapLen <= len) {
        map = ElementMap::deserialize(data + pos, mapLen);
        pos += mapLen;
    }

    // Read tag
    long tag = static_cast<long>(readI64(data, pos, len));

    return scope.makeResult(NamedShape(shape, map, tag));
}

} // namespace oreo
