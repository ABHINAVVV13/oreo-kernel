// oreo_serialize.h — Binary serialization for NamedShape (BRep + element map).
//
// Every operation takes a KernelContext& as its first parameter.
// Every operation takes a KernelContext& as its first parameter.

#ifndef OREO_SERIALIZE_H
#define OREO_SERIALIZE_H

#include "core/kernel_context.h"
#include "core/operation_result.h"
#include "core/diagnostic_scope.h"
#include "naming/named_shape.h"

#include <cstdint>
#include <vector>

namespace oreo {

// ═══════════════════════════════════════════════════════════════
// Context-aware serialization functions
// ═══════════════════════════════════════════════════════════════

// Serialize a NamedShape to a binary buffer.
// Format: [BRep data length (4 bytes)] [BRep data] [ElementMap data length (4 bytes)] [ElementMap data] [tag (8 bytes)]
OperationResult<std::vector<uint8_t>> serialize(KernelContext& ctx, const NamedShape& shape);

// Deserialize a NamedShape from a binary buffer.
OperationResult<NamedShape> deserialize(KernelContext& ctx, const uint8_t* data, size_t len);

} // namespace oreo

#endif // OREO_SERIALIZE_H
