// feature.cpp — Feature execution: dispatches feature type to the appropriate
// geometry operation, resolving element references along the way.

#include "feature.h"
#include "geometry/oreo_geometry.h"
#include "core/kernel_context.h"
#include "core/operation_result.h"
#include "core/diagnostic_scope.h"
#include "query/oreo_query.h"

namespace oreo {

namespace {

// Extract an OperationResult<NamedShape> from a GeomResult, setting feature status on failure.
// Propagates the GeomResult as-is since executeFeature now returns OperationResult<NamedShape>.
OperationResult<NamedShape> extractResult(Feature& feature, GeomResult&& result) {
    if (!result.ok()) {
        feature.status = FeatureStatus::ExecutionFailed;
        feature.errorMessage = result.errorMessage();
    }
    return std::move(result);
}

// Resolve a single ElementRef using the provided resolver.
// If resolution fails, sets feature status and returns empty shape.
TopoDS_Shape resolveRef(Feature& feature, const ElementRef& ref, const RefResolver& resolver) {
    auto resolved = resolver(ref);
    if (!resolved.resolved) {
        feature.status = FeatureStatus::BrokenReference;
        feature.errorMessage = "Cannot resolve reference: " + ref.elementName
                             + " from feature " + ref.featureId;
        return {};
    }
    return resolved.shape;
}

// Resolve a vector of ElementRefs to NamedEdge/NamedFace lists.
std::vector<NamedEdge> resolveEdges(Feature& feature,
                                    const std::vector<ElementRef>& refs,
                                    const RefResolver& resolver) {
    std::vector<NamedEdge> edges;
    for (auto& ref : refs) {
        auto resolved = resolver(ref);
        if (!resolved.resolved) {
            feature.status = FeatureStatus::BrokenReference;
            feature.errorMessage = "Cannot resolve edge reference: " + ref.elementName;
            return {};
        }
        edges.push_back({resolved.indexedName, resolved.shape});
    }
    return edges;
}

std::vector<NamedFace> resolveFaces(Feature& feature,
                                    const std::vector<ElementRef>& refs,
                                    const RefResolver& resolver) {
    std::vector<NamedFace> faces;
    for (auto& ref : refs) {
        auto resolved = resolver(ref);
        if (!resolved.resolved) {
            feature.status = FeatureStatus::BrokenReference;
            feature.errorMessage = "Cannot resolve face reference: " + ref.elementName;
            return {};
        }
        faces.push_back({resolved.indexedName, resolved.shape});
    }
    return faces;
}

} // anonymous namespace

OperationResult<NamedShape> executeFeature(KernelContext& ctx,
                          Feature& feature,
                          const NamedShape& currentShape,
                          const RefResolver& resolver)
{
    DiagnosticScope scope(ctx);
    feature.status = FeatureStatus::OK;
    feature.errorMessage.clear();

    if (feature.suppressed) {
        feature.status = FeatureStatus::Suppressed;
        return scope.makeResult(currentShape);  // Pass through unchanged
    }

    try {
        const std::string& type = feature.type;

        // ── Extrude ──────────────────────────────────────
        if (type == "Extrude") {
            auto dir = feature.get<gp_Vec>("direction");
            return extractResult(feature, extrude(ctx, currentShape, dir));
        }

        // ── Revolve ──────────────────────────────────────
        if (type == "Revolve") {
            auto axis = feature.get<gp_Ax1>("axis");
            double angle = feature.get<double>("angle");
            return extractResult(feature, revolve(ctx, currentShape, axis, angle));
        }

        // ── Fillet ───────────────────────────────────────
        if (type == "Fillet") {
            auto edgeRefs = feature.get<std::vector<ElementRef>>("edges");
            double radius = feature.get<double>("radius");
            auto edges = resolveEdges(feature, edgeRefs, resolver);
            if (feature.status == FeatureStatus::BrokenReference) return scope.makeFailure<NamedShape>();
            return extractResult(feature, fillet(ctx, currentShape, edges, radius));
        }

        // ── Chamfer ──────────────────────────────────────
        if (type == "Chamfer") {
            auto edgeRefs = feature.get<std::vector<ElementRef>>("edges");
            double distance = feature.get<double>("distance");
            auto edges = resolveEdges(feature, edgeRefs, resolver);
            if (feature.status == FeatureStatus::BrokenReference) return scope.makeFailure<NamedShape>();
            return extractResult(feature, chamfer(ctx, currentShape, edges, distance));
        }

        // ── Boolean Union ────────────────────────────────
        if (type == "BooleanUnion") {
            auto toolRef = feature.get<ElementRef>("tool");
            auto toolShape = resolveRef(feature, toolRef, resolver);
            if (feature.status == FeatureStatus::BrokenReference) return scope.makeFailure<NamedShape>();
            NamedShape tool(toolShape, ctx.tags.nextTag());
            return extractResult(feature, booleanUnion(ctx, currentShape, tool));
        }

        // ── Boolean Subtract ─────────────────────────────
        if (type == "BooleanSubtract") {
            auto toolRef = feature.get<ElementRef>("tool");
            auto toolShape = resolveRef(feature, toolRef, resolver);
            if (feature.status == FeatureStatus::BrokenReference) return scope.makeFailure<NamedShape>();
            NamedShape tool(toolShape, ctx.tags.nextTag());
            return extractResult(feature, booleanSubtract(ctx, currentShape, tool));
        }

        // ── Shell ────────────────────────────────────────
        if (type == "Shell") {
            auto faceRefs = feature.get<std::vector<ElementRef>>("faces");
            double thickness = feature.get<double>("thickness");
            auto faces = resolveFaces(feature, faceRefs, resolver);
            if (feature.status == FeatureStatus::BrokenReference) return scope.makeFailure<NamedShape>();
            return extractResult(feature, shell(ctx, currentShape, faces, thickness));
        }

        // ── Mirror ───────────────────────────────────────
        if (type == "Mirror") {
            auto plane = feature.get<gp_Ax2>("plane");
            return extractResult(feature, mirror(ctx, currentShape, plane));
        }

        // ── Linear Pattern ───────────────────────────────
        if (type == "LinearPattern") {
            auto dir = feature.get<gp_Vec>("direction");
            int count = feature.get<int>("count");
            double spacing = feature.get<double>("spacing");
            return extractResult(feature, patternLinear(ctx, currentShape, dir, count, spacing));
        }

        // ── Circular Pattern ─────────────────────────────
        if (type == "CircularPattern") {
            auto axis = feature.get<gp_Ax1>("axis");
            int count = feature.get<int>("count");
            double angle = feature.get<double>("angle");
            return extractResult(feature, patternCircular(ctx, currentShape, axis, count, angle));
        }

        // ── Draft ────────────────────────────────────────
        if (type == "Draft") {
            auto faceRefs = feature.get<std::vector<ElementRef>>("faces");
            double angle = feature.get<double>("angle");
            auto dir = feature.get<gp_Dir>("pullDirection");
            auto faces = resolveFaces(feature, faceRefs, resolver);
            if (feature.status == FeatureStatus::BrokenReference) return scope.makeFailure<NamedShape>();
            return extractResult(feature, draft(ctx, currentShape, faces, angle, dir));
        }

        // ── Hole ─────────────────────────────────────────
        if (type == "Hole") {
            auto faceRef = feature.get<ElementRef>("face");
            auto center = feature.get<gp_Pnt>("center");
            double diameter = feature.get<double>("diameter");
            double depth = feature.get<double>("depth");

            auto resolved = resolver(faceRef);
            if (!resolved.resolved) {
                feature.status = FeatureStatus::BrokenReference;
                feature.errorMessage = "Cannot resolve hole face reference";
                return scope.makeFailure<NamedShape>();
            }
            NamedFace face = {resolved.indexedName, resolved.shape};
            return extractResult(feature, hole(ctx, currentShape, face, center, diameter, depth));
        }

        // ── Pocket ───────────────────────────────────────
        if (type == "Pocket") {
            double depth = feature.get<double>("depth");
            auto profileRef = feature.getOr<ElementRef>("profile", ElementRef{});
            if (profileRef.isNull()) {
                feature.status = FeatureStatus::ExecutionFailed;
                feature.errorMessage = "Pocket requires a 'profile' parameter referencing a wire/face";
                return scope.makeFailure<NamedShape>();
            }
            auto profileShape = resolveRef(feature, profileRef, resolver);
            if (feature.status == FeatureStatus::BrokenReference) return scope.makeFailure<NamedShape>();
            NamedShape profile(profileShape, ctx.tags.nextTag());
            return extractResult(feature, pocket(ctx, currentShape, profile, depth));
        }

        // ── Offset ───────────────────────────────────────
        if (type == "Offset") {
            double distance = feature.get<double>("distance");
            return extractResult(feature, offset(ctx, currentShape, distance));
        }

        // ── Thicken ──────────────────────────────────────
        if (type == "Thicken") {
            double thickness = feature.get<double>("thickness");
            return extractResult(feature, thicken(ctx, currentShape, thickness));
        }

        // ── SplitBody ────────────────────────────────────
        if (type == "SplitBody") {
            auto point = feature.get<gp_Pnt>("point");
            auto dir = feature.get<gp_Dir>("normal");
            return extractResult(feature, splitBody(ctx, currentShape, gp_Pln(point, dir)));
        }

        // ── VariableFillet ───────────────────────────────
        if (type == "VariableFillet") {
            auto edgeRef = feature.get<ElementRef>("edge");
            double startR = feature.get<double>("startRadius");
            double endR = feature.get<double>("endRadius");
            auto resolved = resolver(edgeRef);
            if (!resolved.resolved) {
                feature.status = FeatureStatus::BrokenReference;
                feature.errorMessage = "Cannot resolve edge for variable fillet";
                return scope.makeFailure<NamedShape>();
            }
            NamedEdge edge = {resolved.indexedName, resolved.shape};
            return extractResult(feature, filletVariable(ctx, currentShape, edge, startR, endR));
        }

        // ── MakeFace ─────────────────────────────────────
        if (type == "MakeFace") {
            return extractResult(feature, makeFaceFromWire(ctx, currentShape));
        }

        // ── Combine ──────────────────────────────────────
        if (type == "Combine") {
            auto shapeRefs = feature.getOr<std::vector<ElementRef>>("shapes", {});
            if (shapeRefs.empty()) {
                feature.status = FeatureStatus::ExecutionFailed;
                feature.errorMessage = "Combine requires a 'shapes' parameter with references to shapes to combine";
                return scope.makeFailure<NamedShape>();
            }
            std::vector<NamedShape> shapes = {currentShape};
            for (auto& ref : shapeRefs) {
                auto resolved = resolveRef(feature, ref, resolver);
                if (feature.status == FeatureStatus::BrokenReference) return scope.makeFailure<NamedShape>();
                shapes.emplace_back(resolved, ctx.tags.nextTag());
            }
            return extractResult(feature, combine(ctx, shapes));
        }

        // ── Rib ──────────────────────────────────────────
        if (type == "Rib") {
            auto dir = feature.get<gp_Dir>("direction");
            double thickness = feature.get<double>("thickness");
            auto profileRef = feature.getOr<ElementRef>("profile", ElementRef{});
            if (profileRef.isNull()) {
                feature.status = FeatureStatus::ExecutionFailed;
                feature.errorMessage = "Rib requires a 'profile' parameter referencing a wire profile";
                return scope.makeFailure<NamedShape>();
            }
            auto profileShape = resolveRef(feature, profileRef, resolver);
            if (feature.status == FeatureStatus::BrokenReference) return scope.makeFailure<NamedShape>();
            NamedShape ribProfile(profileShape, ctx.tags.nextTag());
            return extractResult(feature, rib(ctx, currentShape, ribProfile, dir, thickness));
        }

        // ── Primitives (used as first feature) ───────────
        if (type == "MakeBox") {
            auto d = feature.get<gp_Vec>("dimensions");
            return extractResult(feature, makeBox(ctx, d.X(), d.Y(), d.Z()));
        }
        if (type == "MakeCylinder") {
            double r = feature.get<double>("radius");
            double h = feature.get<double>("height");
            return extractResult(feature, makeCylinder(ctx, r, h));
        }
        if (type == "MakeSphere") {
            double r = feature.get<double>("radius");
            return extractResult(feature, makeSphere(ctx, r));
        }

        // ── Unknown feature type ─────────────────────────
        feature.status = FeatureStatus::ExecutionFailed;
        feature.errorMessage = "Unknown feature type: " + type;
        return scope.makeFailure<NamedShape>();

    } catch (const std::bad_variant_access& e) {
        feature.status = FeatureStatus::ExecutionFailed;
        feature.errorMessage = std::string("Missing or wrong parameter type: ") + e.what();
        return scope.makeFailure<NamedShape>();
    } catch (const std::out_of_range& e) {
        feature.status = FeatureStatus::ExecutionFailed;
        feature.errorMessage = std::string("Missing parameter: ") + e.what();
        return scope.makeFailure<NamedShape>();
    } catch (const std::exception& e) {
        feature.status = FeatureStatus::ExecutionFailed;
        feature.errorMessage = std::string("Execution error: ") + e.what();
        return scope.makeFailure<NamedShape>();
    }
}

} // namespace oreo
