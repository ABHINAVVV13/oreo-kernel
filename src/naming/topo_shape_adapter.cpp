// topo_shape_adapter.cpp — Full makeShapeWithElementMap algorithm from FreeCAD 1.0.
//
// This is the battle-tested 566-line algorithm from TopoShapeExpansion.cpp
// (lines 1213-1954), adapted to work with oreo-kernel's types.
//
// Original: FreeCAD 1.0, LGPL-2.1+
// Authors: Zheng Lei (realthunder), FreeCAD Project Association

#include "topo_shape_adapter.h"

#include "core/shape_identity_v1.h"
#include "freecad/ElementNamingUtils.h"
#include "freecad/MappedElement.h"

#include <BRep_Tool.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <GeomLib_IsPlanarSurface.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Pln.hxx>
#include <Precision.hxx>

#include <boost/algorithm/string/predicate.hpp>

#include <cassert>
#include <climits>
#include <cmath>
#include <limits>
#include <map>
#include <sstream>

namespace oreo {

// ─── Helper structs (from FreeCAD TopoShapeExpansion.cpp lines 1213-1299) ────

struct ShapeInfo {
    const TopoDS_Shape& shape;
    ShapeAncestryCache& cache;
    TopAbs_ShapeEnum type;
    const char* shapetype;

    ShapeInfo(const TopoDS_Shape& shape, TopAbs_ShapeEnum type, ShapeAncestryCache& cache)
        : shape(shape), cache(cache), type(type)
        , shapetype(type == TopAbs_VERTEX ? "Vertex"
                  : type == TopAbs_EDGE ? "Edge"
                  : type == TopAbs_FACE ? "Face" : "Shape")
    {}

    int count() const { return cache.count(); }
    TopoDS_Shape find(int index) { return cache.find(shape, index); }
    int find(const TopoDS_Shape& sub) { return cache.find(shape, sub); }
};

struct NameKey {
    Data::MappedName name;
    std::int64_t tag = 0;
    int shapetype = 0;

