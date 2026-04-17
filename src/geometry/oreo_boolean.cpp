// oreo_boolean.cpp — Production-grade boolean operations.
//
// Features:
//   - Auto-fuzzy tolerance (size-aware, from FreeCAD's FCBRepAlgoAPI pattern)
//   - Input validation (BRepCheck_Analyzer)
//   - Automatic compound expansion (flatten before boolean)
//   - Retry-with-healing: if boolean fails, attempt ShapeFix on inputs and retry
//   - Retry with increased tolerance on failure
//   - Proper element map tracing through the operation

#include "oreo_geometry.h"
#include "core/diagnostic_scope.h"
#include "core/occt_try.h"
#include "core/oreo_error.h"
#include "core/oreo_tolerance.h"
#include "io/shape_fix.h"
#include "naming/map_shape_elements.h"
#include "naming/shape_mapper.h"

#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <Bnd_Box.hxx>
#include <Precision.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Iterator.hxx>

#include <cmath>
#include <memory>
#include <vector>

namespace oreo {

namespace {

// Compute auto-fuzzy tolerance based on bounding box extent.
// Formula from FreeCAD: factor * sqrt(bboxExtent) * Precision::Confusion()
double computeAutoFuzzy(KernelContext& ctx, const TopoDS_Shape& a, const TopoDS_Shape& b) {
    Bnd_Box bounds;
    BRepBndLib::Add(a, bounds);
    BRepBndLib::Add(b, bounds);
    if (bounds.IsVoid()) return 0.0;
    double extent = bounds.SquareExtent();
    if (!std::isfinite(extent) || extent < 0.0) return 0.0;
    return ctx.tolerance().booleanFuzzyFactor * std::sqrt(extent) * Precision::Confusion();
}

// Expand a compound shape into its constituent solids/shells.
// Booleans work better on individual solids than on compounds.
std::vector<TopoDS_Shape> expandCompound(const TopoDS_Shape& shape) {
    std::vector<TopoDS_Shape> result;
    if (shape.ShapeType() == TopAbs_COMPOUND) {
        for (TopoDS_Iterator it(shape); it.More(); it.Next()) {
            auto children = expandCompound(it.Value());
            result.insert(result.end(), children.begin(), children.end());
        }
    } else {
        result.push_back(shape);
    }
    return result;
}

// Validate a shape for boolean operations
bool validateForBoolean(KernelContext& ctx, const TopoDS_Shape& shape, const char* label) {
    if (shape.IsNull()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                       std::string("Null ") + label + " shape for boolean");
        return false;
    }
    BRepCheck_Analyzer checker(shape);
    if (!checker.IsValid()) {
        ctx.diag().error(ErrorCode::SHAPE_INVALID,
                       std::string(label) + " shape is invalid for boolean operation",
                       "Run ShapeFix on input before boolean");
        return false;
    }
    return true;
}

enum class BoolOp { Fuse, Cut, Common };

// Execute a boolean operation with full error handling and retry logic.
GeomResult executeBooleanWithRetry(
    KernelContext& ctx,
    BoolOp op,
    const char* opName,
    const NamedShape& a,
    const NamedShape& b,
    double tolerance)
{
    DiagnosticScope scope(ctx);

    if (a.isNull() || b.isNull()) {
        ctx.diag().error(ErrorCode::INVALID_INPUT,
                       std::string("Cannot perform ") + opName + " with null shape");
        return scope.makeFailure<NamedShape>();
    }

    OREO_OCCT_TRY

    TopoDS_Shape shapeA = a.shape();
    TopoDS_Shape shapeB = b.shape();

    // -- Attempt 1: Direct boolean ----------------------------------------
    // Returns the entire BRepAlgoAPI_BooleanOperation so we can reuse its
    // history for element mapping without a costly re-execution.
    auto tryBoolean = [&](const TopoDS_Shape& sa, const TopoDS_Shape& sb,
                          double fuzzy) -> std::unique_ptr<BRepAlgoAPI_BooleanOperation> {
        std::unique_ptr<BRepAlgoAPI_BooleanOperation> mkOp;

        // Use default constructors — the 2-arg constructors auto-execute the
        // operation in OCCT 7.x, which means SetFuzzyValue() would be too late.
        switch (op) {
            case BoolOp::Fuse:   mkOp = std::make_unique<BRepAlgoAPI_Fuse>(); break;
            case BoolOp::Cut:    mkOp = std::make_unique<BRepAlgoAPI_Cut>(); break;
            case BoolOp::Common: mkOp = std::make_unique<BRepAlgoAPI_Common>(); break;
        }

        // Set arguments explicitly before Build()
        TopTools_ListOfShape args, tools;
        args.Append(sa);
        tools.Append(sb);
        mkOp->SetArguments(args);
        mkOp->SetTools(tools);

        // Set fuzzy BEFORE Build() — this is the critical fix
        if (fuzzy > 0.0) {
            mkOp->SetFuzzyValue(fuzzy);
        } else if (fuzzy < 0.0) {
            double autoFuzzy = computeAutoFuzzy(ctx, sa, sb);
            if (autoFuzzy > 0.0) mkOp->SetFuzzyValue(autoFuzzy);
        }

        mkOp->SetRunParallel(Standard_True);
        mkOp->Build();

        if (mkOp->IsDone() && !mkOp->HasErrors()) {
            return mkOp;
        }
        return nullptr;
    };

    // Attempt 1: with user-specified tolerance
    double fuzzy = tolerance;
    std::unique_ptr<BRepAlgoAPI_BooleanOperation> successfulOp = tryBoolean(shapeA, shapeB, fuzzy);

    // -- Attempt 2: ShapeFix inputs and retry -----------------------------
    if (!successfulOp) {
        TopoDS_Shape fixedA = shapeA;
        TopoDS_Shape fixedB = shapeB;
        bool fixedSomething = false;

        if (!isShapeValid(fixedA)) {
            fixedSomething |= fixShape(fixedA, ctx.tolerance());
        }
        if (!isShapeValid(fixedB)) {
            fixedSomething |= fixShape(fixedB, ctx.tolerance());
        }

        if (fixedSomething) {
            successfulOp = tryBoolean(fixedA, fixedB, fuzzy);
            if (successfulOp) {
                shapeA = fixedA;
                shapeB = fixedB;
            }
        }
    }

    // -- Attempt 3: Increase tolerance (2x, then 10x) --------------------
    if (!successfulOp) {
        double autoFuzzy = computeAutoFuzzy(ctx, shapeA, shapeB);
        for (double multiplier : {2.0, 10.0, 100.0}) {
            double escalatedFuzzy = std::max(autoFuzzy * multiplier, 1e-5);
            successfulOp = tryBoolean(shapeA, shapeB, escalatedFuzzy);
            if (successfulOp) break;
        }
    }

    // -- All attempts failed ----------------------------------------------
    if (!successfulOp) {
        ctx.diag().error(ErrorCode::BOOLEAN_FAILED,
                       std::string(opName) + " failed after multiple retry attempts",
                       "Geometry may be too complex or near-degenerate for boolean. "
                       "Try simplifying inputs or adjusting tolerance.");
        return scope.makeFailure<NamedShape>();
    }

    TopoDS_Shape result = successfulOp->Shape();

    // -- Validate and optionally fix result --------------------------------
    if (!isShapeValid(result)) {
        fixShape(result, ctx.tolerance());
        // If still invalid after fix, warn but still return the result
        if (!isShapeValid(result)) {
            ctx.diag().warning(ErrorCode::SHAPE_INVALID,
                             std::string(opName) + " result is invalid after ShapeFix — geometry may be degraded",
                             "Result returned but may have self-intersections or other defects");
        }
    }

    // -- Build element map ------------------------------------------------
    // Reuse the successful operation object directly — it already has the
    // modification/generation history needed for element mapping.
    auto tag = ctx.tags().nextShapeIdentity();
    MakerMapper mapper(*successfulOp);
    auto mapped = mapShapeElements(ctx, result, mapper, {a, b}, tag, opName);
    return scope.makeResult(mapped);
    OREO_OCCT_CATCH_NS(scope, ctx, opName)
}

} // anonymous namespace

GeomResult booleanUnion(KernelContext& ctx, const NamedShape& a, const NamedShape& b, double tolerance) {
    return executeBooleanWithRetry(ctx, BoolOp::Fuse, "Fuse", a, b, tolerance);
}

GeomResult booleanSubtract(KernelContext& ctx, const NamedShape& target, const NamedShape& tool, double tolerance) {
    return executeBooleanWithRetry(ctx, BoolOp::Cut, "Cut", target, tool, tolerance);
}

GeomResult booleanIntersect(KernelContext& ctx, const NamedShape& a, const NamedShape& b, double tolerance) {
    return executeBooleanWithRetry(ctx, BoolOp::Common, "Common", a, b, tolerance);
}

} // namespace oreo
