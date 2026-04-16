// planegcs_config.h — Replaces FreeCAD's SketcherGlobal.h and FCGlobal.h
// for standalone PlaneGCS compilation.
//
// Original code: FreeCAD 1.0 (LGPL-2.1+)
// Extracted for oreo-kernel.

#ifndef PLANEGCS_CONFIG_H
#define PLANEGCS_CONFIG_H

// DLL export/import macro — originally SketcherExport
#if defined(_WIN32) || defined(__CYGWIN__)
    #ifdef OREO_KERNEL_EXPORTS
        #define SketcherExport __declspec(dllexport)
    #else
        #define SketcherExport __declspec(dllimport)
    #endif
#elif __GNUC__ >= 4
    #define SketcherExport __attribute__((visibility("default")))
#else
    #define SketcherExport
#endif

// Stub for Base::Console — used only for solver debug logging.
// In production oreo-kernel, this is a no-op unless OREO_PLANEGCS_DEBUG is set.
#include <cstdarg>
#include <cstdio>
#include <string>

namespace Base {

class ConsoleStub {
public:
    void Log(const char* fmt, ...) {
#ifdef OREO_PLANEGCS_DEBUG
        va_list args;
        va_start(args, fmt);
        vfprintf(stderr, fmt, args);
        va_end(args);
#else
        (void)fmt;
#endif
    }

    void Warning(const char* fmt, ...) {
#ifdef OREO_PLANEGCS_DEBUG
        va_list args;
        va_start(args, fmt);
        fprintf(stderr, "[WARN] ");
        vfprintf(stderr, fmt, args);
        va_end(args);
#else
        (void)fmt;
#endif
    }

    void Error(const char* fmt, ...) {
        // Always print errors
        va_list args;
        va_start(args, fmt);
        fprintf(stderr, "[ERROR] ");
        vfprintf(stderr, fmt, args);
        va_end(args);
    }
};

inline ConsoleStub& Console() {
    static ConsoleStub instance;
    return instance;
}

} // namespace Base

#endif // PLANEGCS_CONFIG_H
