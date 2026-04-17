// SPDX-License-Identifier: LGPL-2.1-or-later

// named_shape.h — TopoDS_Shape + ElementMap: the core type for oreo-kernel.
// Every geometry operation returns a NamedShape.
//
// A NamedShape carries:
//   1. The OCCT TopoDS_Shape (the actual geometry)
//   2. An ElementMap (stable names for all sub-shapes)
//   3. A ShapeIdentity (full 64+64 identity of the op that produced it)
//
// Phase 3 migration (identity-model v2 hardening): the identity was
// previously stored as an int64_t tag that squeezed multi-doc documentId
// into 32 bits. It is now a full ShapeIdentity. The legacy int64 ctors
// and tag() accessor remain as [[deprecated]] shims routed through
// encodeV1Scalar / decodeV1Scalar; they still work for single-doc and
// for multi-doc with counter ≤ UINT32_MAX, but throw on v2-only
// counters that don't fit the v1 scalar format.

#ifndef OREO_NAMED_SHAPE_H
#define OREO_NAMED_SHAPE_H

#include "core/shape_identity.h"
#include "core/shape_identity_v1.h"
#include "element_map.h"
#include "core/thread_safety.h"

#include <TopoDS_Shape.hxx>
#include <TopExp_Explorer.hxx>
#include <TopAbs_ShapeEnum.hxx>

#include <cstdint>
#include <memory>
#include <vector>

namespace oreo {

// NamedShape is context-bound: the underlying TopoDS_Shape and ElementMap
// share OCCT's process-global state (BRep allocators, handle tables), which
// means a NamedShape must not be mutated from multiple threads concurrently,
// and should not be shared across KernelContexts. Reading concurrently is
// safe; any mutation (including destruction of the last copy that owns the
// shared ElementMap) must be serialized. See src/core/thread_safety.h.
class OREO_CONTEXT_BOUND NamedShape {
public:
    NamedShape() = default;

    // Primary constructors — Phase 3.
    explicit NamedShape(const TopoDS_Shape& shape, ShapeIdentity id = {})
        : shape_(shape)
        , elementMap_(std::make_shared<ElementMap>())
        , shapeId_(id)
    {}

    NamedShape(const TopoDS_Shape& shape, ElementMapPtr map, ShapeIdentity id)
        : shape_(shape)
        , elementMap_(map ? map : std::make_shared<ElementMap>())
        , shapeId_(id)
    {}

    // Deprecated v1 constructors — accept an int64 scalar tag and
    // decode it via decodeV1Scalar (no hint, so high-32 bits of docId
    // are the squeezed low-32 of the originating context). Preserved
    // for source compatibility during the identity-v2 migration.
    [[deprecated("pass ShapeIdentity — the int64 scalar constructor decodes "
                 "via decodeV1Scalar(tag, 0) and loses full documentId high bits")]]
    NamedShape(const TopoDS_Shape& shape, int64_t legacyTag)
        : shape_(shape)
        , elementMap_(std::make_shared<ElementMap>())
        , shapeId_(legacyTag == 0
                   ? ShapeIdentity{}
                   : oreo::decodeV1Scalar(legacyTag, /*hint=*/0, nullptr))
    {}

    [[deprecated("pass ShapeIdentity — the int64 scalar constructor decodes "
                 "via decodeV1Scalar(tag, 0) and loses full documentId high bits")]]
    NamedShape(const TopoDS_Shape& shape, ElementMapPtr map, int64_t legacyTag)
        : shape_(shape)
        , elementMap_(map ? map : std::make_shared<ElementMap>())
        , shapeId_(legacyTag == 0
                   ? ShapeIdentity{}
                   : oreo::decodeV1Scalar(legacyTag, /*hint=*/0, nullptr))
    {}

    // --- Accessors ---

    const TopoDS_Shape& shape() const { return shape_; }
    TopoDS_Shape& shape() { return shape_; }

    ElementMapPtr elementMap() const { return elementMap_; }

    // Primary identity accessor — full 64+64.
    ShapeIdentity shapeId() const { return shapeId_; }

    // Deprecated v1 accessor. Returns 0 for the invalid-sentinel
    // identity (matches pre-v2 default-construction behavior), else
    // encodeV1Scalar which may throw std::overflow_error when the
    // identity's counter exceeds what a v1 scalar can hold.
    [[deprecated("use shapeId() — tag() throws on v2 counters that don't fit "
                 "the v1 int64 scalar format (multi-doc > UINT32_MAX)")]]
    int64_t tag() const {
        if (shapeId_.counter == 0) return 0;
        return oreo::encodeV1Scalar(shapeId_);
    }

    bool isNull() const { return shape_.IsNull(); }

    // --- Sub-shape queries ---

    // Count sub-shapes of a given type
    int countSubShapes(TopAbs_ShapeEnum type) const;

    // Get the i-th sub-shape (1-based, matching OCCT convention)
    TopoDS_Shape getSubShape(TopAbs_ShapeEnum type, int index) const;

    // Get the stable name for an indexed sub-shape
    MappedName getElementName(const IndexedName& index) const;

    // Get the indexed name for a stable name
    IndexedName getIndexedName(const MappedName& name) const;

private:
    TopoDS_Shape shape_;
    ElementMapPtr elementMap_ = std::make_shared<ElementMap>();
    ShapeIdentity shapeId_ {};
};

// Convenience: list of named edges for fillet/chamfer input
struct NamedEdge {
    IndexedName name;
    TopoDS_Shape edge;  // The actual TopoDS_Edge
};

// Convenience: list of named faces for shell input
struct NamedFace {
    IndexedName name;
    TopoDS_Shape face;  // The actual TopoDS_Face
};

} // namespace oreo

#endif // OREO_NAMED_SHAPE_H
