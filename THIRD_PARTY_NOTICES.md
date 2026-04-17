# Third-Party Notices

oreo-kernel depends on and/or incorporates code from the following projects.

## Open CASCADE Technology

- Component: geometry kernel and STEP I/O libraries linked by the build.
- License: GNU LGPL v2.1 with Open CASCADE exception.
- Source: https://dev.opencascade.org/

## FreeCAD Topological Naming Code

- Component: extracted element-map and mapped-name implementation under
  `src/naming/freecad/`.
- License: LGPL-compatible FreeCAD licensing as noted in the source headers.
- Source: https://www.freecad.org/

## PlaneGCS

- Component: sketch constraint solver integration under `src/sketch/`.
- License: see upstream/source headers for the exact terms retained with the
  imported code.

## Other Build Dependencies

The build also links against Eigen, Boost, Qt Core, nlohmann/json, and GoogleTest
for tests. Package-level copyright files are supplied by the active vcpkg
installation under `vcpkg_installed/<triplet>/share/<package>/copyright`.