    NameKey() = default;
    explicit NameKey(Data::MappedName n) : name(std::move(n)) {}
    NameKey(int type, Data::MappedName n) : name(std::move(n)) {
        switch (type) {
            case TopAbs_VERTEX: shapetype = 0; break;
            case TopAbs_EDGE:   shapetype = 1; break;
            case TopAbs_FACE:   shapetype = 2; break;
            default:            shapetype = 3;
        }
    }
    bool operator<(const NameKey& other) const {
        if (shapetype < other.shapetype) return true;
        if (shapetype > other.shapetype) return false;
        if (tag < other.tag) return true;
        if (tag > other.tag) return false;
        return name < other.name;
    }
};

struct NameInfo {
    int index {};
    Data::ElementIDRefs sids;
    const char* shapetype {};
};

// Postfix helpers
static const std::string& modPostfix() {
    static std::string s(Data::POSTFIX_MOD); return s;
}
static const std::string& genPostfix() {
    static std::string s(Data::POSTFIX_GEN); return s;
}
static const std::string& modgenPostfix() {
    static std::string s(Data::POSTFIX_MODGEN); return s;
}
static const std::string& upperPostfix() {
    static std::string s(Data::POSTFIX_UPPER); return s;
}
static const std::string& lowerPostfix() {
    static std::string s(Data::POSTFIX_LOWER); return s;
}

// Check for parallel/coplanar faces (from FreeCAD lines 1333-1385)
static void checkForParallelOrCoplanar(
    const TopoDS_Shape& newShape,
    const ShapeInfo& newInfo,
    std::vector<TopoDS_Shape>& newShapes,
    const gp_Pln& pln,
    int& parallelFace,
    int& coplanarFace,
    int& checkParallel)
{
    for (TopExp_Explorer xp(newShape, newInfo.type); xp.More(); xp.Next()) {
        newShapes.push_back(xp.Current());

        if ((parallelFace < 0 || coplanarFace < 0) && checkParallel > 0) {
            gp_Pln plnOther;
            if (TopoShapeAdapter::findPlane(newShapes.back(), plnOther)) {
                if (pln.Axis().IsParallel(plnOther.Axis(), Precision::Angular())) {
                    if (coplanarFace < 0) {
                        gp_Vec vec(pln.Axis().Location(), plnOther.Axis().Location());
                        Standard_Real D1 = gp_Vec(pln.Axis().Direction()).Dot(vec);
                        if (D1 < 0) D1 = -D1;
                        Standard_Real D2 = gp_Vec(plnOther.Axis().Direction()).Dot(vec);
                        if (D2 < 0) D2 = -D2;
                        if (D1 <= Precision::Confusion() && D2 <= Precision::Confusion()) {
                            coplanarFace = (int)newShapes.size();
                            continue;
                        }
                    }
                    if (parallelFace < 0) {
                        parallelFace = (int)newShapes.size();
                    }
                }
            }
        }
    }
}

// ─── TopoShapeAdapter implementation ─────────────────────────────────────────

const std::string& TopoShapeAdapter::shapeName(TopAbs_ShapeEnum type) {
    static const std::string vertex("Vertex"), edge("Edge"), face("Face"), shape("Shape");
    switch (type) {
        case TopAbs_VERTEX: return vertex;
        case TopAbs_EDGE:   return edge;
        case TopAbs_FACE:   return face;
        default:            return shape;
    }
}

bool TopoShapeAdapter::findPlane(const TopoDS_Shape& face, gp_Pln& pln) {
    if (face.IsNull() || face.ShapeType() != TopAbs_FACE) return false;
    try {
        BRepAdaptor_Surface adapt(TopoDS::Face(face));
        if (adapt.GetType() == GeomAbs_Plane) {
            pln = adapt.Plane();
            return true;
        }
        // Try harder with GeomLib
        Handle(Geom_Surface) surf = BRep_Tool::Surface(TopoDS::Face(face));
        if (!surf.IsNull()) {
            GeomLib_IsPlanarSurface check(surf);
            if (check.IsPlanar()) {
                pln = check.Plan();
                return true;
            }
        }
    } catch (...) {
        // findPlane is a best-effort geometry query used during element naming.
        // Failure here is non-fatal — the caller treats a false return as
        // "not planar" and skips parallel/coplanar face detection for this element.
    }
    return false;
}

void TopoShapeAdapter::initCache() {
    if (cacheBuilt_) return;
    vertexCache_.build(shape_, TopAbs_VERTEX);
    edgeCache_.build(shape_, TopAbs_EDGE);
    faceCache_.build(shape_, TopAbs_FACE);
    cacheBuilt_ = true;
}

bool TopoShapeAdapter::canMapElement() const {
    return !shape_.IsNull();
}

Data::MappedName TopoShapeAdapter::getMappedName(
    const Data::IndexedName& idx, bool /*silent*/, Data::ElementIDRefs* sids) const
{
    if (!elementMap_) return {};
    return elementMap_->find(idx, sids);
}

void TopoShapeAdapter::mapSubElement(const std::vector<TopoShapeAdapter>& shapes, const char* /*op*/) {
    // Import element names from input shapes into our element map.
    // This is a simplified version of FreeCAD's mapSubElement — it copies names
    // from inputs that correspond to sub-shapes still present in the result.
    if (!elementMap_) elementMap_ = std::make_shared<Data::ElementMap>();

    for (auto& other : shapes) {
        if (other.isNull() || !other.elementMap_) continue;

        // For each sub-shape type, check if input elements exist in result
        for (TopAbs_ShapeEnum type : {TopAbs_VERTEX, TopAbs_EDGE, TopAbs_FACE}) {
            const char* typeName = shapeName(type).c_str();
            TopTools_IndexedMapOfShape otherMap, resultMap;
            TopExp::MapShapes(other.shape_, type, otherMap);
            TopExp::MapShapes(shape_, type, resultMap);

            for (int i = 1; i <= otherMap.Extent(); ++i) {
                int resultIdx = resultMap.FindIndex(otherMap(i));
                if (resultIdx > 0) {
                    // This input element exists identically in the result
                    Data::IndexedName otherIdx = Data::IndexedName::fromConst(typeName, i);
                    Data::ElementIDRefs sids;
                    Data::MappedName name = other.elementMap_->find(otherIdx, &sids);
                    if (name) {
                        Data::IndexedName resultIdxName = Data::IndexedName::fromConst(typeName, resultIdx);
                        if (!elementMap_->find(resultIdxName)) {
                            elementMap_->setElementName(resultIdxName, name, other.Tag, &sids);
                        }
                    }
                }
            }
        }
    }
}

TopoShapeAdapter TopoShapeAdapter::fromNamedShape(const NamedShape& ns) {
    TopoShapeAdapter adapter;
    adapter.shape_ = ns.shape();
    adapter.Tag = ns.tag();
    adapter.elementMap_ = std::make_shared<Data::ElementMap>();

    // Import names from the oreo ElementMap into the Data::ElementMap
    if (auto oreoMap = ns.elementMap()) {
        auto allEntries = oreoMap->getAll();
        for (auto& [oreoIdx, oreoName] : allEntries) {
            // Convert oreo::IndexedName → Data::IndexedName
            // Use fromConst with the type string; Data::IndexedName internally
            // stores/deduplicates the type pointer via its set() mechanism.
            Data::IndexedName dataIdx(oreoIdx.type().c_str(), oreoIdx.index());

            // Convert oreo::MappedName → Data::MappedName
            // oreo::MappedName::data() returns the full encoded name as std::string
            Data::MappedName dataName(oreoName.data());

            adapter.elementMap_->setElementName(dataIdx, dataName, ns.tag());
        }
    }

    return adapter;
}

std::vector<TopoShapeAdapter> TopoShapeAdapter::fromNamedShapes(const std::vector<NamedShape>& shapes) {
    std::vector<TopoShapeAdapter> result;
    result.reserve(shapes.size());
    for (auto& s : shapes) {
        result.push_back(fromNamedShape(s));
    }
    return result;
}

namespace {

// canonicalizeToV2 — rewrite a FreeCAD-produced mapped-name string so
// every ;:H<hex> and ;:T<dec> hop becomes the canonical v2 carrier
// ;:P<32hex>. The FreeCAD extraction's internal encodeElementName only
// knows how to emit ;:H/;:T; this bridge canonicalizes at the
// TopoShapeAdapter → NamedShape boundary so downstream consumers see
// pure v2 naming regardless of whether a hop came from a primitive
// op (already ;:P) or the FreeCAD algorithm (originally ;:H/;:T).
//
// Correctness invariants:
//   - Left-to-right scan — rewrites preserve postfix order.
//   - Each hop is rewritten independently; ;:M / ;:G / ;:C / ;:U /
//     ;:L / ;:Q / ;:R etc. pass through unchanged.
//   - If a ;:H/;:T payload fails to decode (e.g. decodeV1Scalar
//     throws Case Error because the ambient document's docId low-32
//     doesn't match the scalar's high-32 — possible when a cross-doc
//     name was carried in as an input), the hop is LEFT AS-IS and
//     scanning continues past it. Readers handle mixed chains via
//     the rightmost-marker rule; data integrity is preserved.
//   - A ;:P<32hex> already present passes through unchanged (no
//     double-canonicalization).
//
// Size impact: each v1 ;:H<hex> (variable width) becomes ;:P<32hex>
// (fixed width). Worst case: a single-char hex ";:H1" (4 bytes) grows
// to ";:P00000000000000000000000000000001" (35 bytes) — +31 bytes per
// hop. Measured on the bench_identity 10k-op workload the difference
// is <15% on serialize throughput, within the §7 gate.
std::string canonicalizeToV2(const std::string& src,
                             std::uint64_t documentId) {
    std::string out;
    out.reserve(src.size() + 32);

    auto rewriteScalar = [&](std::int64_t scalar) -> bool {
        ShapeIdentity id;
        try {
            id = oreo::decodeV1Scalar(scalar, documentId, nullptr);
        } catch (const std::invalid_argument&) {
            // Hint/scalar mismatch — leave the ;:H as-is. Could happen
            // with cross-document history crossings that shouldn't
            // normally reach this point, but if they do we preserve
            // the raw bytes rather than corrupting them.
            return false;
        }
        char buf[36];  // 32 hex + NUL + slack
        std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                      static_cast<unsigned long long>(id.documentId),
                      static_cast<unsigned long long>(id.counter));
        out += ";:P";
        out += buf;
        return true;
    };

