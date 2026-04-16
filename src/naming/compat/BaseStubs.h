// BaseStubs.h — Minimal stubs for Base:: types used by element-map code.
// Replaces Base/Handle.h, Base/Persistence.h, Base/BaseClass.h, Base/Bitmask.h

#ifndef OREO_BASE_STUBS_H
#define OREO_BASE_STUBS_H

#include <atomic>
#include <cstddef>
#include <iostream>
#include <type_traits>

namespace Base {

// ── Handled: intrusive reference counting ────────────────────
// Matches FreeCAD's Base::Handled interface.
class Handled {
public:
    Handled() : refCount_(0) {}
    virtual ~Handled() = default;

    void ref() const { ++refCount_; }
    void unref() const {
        if (--refCount_ == 0) {
            delete this;
        }
    }
    int unrefNoDelete() const { return --refCount_; }
    int getRefCount() const { return refCount_.load(); }

    Handled(const Handled&) = delete;
    Handled(Handled&&) = delete;
    Handled& operator=(const Handled&) { return *this; }
    Handled& operator=(Handled&&) = delete;

private:
    mutable std::atomic<int> refCount_;
};

// ── BaseClass: type system stub ──────────────────────────────
class Type {
public:
    bool isDerivedFrom(const Type&) const { return false; }
};

class BaseClass {
public:
    static Type getClassTypeId() { return {}; }
    virtual Type getTypeId() const { return {}; }
    bool isDerivedFrom(const Type) const { return false; }

    static void init() {}
    virtual void* getPyObject() { return nullptr; }
    virtual void setPyObject(void*) {}

    virtual ~BaseClass() = default;
};

// ── Persistence: serialization stub ──────────────────────────
// We stub Save/Restore since oreo-kernel has its own serialization.
class Writer {};
class XMLReader {};
class Reader {};

class Persistence : public BaseClass {
public:
    virtual unsigned int getMemSize() const { return 0; }
    virtual void Save(Writer&) const {}
    virtual void Restore(XMLReader&) {}
    virtual void SaveDocFile(Writer&) const {}
    virtual void RestoreDocFile(Reader&) {}

    static std::string encodeAttribute(const std::string& s) { return s; }
    void dumpToStream(std::ostream&, int) const {}
    void restoreFromStream(std::istream&) {}
};

// ── Flags: bitfield helper ───────────────────────────────────
template<typename Enum>
class Flags {
public:
    using UnderlyingType = typename std::underlying_type<Enum>::type;

    Flags() : value_(0) {}
    Flags(Enum e) : value_(static_cast<UnderlyingType>(e)) {}

    bool testFlag(Enum e) const {
        return (value_ & static_cast<UnderlyingType>(e)) != 0;
    }
    void setFlag(Enum e, bool on = true) {
        if (on) value_ |= static_cast<UnderlyingType>(e);
        else value_ &= ~static_cast<UnderlyingType>(e);
    }

    Flags& operator|=(Enum e) { value_ |= static_cast<UnderlyingType>(e); return *this; }
    Flags& operator&=(Enum e) { value_ &= static_cast<UnderlyingType>(e); return *this; }

private:
    UnderlyingType value_;
};

// ── Console: logging stub ────────────────────────────────────
class ConsoleStub {
public:
    template<typename... Args>
    void Log(const char*, Args...) {}
    template<typename... Args>
    void Warning(const char*, Args...) {}
    template<typename... Args>
    void Error(const char*, Args...) {}
    template<typename... Args>
    void Message(const char*, Args...) {}
};

inline ConsoleStub& Console() {
    static ConsoleStub instance;
    return instance;
}

// ── FC_WARN/FC_LOG/FC_ERR macros ─────────────────────────────
#define FC_WARN(msg) do {} while(0)
#define FC_LOG(msg) do {} while(0)
#define FC_ERR(msg) do {} while(0)
#define FC_THROWM(ExcType, msg) throw std::runtime_error(msg)

} // namespace Base

// ── App:: stubs ──────────────────────────────────────────────
namespace App {
    class DocumentObject;  // Forward declaration only — never instantiated

    // StringHasherRef is defined in the real StringHasher.h,
    // but we forward-declare the alias here for headers that reference it.
}

#endif // OREO_BASE_STUBS_H
