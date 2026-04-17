// oreo_serialize.h — Binary serialization for NamedShape (BRep + element map).
//
// Every operation takes a KernelContext& as its first parameter.
//
// Wire format (current version = 3 — identity-model v2 hardening):
//
//   [format_version : u8 = 3]
//   [brep_len       : u32]
//   [brep_data      : brep_len bytes]
//   [map_len        : u32]
//   [map_data       : map_len bytes]   <- uses the inner ElementMap format (v3)
//   [root_identity  : {u64 documentId, u64 counter}]  <- 16 bytes
//
// Legacy reader (version = 1) is kept for backward compatibility with
// buffers produced before Phase 4. v1 had an 8-byte int64 tag at the
// tail; the v1 reader decodes it via decodeV1Scalar with the calling
// context's documentId as hint. See docs/identity-model.md §5.3.
// Version 2 is RESERVED/ILLEGAL at the outer layer and is rejected —
// it was never written to disk; skipping to 3 keeps the outer and
// inner (ElementMap) FORMAT_VERSION numbers unified.

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
constexpr std::uint8_t OREO_SERIALIZE_FORMAT_VERSION = 3;

// Legacy version the reader still accepts (read-only).
constexpr std::uint8_t OREO_SERIALIZE_FORMAT_VERSION_V1_LEGACY = 1;

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
