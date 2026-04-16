// shape_fix.cpp — Production-grade multi-step ShapeFix pipeline.
//
// Runs multiple OCCT ShapeFix classes in sequence:
//   1. ShapeFix_Shape (general shape fixing)
//   2. ShapeFix_Solid (fix solid shell consistency)
//   3. ShapeFix_Wire (fix wire edge connectivity)
//   4. ShapeFix_Face (fix face bounds)
//   5. ShapeUpgrade_ShellSewing (sew disconnected shells)
//   6. SameParameter fix (fix curve/surface alignment)

#include "shape_fix.h"
#include "core/kernel_context.h"

#include <BRepCheck_Analyzer.hxx>
#include <ShapeFix_Shape.hxx>
#include <ShapeFix_Solid.hxx>
#include <ShapeFix_Shell.hxx>
#include <ShapeFix_Face.hxx>
#include <ShapeFix_Wire.hxx>
#include <ShapeFix_Edge.hxx>
#include <ShapeUpgrade_ShellSewing.hxx>
#include <BRepLib.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>

namespace oreo {

bool isShapeValid(const TopoDS_Shape& shape) {
    if (shape.IsNull()) return false;
    BRepCheck_Analyzer checker(shape);
    return checker.IsValid();
}

bool fixShape(TopoDS_Shape& shape, const TolerancePolicy& tol) {
    if (shape.IsNull()) return false;

    // Already valid — no fix needed
    if (isShapeValid(shape)) return false;
    bool modified = false;

    // ── Step 1: General ShapeFix_Shape ───────────────────────
    {
        Handle(ShapeFix_Shape) fixer = new ShapeFix_Shape(shape);
        fixer->SetPrecision(tol.linearPrecision);
        fixer->SetMaxTolerance(tol.linearPrecision * 100.0);
        fixer->SetMinTolerance(tol.linearPrecision * 0.01);
        fixer->Perform();
        TopoDS_Shape fixed = fixer->Shape();
        if (!fixed.IsNull()) {
            shape = fixed;
            modified = true;
        }
    }

    if (isShapeValid(shape)) return modified;

    // ── Step 2: Fix solids ───────────────────────────────────
    for (TopExp_Explorer ex(shape, TopAbs_SOLID); ex.More(); ex.Next()) {
        Handle(ShapeFix_Solid) solidFix = new ShapeFix_Solid(TopoDS::Solid(ex.Current()));
        solidFix->SetPrecision(tol.linearPrecision);
        solidFix->Perform();
    }

    // ── Step 3: Fix shells ───────────────────────────────────
    for (TopExp_Explorer ex(shape, TopAbs_SHELL); ex.More(); ex.Next()) {
        Handle(ShapeFix_Shell) shellFix = new ShapeFix_Shell(TopoDS::Shell(ex.Current()));
        shellFix->SetPrecision(tol.linearPrecision);
        shellFix->Perform();
    }

    // ── Step 4: Fix faces and their wires ────────────────────
    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next()) {
        Handle(ShapeFix_Face) faceFix = new ShapeFix_Face(TopoDS::Face(ex.Current()));
        faceFix->SetPrecision(tol.linearPrecision);
        faceFix->Perform();
    }

    // ── Step 5: SameParameter fix ────────────────────────────
    // Ensures that curves on surfaces match 3D curves
    BRepLib::SameParameter(shape, tol.linearPrecision, Standard_True);

    if (isShapeValid(shape)) return true;

    // ── Step 6: Shell sewing (last resort) ───────────────────
    // Sews disconnected shells together
    {
        ShapeUpgrade_ShellSewing sewing;
        TopoDS_Shape sewn = sewing.ApplySewing(shape);
        if (!sewn.IsNull()) {
            shape = sewn;
            modified = true;
        }
    }

    if (isShapeValid(shape)) return modified;

    // ── Step 7: One more pass with relaxed tolerance ─────────
    {
        Handle(ShapeFix_Shape) fixer = new ShapeFix_Shape(shape);
        fixer->SetPrecision(tol.linearPrecision * 10.0);
        fixer->SetMaxTolerance(tol.linearPrecision * 1000.0);
        fixer->Perform();
        TopoDS_Shape fixed = fixer->Shape();
        if (!fixed.IsNull()) {
            shape = fixed;
            modified = true;
        }
    }

    if (!isShapeValid(shape)) {
        return false;
    }

    return modified;
}

bool fixShape(TopoDS_Shape& shape) {
    return fixShape(shape, TolerancePolicy{});
}

} // namespace oreo
