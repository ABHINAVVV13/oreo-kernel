// SPDX-License-Identifier: LGPL-2.1-or-later

// FCGlobal.h stub — replaces FreeCAD's FCGlobal.h for oreo-kernel.
// Provides AppExport macro and FREECAD_DECL_EXPORT/IMPORT.

#ifndef OREO_FCGLOBAL_STUB_H
#define OREO_FCGLOBAL_STUB_H

// DLL export/import macros — see src/naming/freecad/compat/FCGlobal.h
// for the full rationale; OREO_KERNEL_STATIC must produce undecorated
// symbols so the static archive can be linked into test executables
// without dllimport/dllexport mismatches.
#if defined(_WIN32)
    #if defined(OREO_KERNEL_STATIC)
        #define FREECAD_DECL_EXPORT
        #define FREECAD_DECL_IMPORT
        #define AppExport
        #define BaseExport
    #elif defined(OREO_KERNEL_EXPORTS)
        #define FREECAD_DECL_EXPORT __declspec(dllexport)
        #define FREECAD_DECL_IMPORT __declspec(dllimport)
        #define AppExport __declspec(dllexport)
        #define BaseExport __declspec(dllexport)
    #else
        #define FREECAD_DECL_EXPORT __declspec(dllimport)
        #define FREECAD_DECL_IMPORT __declspec(dllimport)
        #define AppExport __declspec(dllimport)
        #define BaseExport __declspec(dllimport)
    #endif
#elif __GNUC__ >= 4
    #define FREECAD_DECL_EXPORT __attribute__((visibility("default")))
    #define FREECAD_DECL_IMPORT __attribute__((visibility("default")))
    #define AppExport __attribute__((visibility("default")))
    #define BaseExport __attribute__((visibility("default")))
#else
    #define FREECAD_DECL_EXPORT
    #define FREECAD_DECL_IMPORT
    #define AppExport
    #define BaseExport
#endif

// Stub for PreCompiled.h — just defines _USE_MATH_DEFINES for Windows
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif

// Type system macros (no-ops — we don't use FreeCAD's RTTI)
#define TYPESYSTEM_HEADER_WITH_OVERRIDE()
#define TYPESYSTEM_HEADER()
#define FC_LOG_LEVEL_INIT(name, verbose, level)

#endif // OREO_FCGLOBAL_STUB_H