    std::size_t pos = 0;
    while (pos < src.size()) {
        // Match ";:H" (hex v1 tag).
        if (pos + 3 <= src.size() && src.compare(pos, 3, ";:H") == 0) {
            std::size_t start = pos + 3;
            std::size_t end = start;
            while (end < src.size()
                   && std::isxdigit(static_cast<unsigned char>(src[end]))) {
                ++end;
            }
            if (end > start) {
                // strtoull handles the full 64-bit bit pattern; cast
                // to int64 preserves the same bits for decodeV1Scalar.
                const std::uint64_t u = std::strtoull(
                    src.substr(start, end - start).c_str(), nullptr, 16);
                if (rewriteScalar(static_cast<std::int64_t>(u))) {
                    pos = end;
                    continue;
                }
            }
            // Malformed or un-decodable — fall through to byte-copy.
        }

        // Match ";:T" (decimal v1 tag).
        if (pos + 3 <= src.size() && src.compare(pos, 3, ";:T") == 0) {
            std::size_t start = pos + 3;
            std::size_t end = start;
            while (end < src.size()
                   && std::isdigit(static_cast<unsigned char>(src[end]))) {
                ++end;
            }
            if (end > start) {
                const std::int64_t s = std::strtoll(
                    src.substr(start, end - start).c_str(), nullptr, 10);
                if (rewriteScalar(s)) {
                    pos = end;
                    continue;
                }
            }
        }

        // Default: copy one byte through verbatim. This preserves
        // postfixes we don't rewrite (;:M, ;:G, ;:C, ;:P, ;:Q, …)
        // plus any non-postfix content in the base name.
        out += src[pos++];
    }

