// SPDX-License-Identifier: LGPL-2.1-or-later

// glb_smoke_main.cpp — Tiny standalone program that exports a box to
// `cube.glb` in the current directory. Not linked as a ctest — used only
// for manual drag-and-drop verification with three.js / Blender / Windows
// 3D Viewer. Run it once to confirm the file opens in external tools.

#include "core/kernel_context.h"
#include "geometry/oreo_geometry.h"
#include "mesh/oreo_mesh.h"
#include "mesh/oreo_mesh_export.h"

#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    const char* outPath = argc >= 2 ? argv[1] : "cube.glb";

    auto ctx = oreo::KernelContext::create();
    auto boxR = oreo::makeBox(*ctx, 20.0, 20.0, 20.0);
    if (!boxR.ok()) { std::cerr << "makeBox failed\n"; return 1; }

    oreo::MeshParams params;
    params.deflectionMode = oreo::DeflectionMode::Auto;
    auto meshR = oreo::tessellate(*ctx, boxR.value(), params);
    if (!meshR.ok()) { std::cerr << "tessellate failed\n"; return 1; }

    auto glbR = oreo::exportGLB(*ctx, meshR.value());
    if (!glbR.ok()) { std::cerr << "exportGLB failed: " << glbR.errorMessage() << "\n"; return 1; }

    auto bytes = std::move(glbR).value();
    std::ofstream out(outPath, std::ios::binary);
    if (!out) { std::cerr << "Cannot open " << outPath << "\n"; return 1; }
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::cout << "Wrote " << bytes.size() << " bytes to " << outPath << "\n";
    return 0;
}
