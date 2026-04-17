// map_shape_elements.cpp — Bridge between geometry operations and the real
// FreeCAD element-mapping algorithm.
//
// Context-aware: receives KernelContext& for tag allocation and diagnostics.
// Phase 3 identity-v2: opId is a full ShapeIdentity, not a squeezed int64.

#include "map_shape_elements.h"
#include "topo_shape_adapter.h"

#include "core/shape_identity_v1.h"

#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopAbs_ShapeEnum.hxx>

namespace oreo {

namespace {

// Populate default IndexedName-based mapped names for every sub-shape of
// `result`. Used when there are no input shapes to trace history from —
// primitive operations (makeBox, makeCylinder, …). Without this, the
// legacy FreeCAD buildElementMap path early-returns on empty input and
// leaves the element map empty, which means element accessors would
// return "". Each name carries a ;:P<16hex>.<16hex> identity postfix
// (the v2 canonical carrier). No separate ;:Q postfix is emitted —
// ;:P carries the full documentId inline.
NamedShape buildPrimitiveNames(
    const TopoDS_Shape& result,
    ShapeIdentity opId)
{
    auto map = std::make_shared<ElementMap>();

    struct TypeInfo {
        TopAbs_ShapeEnum occt;
        const char* name;
    };
    constexpr TypeInfo kTypes[] = {
        {TopAbs_VERTEX, "Vertex"},
        {TopAbs_EDGE,   "Edge"},
        {TopAbs_FACE,   "Face"},
    };

    for (const auto& t : kTypes) {
        TopTools_IndexedMapOfShape subMap;
        TopExp::MapShapes(result, t.occt, subMap);
        for (int i = 1; i <= subMap.Extent(); ++i) {
            IndexedName idx(t.name, i);
            MappedName name(std::string(t.name) + std::to_string(i));
            // v2 canonical carrier: ";:P<16hex>.<16hex>". Full
            // documentId + counter in one postfix; no separate ;:Q.
            name.appendShapeIdentity(opId);
            map->setElementName(idx, name, /*tag=*/0);
        }
    }

    return NamedShape(result, map, opId);
}

} // anonymous namespace

NamedShape mapShapeElements(
    KernelContext& ctx,
    const TopoDS_Shape& resultShape,
    const ShapeMapper& mapper,
    const std::vector<NamedShape>& inputShapes,
    ShapeIdentity opId,
    const char* opName)
{
    if (resultShape.IsNull()) {
        auto map = std::make_shared<ElementMap>();
        return NamedShape(resultShape, map, opId);
    }

    // Primitive op: no inputs to trace history from. The FreeCAD
    // buildElementMap algorithm early-returns in this case, so if we let it
    // run the resulting element map would be empty and element accessors
    // would return "". Generate default IndexedName-based names instead.
    if (inputShapes.empty()) {
        (void)mapper;  // unused in the primitive path
        (void)opName;
        return buildPrimitiveNames(resultShape, opId);
    }

    // Convert inputs to adapters.
    auto inputAdapters = TopoShapeAdapter::fromNamedShapes(inputShapes);

    // Run the full FreeCAD algorithm. The FreeCAD extraction's internal
    // contract still uses int64 for master tags (adapter.Tag), so we
    // pass encodeV1Scalar(opId) — lossless for identities the allocator
    // produces (counter ≤ INT64_MAX in single-doc, ≤ UINT32_MAX in
    // multi-doc). The full documentId goes through documentId_ so any
    // names the adapter emits can stamp the full 64-bit identity. The
    // NamedShape we return carries opId NATIVELY — we don't re-decode.
    TopoShapeAdapter adapter;
    adapter.Tag = (opId.counter == 0) ? 0 : oreo::encodeV1Scalar(opId);
    adapter.buildElementMap(resultShape, mapper, inputAdapters, opName,
                            opId.documentId);

    // toNamedShape reconstructs a ShapeIdentity from adapter.Tag +
    // documentId_. Re-wrap with the authoritative opId so callers see
    // the exact identity the allocator handed out, not a round-trip.
    NamedShape ns = adapter.toNamedShape();
    return NamedShape(ns.shape(), ns.elementMap(), opId);
}

} // namespace oreo