    return out;
}

} // anonymous namespace

NamedShape TopoShapeAdapter::toNamedShape() const {
    // Convert back to our NamedShape, rewriting every ;:H/;:T tag hop
    // emitted by the FreeCAD extraction into the canonical v2 ;:P
    // carrier. After this pass the element names are pure v2: every
    // identity is a full 16-byte {documentId, counter}, no squeeze.
    // The FreeCAD code itself is unchanged — canonicalization happens
    // exactly at the extraction boundary, which keeps future upstream
    // re-pulls straightforward.
    //
    // No ;:Q is appended: the v2 ;:P carrier already encodes the full
    // documentId inline on every hop, so the top-level docId stamp is
    // redundant. ;:Q reading remains as a fallback for legacy names
    // that still carry ;:H (e.g. buffers deserialized in compat mode
    // where the canonicalization never ran).
    auto oreoMap = std::make_shared<ElementMap>();

    if (elementMap_) {
        auto allElements = elementMap_->getAll();
        for (auto& elem : allElements) {
            // Convert Data::IndexedName → oreo::IndexedName
            IndexedName idx(elem.index.getType(), elem.index.getIndex());

            // Convert Data::MappedName → oreo::MappedName. Use
            // toString() to get the full name (data + postfix).
            // dataBytes().toStdString() is WRONG — it drops the
            // postfix portion which contains the operation history
            // chain (;:H tags, ;:M/;:G etc).
            const std::string rawName = elem.name.toString();
            MappedName name(canonicalizeToV2(rawName, documentId_));
            oreoMap->setElementName(idx, name, Tag);
        }
    }

    // Reconstruct the full ShapeIdentity from Tag + documentId_ — the
    // two fields together describe what nextShapeIdentity() returned.
    // This keeps the deprecated int64 ctor off the hot path.
    ShapeIdentity id = (Tag == 0)
        ? ShapeIdentity{}
        : oreo::decodeV1Scalar(Tag, documentId_, nullptr);
    return NamedShape(shape_, oreoMap, id);
}

