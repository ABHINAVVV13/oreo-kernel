// oreo_serialize.h — Binary serialization for NamedShape (BRep + element map).
//
// Every operation takes a KernelContext& as its first parameter.
//
// Wire format (current version = 1):
//
//   [format_version : u8 = 1]
//   [brep_len       : u32]
//   [brep_data      : brep_len bytes]
//   [map_len        : u32]
//   [map_data       : map_len bytes]   <- uses the inner ElementMap format
//   [tag            : i64]
//
// The u8 version byte is new in the April 2026 foundation-audit hardening
// pass. Buffers without a version byte (pre-audit files) deserialize as if
// they have version 0 — which is not equal to OREO_SERIALIZE_FORMAT and is
// rejected with DESERIALIZE_FAILED. This is intentional: the pre-audit
// format truncated int64 tags to 32 bits on Windows (long=32), so silently
// accepting those files would return tags whose high bits had been zeroed.

#ifndef OREO_SERIALIZE_H
#define OREO_SERIALIZE_H

#include "core/kernel_context.h"
#include "core/operation_result.h"
#include "core/diagnostic_scope.h"
#include "naming/named_shape.h"

#include <cstdint>
#include <vector>

namespace oreo {

// Current on-disk format version. Bump when any byte in the layout moves.
// See the file header for the full wire layout.
constexpr std::uint8_t OREO_SERIALIZE_FORMAT_VERSION = 1;

// ═══════════════════════════════════════════════════════════════
// Context-aware serialization functions
// ═══════════════════════════════════════════════════════════════

// Serialize a NamedShape to a binary buffer. See the header file comment
// for the wire layout.
OperationResult<std::vector<std::uint8_t>> serialize(KernelContext& ctx, const NamedShape& shape);

// Deserialize a NamedShape from a binary buffer. Rejects buffers whose
// first byte is not OREO_SERIALIZE_FORMAT_VERSION with DESERIALIZE_FAILED.
OperationResult<NamedShape> deserialize(KernelContext& ctx, const std::uint8_t* data, std::size_t len);

} // namespace oreo

#endif // OREO_SERIALIZE_H
