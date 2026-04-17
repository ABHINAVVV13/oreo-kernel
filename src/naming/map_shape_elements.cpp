// map_shape_elements.cpp — Bridge between geometry operations and the real
// FreeCAD element-mapping algorithm.
//
// Context-aware: receives KernelContext& for tag allocation and diagnostics.

#include "map_shape_elements.h"
#include "topo_shape_adapter.h"

#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopAbs_ShapeEnum.hxx>

namespace oreo {

namespace {

// Populate default IndexedName-based mapped names for every sub-shape of
// `result`. Used when there are no input shapes to trace history from —
// primitive operations (makeBox, makeCylinder, …). Without this, the
// legacy FreeCAD buildElementMap path early-returns on empty input and
// leaves the element map empty, which means oreo_face_name returns "" for
// primitives. The names produced here carry the full 64-bit documentId as a
// ;:Q postfix so the DocumentIdentityEndToEnd regression test observes it.
NamedShape buildPrimitiveNames(
    const TopoDS_Shape& result,
    std::int64_t opTag,
    std::uint64_t documentId)
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
            // Base name: "Face1" / "Edge3" etc. Then append the op tag so
            // downstream ops can trace back to this primitive, and finally
            // the ;:Q documentId postfix when multi-doc.
            MappedName name(std::string(t.name) + std::to_string(i));
            name.appendTag(opTag);
            name.appendDocumentId(documentId);
            map->setElementName(idx, name, opTag);
        }
    }

    return NamedShape(result, map, opTag);
}

} // anonymous namespace

NamedShape mapShapeElements(
    KernelContext& ctx,
    const TopoDS_Shape& resultShape,
    const ShapeMapper& mapper,
    const std::vector<NamedShape>& inputShapes,
    std::int64_t opTag,
    const char* opName)
{
    if (resultShape.IsNull()) {
        auto map = std::make_shared<ElementMap>();
        return NamedShape(resultShape, map, opTag);
    }

    // Retrieve the full 64-bit documentId from the context. When non-zero it
    // is stamped as a ;:Q postfix on every generated name so the high 32
    // bits of documentId survive the TagAllocator's (low32 | counter)
    // encoding on both MSVC (32-bit long) and GCC (64-bit long).
    const std::uint64_t documentId = ctx.tags().documentId();

    // Primitive op: no inputs to trace history from. The FreeCAD
    // buildElementMap algorithm early-returns in this case, so if we let it
    // run the resulting element map would be empty and oreo_face_name etc.
    // would return "". Generate default IndexedName-based names instead.
    if (inputShapes.empty()) {
        (void)mapper;  // unused in the primitive path
        (void)opName;
        return buildPrimitiveNames(resultShape, opTag, documentId);
    }

    // Convert inputs to adapters
    auto inputAdapters = TopoShapeAdapter::fromNamedShapes(inputShapes);

    // Run the full FreeCAD algorithm
    TopoShapeAdapter adapter;
    adapter.Tag = opTag;
    adapter.buildElementMap(resultShape, mapper, inputAdapters, opName, documentId);

    // Convert back to NamedShape
    return adapter.toNamedShape();
}

} // namespace oreo