// ─── THE ALGORITHM: makeShapeWithElementMap ──────────────────────────────────
// Extracted from FreeCAD 1.0 TopoShapeExpansion.cpp lines 1388-1954.
// Adapted to use TopoShapeAdapter instead of FreeCAD's TopoShape.

void TopoShapeAdapter::buildElementMap(
    const TopoDS_Shape& shape,
    const ShapeMapper& mapper,
    const std::vector<TopoShapeAdapter>& inputShapes,
    const char* op,
    std::uint64_t documentId)
{
    shape_ = shape;
    documentId_ = documentId;
    cacheBuilt_ = false;

    if (shape_.IsNull()) return;
    if (inputShapes.empty()) return;

    size_t canMap = 0;
    for (auto& s : inputShapes) {
        if (s.canMapElement()) ++canMap;
    }
    if (canMap == 0) return;

    if (!op) op = "Maker";
    std::string _op = op;
    _op += '_';

    initCache();
    ShapeInfo vertexInfo(shape_, TopAbs_VERTEX, vertexCache_);
    ShapeInfo edgeInfo(shape_, TopAbs_EDGE, edgeCache_);
    ShapeInfo faceInfo(shape_, TopAbs_FACE, faceCache_);

    // Import unchanged sub-elements from inputs
    std::vector<TopoShapeAdapter> mutableInputs = inputShapes;
    for (auto& input : mutableInputs) {
        const_cast<TopoShapeAdapter&>(input).initCache();
    }
    mapSubElement(inputShapes);

    std::array<ShapeInfo*, 3> infos = {&vertexInfo, &edgeInfo, &faceInfo};

    std::array<ShapeInfo*, TopAbs_SHAPE> infoMap {};
    infoMap[TopAbs_VERTEX] = &vertexInfo;
    infoMap[TopAbs_EDGE] = &edgeInfo;
    infoMap[TopAbs_WIRE] = &edgeInfo;
    infoMap[TopAbs_FACE] = &faceInfo;
    infoMap[TopAbs_SHELL] = &faceInfo;
    infoMap[TopAbs_SOLID] = &faceInfo;
    infoMap[TopAbs_COMPOUND] = &faceInfo;
    infoMap[TopAbs_COMPSOLID] = &faceInfo;

    std::ostringstream ss;
    std::string postfix;
    Data::MappedName newName;

    std::map<Data::IndexedName, std::map<NameKey, NameInfo>> newNames;

    // ── PASS 1: Collect names from mapper.modified() and mapper.generated() ──
    for (auto& pinfo : infos) {
        auto& info = *pinfo;
        for (auto& incomingShape : inputShapes) {
            if (!incomingShape.canMapElement()) continue;

            // Build sub-shape map for this input
            TopTools_IndexedMapOfShape otherSubShapes;
            TopExp::MapShapes(incomingShape.shape_, info.type, otherSubShapes);
            if (otherSubShapes.Extent() == 0) continue;

            for (int i = 1; i <= otherSubShapes.Extent(); ++i) {
                const auto& otherElement = otherSubShapes(i);
                Data::ElementIDRefs sids;
                Data::IndexedName otherIdx = Data::IndexedName::fromConst(info.shapetype, i);
                NameKey key(info.type,
                    incomingShape.getMappedName(otherIdx, true, &sids));
                if (!key.name) {
                    key.name = Data::MappedName(otherIdx);
                }

                // ── Modified elements ────────────────────────
                int newShapeCounter = 0;
                for (auto& newShape : mapper.modified(otherElement)) {
                    ++newShapeCounter;
                    if (newShape.ShapeType() >= TopAbs_SHAPE) continue;

                    auto* newInfoPtr = infoMap.at(newShape.ShapeType());
                    if (!newInfoPtr) continue;
                    auto& newInfo = *newInfoPtr;
                    if (newInfo.type != newShape.ShapeType()) continue;

                    int newShapeIndex = newInfo.find(newShape);
                    if (newShapeIndex == 0) continue;

                    Data::IndexedName element =
                        Data::IndexedName::fromConst(newInfo.shapetype, newShapeIndex);
                    if (getMappedName(element)) continue;

                    key.tag = incomingShape.Tag;
                    auto& name_info = newNames[element][key];
                    name_info.sids = sids;
                    name_info.index = newShapeCounter;
                    name_info.shapetype = info.shapetype;
                }

                // ── Generated elements ───────────────────────
                int checkParallel = -1;
                gp_Pln pln;
                newShapeCounter = 0;

                for (auto& newShape : mapper.generated(otherElement)) {
                    if (newShape.ShapeType() >= TopAbs_SHAPE) continue;

                    int parallelFace = -1;
                    int coplanarFace = -1;
                    auto* newInfoPtr = infoMap.at(newShape.ShapeType());
                    if (!newInfoPtr) continue;
                    auto& newInfo = *newInfoPtr;
                    std::vector<TopoDS_Shape> newShapes;
                    int shapeOffset = 0;

                    if (newInfo.type == newShape.ShapeType()) {
                        newShapes.push_back(newShape);
                    } else {
                        // Higher level shape — offset for sorting priority
                        shapeOffset = 3;
                        if (info.type == TopAbs_FACE && checkParallel < 0) {
                            if (!findPlane(otherElement, pln)) checkParallel = 0;
                            else checkParallel = 1;
                        }
                        checkForParallelOrCoplanar(newShape, newInfo, newShapes,
                                                    pln, parallelFace, coplanarFace, checkParallel);
                    }

                    key.shapetype += shapeOffset;
                    for (auto& workingShape : newShapes) {
                        ++newShapeCounter;
                        int workingShapeIndex = newInfo.find(workingShape);
                        if (workingShapeIndex == 0) continue;

                        Data::IndexedName element =
                            Data::IndexedName::fromConst(newInfo.shapetype, workingShapeIndex);
                        if (getMappedName(element)) continue;

                        key.tag = incomingShape.Tag;
                        auto& name_info = newNames[element][key];
                        name_info.sids = sids;
                        if (newShapeCounter == parallelFace) {
                            name_info.index = std::numeric_limits<int>::min();
                        } else if (newShapeCounter == coplanarFace) {
                            name_info.index = std::numeric_limits<int>::min() + 1;
                        } else {
                            name_info.index = -newShapeCounter;
                        }
                        name_info.shapetype = info.shapetype;
                    }
                    key.shapetype -= shapeOffset;
                }
            }
        }
    }

    // ── PASS 2+3+4: Name construction with delayed mode ──────────────────────
    bool delayed = false;

    while (true) {
        // ── Direct naming pass ───────────────────────────────
        for (auto itName = newNames.begin(), itNext = itName; itNext != newNames.end();
             itName = itNext) {
            ++itNext;

            auto& element = itName->first;
            auto& names = itName->second;
            const auto& first_key = names.begin()->first;
            auto& first_info = names.begin()->second;

            if (!delayed && first_key.shapetype >= 3 && first_info.index > INT_MIN + 1) {
                continue;  // Delay high-level mappings
            }
            if (!delayed && getMappedName(element)) {
                newNames.erase(itName);
                continue;
            }

            int name_type = first_info.index > 0 ? 1 : 2;  // 1=modified, 2=generated
            Data::MappedName first_name = first_key.name;
            Data::ElementIDRefs sids(first_info.sids);

            // Multi-source encoding
            postfix.clear();
            if (names.size() > 1) {
                ss.str("");
                ss << '(';
                bool first = true;
                auto it = names.begin();
                int count = 0;
                for (++it; it != names.end(); ++it) {
                    auto& other_key = it->first;
                    if (other_key.shapetype >= 3 && first_key.shapetype < 3) break;
                    if (first) first = false;
                    else ss << '|';

                    auto& other_info = it->second;
                    std::ostringstream ss2;
                    if (other_info.index != 1) {
                        ss2 << Data::ELEMENT_MAP_PREFIX << 'K';
                        if (other_info.index == INT_MIN) ss2 << '0';
                        else if (other_info.index == INT_MIN + 1) ss2 << "00";
                        else ss2 << other_info.index;
                    }
                    Data::MappedName other_name = other_key.name;
                    ensureElementMap()->encodeElementName(
                        *other_info.shapetype, other_name, ss2, &sids,
                        Tag, nullptr, other_key.tag);
                    ss << other_name;

                    if ((name_type == 1 && other_info.index < 0)
                        || (name_type == 2 && other_info.index > 0)) {
                        name_type = 0;  // Both modified and generated
                    }
                    sids += other_info.sids;
                    if (++count == 4) break;  // Limit name length
                }
                if (!first) {
                    ss << ')';
                    if (Hasher) {
                        sids.push_back(Hasher->getID(ss.str().c_str()));
                        ss.str("");
                        ss << sids.back().toString();
                    }
                    postfix = ss.str();
                }
            }

            ss.str("");
            if (name_type == 2) ss << genPostfix();
            else if (name_type == 1) ss << modPostfix();
            else ss << modgenPostfix();

            if (first_info.index == INT_MIN) ss << '0';
            else if (first_info.index == INT_MIN + 1) ss << "00";
            else if (std::abs(first_info.index) > 1) ss << std::abs(first_info.index);
            ss << postfix;

            ensureElementMap()->encodeElementName(
                element[0], first_name, ss, &sids, Tag, op, first_key.tag);
            elementMap()->setElementName(element, first_name, Tag, &sids);

            if (!delayed && first_key.shapetype < 3) {
                newNames.erase(itName);
            }
        }

        // ── Reverse pass: name lower elements from higher ones ───
        for (size_t infoIndex = infos.size() - 1; infoIndex != 0; --infoIndex) {
            std::map<Data::IndexedName,
                     std::map<Data::MappedName, NameInfo, Data::ElementNameComparator>> names;
            auto& info = *infos.at(infoIndex);
            auto& next = *infos.at(infoIndex - 1);
            int elementCounter = 1;
            auto it = newNames.end();
            if (delayed) {
                it = newNames.upper_bound(Data::IndexedName::fromConst(info.shapetype, 0));
            }
            for (;; ++elementCounter) {
                Data::IndexedName element;
                if (!delayed) {
                    if (elementCounter > info.count()) break;
                    element = Data::IndexedName::fromConst(info.shapetype, elementCounter);
                    if (newNames.count(element) != 0U) continue;
                } else if (it == newNames.end()
                           || !boost::starts_with(it->first.getType(), info.shapetype)) {
                    break;
                } else {
                    element = it->first;
                    ++it;
                    elementCounter = element.getIndex();
                    if (elementCounter == 0 || elementCounter > info.count()) continue;
                }

                Data::ElementIDRefs sids;
                Data::MappedName mapped = getMappedName(element, false, &sids);
                if (!mapped) continue;

                TopTools_IndexedMapOfShape submap;
                TopExp::MapShapes(info.find(elementCounter), next.type, submap);
                for (int submapIndex = 1, infoCounter = 1; submapIndex <= submap.Extent();
                     ++submapIndex) {
                    int elementIndex = next.find(submap(submapIndex));
                    if (elementIndex == 0) continue;
                    Data::IndexedName indexedName =
                        Data::IndexedName::fromConst(next.shapetype, elementIndex);
                    if (getMappedName(indexedName)) continue;
                    auto& infoRef = names[indexedName][mapped];
                    infoRef.index = infoCounter++;
                    infoRef.sids = sids;
                }
            }

            // Assign the names
            for (auto& [indexedName, nameInfoMap] : names) {
                auto& nameInfoEntry = *nameInfoMap.begin();
                auto& nameInfo = nameInfoEntry.second;
                auto& nameSids = nameInfo.sids;
                newName = nameInfoEntry.first;
                ss.str("");
                ss << upperPostfix();
                if (nameInfo.index > 1) ss << nameInfo.index;
                ensureElementMap()->encodeElementName(
                    indexedName[0], newName, ss, &nameSids, Tag, op);
                elementMap()->setElementName(indexedName, newName, Tag, &nameSids);
            }
        }

        // ── Forward pass: name higher elements from lower ones ───
        bool hasUnnamed = false;
        for (size_t ifo = 1; ifo < infos.size(); ++ifo) {
            auto& info = *infos.at(ifo);
            auto& prev = *infos.at(ifo - 1);
            for (int i = 1; i <= info.count(); ++i) {
                Data::IndexedName element = Data::IndexedName::fromConst(info.shapetype, i);
                if (getMappedName(element)) continue;

                Data::ElementIDRefs sids;
                std::map<Data::MappedName, Data::IndexedName, Data::ElementNameComparator> names;
                TopExp_Explorer xp;
                if (info.type == TopAbs_FACE) {
                    xp.Init(BRepTools::OuterWire(TopoDS::Face(info.find(i))), TopAbs_EDGE);
                } else {
                    xp.Init(info.find(i), prev.type);
                }
                for (; xp.More(); xp.Next()) {
                    int prevIdx = prev.find(xp.Current());
                    if (prevIdx == 0) continue;
                    Data::IndexedName prevElement =
                        Data::IndexedName::fromConst(prev.shapetype, prevIdx);
                    if (!delayed && newNames.count(prevElement) != 0U) {
                        names.clear();
                        break;
                    }
                    Data::ElementIDRefs sid;
                    Data::MappedName name = getMappedName(prevElement, false, &sid);
                    if (!name) { names.clear(); break; }
                    auto res = names.emplace(name, prevElement);
                    if (res.second) sids += sid;
                }
                if (names.empty()) { hasUnnamed = true; continue; }

                auto it = names.begin();
                newName = it->first;
                ss.str("");
                if (names.size() == 1) {
                    ss << lowerPostfix();
                } else {
                    bool first = true;
                    if (!Hasher) ss << lowerPostfix();
                    ss << '(';
                    int count = 0;
                    for (++it; it != names.end(); ++it) {
                        if (first) first = false;
                        else ss << '|';
                        ss << it->first;
                        if (++count == 4) break;
                    }
                    ss << ')';
                    if (Hasher) {
                        sids.push_back(Hasher->getID(ss.str().c_str()));
                        ss.str("");
                        ss << lowerPostfix() << sids.back().toString();
                    }
                }
                ensureElementMap()->encodeElementName(element[0], newName, ss, &sids, Tag, op);
                elementMap()->setElementName(element, newName, Tag, &sids);
            }
        }

        // Check if we need another pass with delayed mode
        if (!hasUnnamed || delayed || newNames.empty()) break;
        delayed = true;
    }
}

} // namespace oreo
