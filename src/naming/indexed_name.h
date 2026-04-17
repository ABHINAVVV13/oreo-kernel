// SPDX-License-Identifier: LGPL-2.1-or-later

// indexed_name.h — Stable element identifier: type string + integer index.
// Inspired by FreeCAD 1.0's IndexedName (LGPL-2.1+), clean reimplementation.
//
// An IndexedName represents one sub-shape in a BRep solid:
//   "Face3" = IndexedName("Face", 3)
//   "Edge12" = IndexedName("Edge", 12)
//   "Vertex1" = IndexedName("Vertex", 1)

#ifndef OREO_INDEXED_NAME_H
#define OREO_INDEXED_NAME_H

#include <cstring>
#include <functional>
#include <ostream>
#include <string>

namespace oreo {

class IndexedName {
public:
    IndexedName() = default;

    IndexedName(const char* type, int index)
        : type_(type ? type : ""), index_(index) {}

    IndexedName(const std::string& type, int index)
        : type_(type), index_(index) {}

    const std::string& type() const { return type_; }
    int index() const { return index_; }

    void setIndex(int i) { index_ = i; }

    bool isNull() const { return type_.empty() || index_ <= 0; }

    std::string toString() const {
        if (isNull()) return {};
        return type_ + std::to_string(index_);
    }

    bool operator==(const IndexedName& o) const {
        return index_ == o.index_ && type_ == o.type_;
    }

    bool operator!=(const IndexedName& o) const { return !(*this == o); }

    bool operator<(const IndexedName& o) const {
        int cmp = type_.compare(o.type_);
        if (cmp != 0) return cmp < 0;
        return index_ < o.index_;
    }

    friend std::ostream& operator<<(std::ostream& os, const IndexedName& n) {
        return os << n.toString();
    }

private:
    std::string type_;
    int index_ = 0;
};

} // namespace oreo

// Hash support
template<>
struct std::hash<oreo::IndexedName> {
    std::size_t operator()(const oreo::IndexedName& n) const noexcept {
        std::size_t h1 = std::hash<std::string>{}(n.type());
        std::size_t h2 = std::hash<int>{}(n.index());
        return h1 ^ (h2 << 1);
    }
};

#endif // OREO_INDEXED_NAME_H
