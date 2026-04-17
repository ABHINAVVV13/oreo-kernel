// SPDX-License-Identifier: LGPL-2.1-or-later

// topo_shape_adapter.h — Adapter that provides FreeCAD's TopoShape element-mapping
// interface on top of oreo-kernel's NamedShape + extracted Data::ElementMap.
//
// This class implements makeShapeWithElementMap — the 566-line algorithm from
// FreeCAD's TopoShapeExpansion.cpp that handles all edge cases in element naming:
//   - Two-pass naming (direct → reverse → forward → delayed)
//   - Parallel/coplanar face detection for extrusions
//   - Multi-source name encoding with hash compression
//   - Hierarchical derivation (faces from edges, edges from vertices)
//
// The algorithm is extracted verbatim from FreeCAD 1.0 (LGPL-2.1+).

#ifndef OREO_TOPO_SHAPE_ADAPTER_H
#define OREO_TOPO_SHAPE_ADAPTER_H

#include "named_shape.h"
#include "shape_mapper.h"

// FreeCAD extracted types
#include "freecad/ElementMap.h"
#include "freecad/MappedName.h"
#include "freecad/IndexedName.h"
#include "freecad/StringHasher.h"

#include <TopTools_IndexedMapOfShape.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>
#include <BRepTools.hxx>
#include <gp_Pln.hxx>
#include <Precision.hxx>

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace oreo {

// Cache for sub-shape ancestry lookups (replaces FreeCAD's TopoShapeCache::Ancestry)
class ShapeAncestryCache {
public:
    void build(const TopoDS_Shape& shape, TopAbs_ShapeEnum type) {
        map_.Clear();
        TopExp::MapShapes(shape, type, map_);
    }

    int count() const { return map_.Extent(); }

    TopoDS_Shape find(const TopoDS_Shape& /*parent*/, int index) const {
        if (index < 1 || index > map_.Extent()) return {};
        return map_(index);
    }

    int find(const TopoDS_Shape& /*parent*/, const TopoDS_Shape& sub) const {
        return map_.FindIndex(sub);
    }

private:
    TopTools_IndexedMapOfShape map_;
};

// Adapter that provides FreeCAD-compatible element mapping on our NamedShape.
// Call buildElementMap() after an OCCT operation to construct proper element names.
class TopoShapeAdapter {
public:
    TopoShapeAdapter() : Tag(0), documentId_(0) {}

    explicit TopoShapeAdapter(const NamedShape& ns)
        : shape_(ns.shape())
        , Tag(0)
        , documentId_(ns.shapeId().documentId)
        , identity_(ns.shapeId())
        , elementMap_(std::make_shared<Data::ElementMap>())
    {}

    // The main entry point — extracted from FreeCAD's makeShapeWithElementMap.
    // Builds element names for resultShape using mapper to trace history from inputShapes.
    // documentId is the full 64-bit KernelContext document identity; it is
    // stamped onto every encoded name as a ;:Q postfix so cross-document
    // identity survives the (low32 | counter) tag encoding.
    void buildElementMap(
        const TopoDS_Shape& resultShape,
        const ShapeMapper& mapper,
        const std::vector<TopoShapeAdapter>& inputs,
        const char* op,
        std::uint64_t documentId = 0);

    // Convert back to NamedShape
    NamedShape toNamedShape() const;

    // Create adapter from NamedShape
    static TopoShapeAdapter fromNamedShape(const NamedShape& ns);

    // Create adapters from a vector of NamedShapes
    static std::vector<TopoShapeAdapter> fromNamedShapes(const std::vector<NamedShape>& shapes);

    // Bind the FreeCAD extraction's small internal tag to oreo's full v2
    // identity. The tag is process-local to one mapping run; it is not
    // serialized or exposed.
    void setMappingIdentity(std::int64_t syntheticTag, ShapeIdentity id);

    // Carry input tag bindings into the result adapter before buildElementMap().
    void inheritMappingIdentitiesFrom(const std::vector<TopoShapeAdapter>& inputs);

    // Access the shape
    const TopoDS_Shape& getShape() const { return shape_; }
    bool isNull() const { return shape_.IsNull(); }

    // Access element map
    Data::ElementMapPtr elementMap() const { return elementMap_; }
    Data::ElementMapPtr ensureElementMap() {
        if (!elementMap_) elementMap_ = std::make_shared<Data::ElementMap>();
        return elementMap_;
    }

    // Get a mapped name for an indexed element
    Data::MappedName getMappedName(const Data::IndexedName& idx,
                                   bool silent = false,
                                   Data::ElementIDRefs* sids = nullptr) const;

    // Check if this shape can be mapped
    bool canMapElement() const;

    // Shape tag (int64 so high bits survive Windows 32-bit `long`).
    std::int64_t Tag;

    // String hasher (optional, for hash compression of long names)
    App::StringHasherRef Hasher;

private:
    TopoDS_Shape shape_;
    // Full 64-bit documentId used to stamp ;:Q postfix during naming.
    std::uint64_t documentId_;
    ShapeIdentity identity_;
    std::map<std::int64_t, ShapeIdentity> tagIdentities_;
    Data::ElementMapPtr elementMap_;

    // Ancestry caches
    ShapeAncestryCache vertexCache_;
    ShapeAncestryCache edgeCache_;
    ShapeAncestryCache faceCache_;
    bool cacheBuilt_ = false;

    void initCache();

    // Import sub-element names from input shapes
    void mapSubElement(const std::vector<TopoShapeAdapter>& shapes, const char* op = nullptr);

    // Shape name helper
    static const std::string& shapeName(TopAbs_ShapeEnum type);

public:
    // Find a plane on a face (for parallel/coplanar detection)
    static bool findPlane(const TopoDS_Shape& face, gp_Pln& pln);
};

} // namespace oreo

#endif // OREO_TOPO_SHAPE_ADAPTER_H
