// shape_fix.cpp — Production-grade multi-step ShapeFix pipeline.
//
// Runs multiple OCCT ShapeFix classes in sequence:
//   1. ShapeFix_Shape (general — delegates to Solid/Shell/Wire/Face internally)
//   2. SameParameter fix (fix curve/surface alignment)
//   3. ShapeUpgrade_ShellSewing (sew disconnected shells, last resort)
//   4. Relaxed tolerance pass
//
// Exception-safe: rolls back to the original shape on OCCT Standard_Failure.

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

    TopoDS_Shape original = shape; // Save for rollback
    bool modified = false;

    try {
        // ── Step 1: General ShapeFix_Shape ───────────────────────
        // ShapeFix_Shape::Perform() internally delegates to ShapeFix_Solid,
        // ShapeFix_Shell, ShapeFix_Wire, and ShapeFix_Face, so no manual
        // sub-shape iteration is needed.
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

        // ── Step 2: SameParameter fix ────────────────────────────
        // Ensures that curves on surfaces match 3D curves
        BRepLib::SameParameter(shape, tol.linearPrecision, Standard_True);

        if (isShapeValid(shape)) return true;

        // ── Step 3: Shell sewing (last resort) ───────────────────
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

        // ── Step 4: One more pass with relaxed tolerance ─────────
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
    } catch (const Standard_Failure&) {
        shape = original; // Rollback
        return false;
    } catch (...) {
        shape = original; // Rollback
        return false;
    }
}

bool fixShape(TopoDS_Shape& shape) {
    return fixShape(shape, TolerancePolicy{});
}

} // namespace oreo
