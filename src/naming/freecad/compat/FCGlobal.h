// SPDX-License-Identifier: LGPL-2.1-or-later

// FCGlobal.h stub — replaces FreeCAD's FCGlobal.h for oreo-kernel.
// Provides AppExport macro and FREECAD_DECL_EXPORT/IMPORT.

#ifndef OREO_FCGLOBAL_STUB_H
#define OREO_FCGLOBAL_STUB_H

// DLL export/import macros.
//
// Three linkage modes mirror those in include/oreo_kernel.h:
//   OREO_KERNEL_STATIC  — building/consuming the STATIC internal archive.
//                         No decoration; the symbols just live in .lib.
//   OREO_KERNEL_EXPORTS — building the SHARED oreo-kernel.dll.
//                         Decorate as dllexport.
//   (neither)           — consuming the SHARED oreo-kernel.dll.
//                         Decorate as dllimport.
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
#define TYPESYSTEM_SOURCE_ABSTRACT(cls, parent)
#define TYPESYSTEM_SOURCE(cls, parent)
#define FC_LOG_LEVEL_INIT(name, verbose, level)

// Part module export macro
#ifndef PartExport
#define PartExport AppExport
#endif

#endif // OREO_FCGLOBAL_STUB_H
