// SPDX-License-Identifier: LGPL-2.1-or-later

// BaseStubs.h — Minimal stubs for Base:: types used by element-map code.
// Replaces Base/Handle.h, Base/Persistence.h, Base/BaseClass.h, Base/Bitmask.h

#ifndef OREO_BASE_STUBS_H
#define OREO_BASE_STUBS_H

#include <atomic>
#include <cstddef>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <type_traits>

// ── PyObject stub (must be before Base namespace) ────────────
// Minimal struct so getPyObject() compiles. Never actually used.
#ifndef Py_OBJECT_H
struct PyObject { int ob_refcnt = 1; };
inline PyObject* Py_None_Stub() { static PyObject none; return &none; }
#define Py_None (Py_None_Stub())
#define Py_INCREF(x) do { ++(x)->ob_refcnt; } while(0)
#define Py_DECREF(x) do { --(x)->ob_refcnt; } while(0)
#define Py_XDECREF(x) do { if(x) Py_DECREF(x); } while(0)
#define Py_RETURN_NONE do { Py_INCREF(Py_None); return Py_None; } while(0)
#endif

namespace Base {

// ── Handled: intrusive reference counting ────────────────────
// Matches FreeCAD's Base::Handled interface.
//
// NOTE on -Wfree-nonheap-object:
//
// GCC 12+ (observed on GCC 13 in the Docker dev container) emits
// -Wfree-nonheap-object on `delete this` inside `unref()`, complaining
// that the pointer being deleted is at a nonzero offset from some
// known allocation boundary. The warning is triggered when the analyzer
// inlines unref() deep into a destructor chain where a Reference<T>
// is an embedded member of some larger object (e.g.
// TopoShapeAdapter::Hasher, where Hasher is `App::StringHasherRef`
// sitting at offset 8 inside the adapter).
//
// This is a false positive. Reference<T>::ptr_ always points at a
// heap-allocated Handled subclass (constructed via `new`); the
// "offset 8" is the offset of the `ptr_` MEMBER within its enclosing
// Reference/adapter, not an offset of the pointee from any allocation.
// GCC's alias analysis conflates the two.
//
// Runtime verification: the clang-asan+ubsan CI cell exercises this
// code path heavily (44/44 passing, zero sanitizer findings — see
// memory `project_p1_p2_landed.md`). If `delete this` were actually
// freeing a non-heap address, ASan would catch it in the invalid-free
// report.
//
// The targeted suppression here keeps the gcc-release build warning-
// clean without disabling the (genuinely useful) warning for the
// rest of the kernel. If a future change moves off `delete this`
// (e.g. switches to std::shared_ptr-backed ref-counting), remove the
// pragma.
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 12
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wfree-nonheap-object"
#endif
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
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 12
#  pragma GCC diagnostic pop
#endif

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
    virtual PyObject* getPyObject() { Py_RETURN_NONE; }
    virtual void setPyObject(PyObject*) {}

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

    UnderlyingType toUnderlyingType() const { return value_; }

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

// ── FreeCAD Exception types ──────────────────────────────────
class ValueError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
    ValueError() : std::runtime_error("ValueError") {}
};

// ── FC_WARN/FC_LOG/FC_ERR/FC_TRACE macros ────────────────────
#define FC_WARN(msg) do {} while(0)
#define FC_LOG(msg) do {} while(0)
#define FC_ERR(msg) do {} while(0)
#define FC_TRACE(msg) do {} while(0)

// These are defined after closing the Base namespace (below)

// ── Reference<T>: intrusive smart pointer ────────────────────
// Matches FreeCAD's Base::Reference<T> — wraps Handled types.
template<typename T>
class Reference {
public:
    Reference() : ptr_(nullptr) {}
    Reference(T* p) : ptr_(p) { if (ptr_) ptr_->ref(); }
    Reference(const Reference& o) : ptr_(o.ptr_) { if (ptr_) ptr_->ref(); }
    Reference(Reference&& o) noexcept : ptr_(o.ptr_) { o.ptr_ = nullptr; }
    ~Reference() { if (ptr_) ptr_->unref(); }

    Reference& operator=(const Reference& o) {
        if (this != &o) {
            if (ptr_) ptr_->unref();
            ptr_ = o.ptr_;
            if (ptr_) ptr_->ref();
        }
        return *this;
    }
    Reference& operator=(Reference&& o) noexcept {
        if (this != &o) {
            if (ptr_) ptr_->unref();
            ptr_ = o.ptr_;
            o.ptr_ = nullptr;
        }
        return *this;
    }
    Reference& operator=(T* p) {
        if (ptr_) ptr_->unref();
        ptr_ = p;
        if (ptr_) ptr_->ref();
        return *this;
    }

    T* operator->() const { return ptr_; }
    T& operator*() const { return *ptr_; }
    operator T*() const { return ptr_; }
    T* get() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    bool operator==(const Reference& o) const { return ptr_ == o.ptr_; }
    bool operator!=(const Reference& o) const { return ptr_ != o.ptr_; }
    bool operator==(const T* p) const { return ptr_ == p; }
    bool operator!=(const T* p) const { return ptr_ != p; }

private:
    T* ptr_;
};

} // namespace Base

// PyObject already defined above, before Base namespace

// ── ENABLE_BITMASK_OPERATORS macro ───────────────────────────
#define ENABLE_BITMASK_OPERATORS(Enum)

// ── FC_LOG_INSTANCE stub (global scope) ──────────────────────
struct _FC_LogStub {
    bool isEnabled(int) const { return false; }
    int level() const { return 0; }
};
#define FC_LOG_INSTANCE (_FC_LogStub{})
#define FC_LOGLEVEL_LOG 0
#define FC_LOGLEVEL_TRACE 0

// ── FC_THROWM: stream-style throw macro ──────────────────────
// FreeCAD uses: FC_THROWM(Base::ValueError, "msg" << var)
// Must be outside namespace Base to be usable everywhere.
struct _FC_StreamThrow {
    std::ostringstream ss;
    template<typename T>
    _FC_StreamThrow& operator<<(const T& v) { ss << v; return *this; }
    [[noreturn]] void throwAs(const char*) { throw std::runtime_error(ss.str()); }
};
#define FC_THROWM(ExcType, msg) do { _FC_StreamThrow _s; _s << msg; _s.throwAs(#ExcType); } while(0)

// ── App:: stubs ──────────────────────────────────────────────
namespace App {
    class DocumentObject;  // Forward declaration only — never instantiated
}

#endif // OREO_BASE_STUBS_H
