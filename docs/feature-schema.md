# Feature parameter schemas

Every built-in `Feature` type has a declarative parameter schema. The
schema runs **before** `executeFeature()` dispatches to OCCT, so a
malformed feature is rejected with a structured diagnostic instead of
crashing inside `std::variant::get` or producing degenerate geometry.

The full registry lives in
[`src/feature/feature_schema.h`](../src/feature/feature_schema.h) /
[`.cpp`](../src/feature/feature_schema.cpp). Test surface:
[`tests/test_feature/test_feature_schema.cpp`](../tests/test_feature/test_feature_schema.cpp).

## How it fires

```
  user calls executeFeature(ctx, feature, currentShape, resolver)
        │
        ▼
  ctx.diag scope opens, feature.status reset
        │
        ▼
  if (feature.suppressed) return currentShape    ── no validation
        │
        ▼
  validateFeature(ctx, feature)                  ◄── pre-execution gate
        │   ├── unknown type?            → INVALID_INPUT
        │   ├── required param missing?  → INVALID_INPUT
        │   ├── wrong variant index?     → INVALID_INPUT
        │   ├── numeric NaN/Inf?         → OUT_OF_RANGE
        │   ├── numeric out of [min,max]?→ OUT_OF_RANGE
        │   ├── vec components ≤ 0
        │   │   when componentsPositive? → OUT_OF_RANGE
        │   ├── ElementRef.elementType
        │   │   does not match refKind?  → INVALID_INPUT
        │   └── ElemRefList empty when
        │       listNonEmpty=true?       → INVALID_INPUT
        │
        ▼
  switch (feature.type) → dispatch to geometry op
```

Unknown extra params produce a **Warning**, not a failure, so a v1.1
feature persisted with extra fields can still be loaded by v1.0.

## Catalogue

| Feature type      | Required params                                              | Notes                                          |
|-------------------|--------------------------------------------------------------|------------------------------------------------|
| `MakeBox`         | `dimensions: Vec` (each component > 0)                       | Primitive — no base shape required             |
| `MakeCylinder`    | `radius: Double > 0`, `height: Double > 0`                   | Primitive                                      |
| `MakeSphere`      | `radius: Double > 0`                                         | Primitive                                      |
| `Extrude`         | `direction: Vec` (non-zero)                                  | Needs face/wire base                           |
| `Revolve`         | `axis: Ax1`, `angle: Double ∈ [-2π, 2π]`                     | Angle is radians; range catches deg/rad mix-ups|
| `Fillet`          | `edges: ElemRefList<Edge>`, `radius: Double > 0`             |                                                |
| `Chamfer`         | `edges: ElemRefList<Edge>`, `distance: Double > 0`           |                                                |
| `BooleanUnion`    | `tool: ElemRef`                                              | Either ref kind acceptable                     |
| `BooleanSubtract` | `tool: ElemRef`                                              |                                                |
| `Shell`           | `faces: ElemRefList<Face>`, `thickness: Double != 0`         | Sign sets shell direction                      |
| `Mirror`          | `plane: Ax2`                                                 |                                                |
| `LinearPattern`   | `direction: Vec`, `count: Int >= 1`, `spacing: Double >= 0`  |                                                |
| `CircularPattern` | `axis: Ax1`, `count: Int >= 1`, `angle: Double ∈ [-2π, 2π]`  |                                                |
| `Draft`           | `faces: ElemRefList<Face>`, `angle: Double ∈ [-89, 89]`,     | Angle in degrees                               |
|                   | `pullDirection: Dir`                                          |                                                |
| `Hole`            | `face: ElemRef<Face>`, `center: Pnt`, `diameter: Double > 0`,|                                                |
|                   | `depth: Double > 0`                                          |                                                |
| `Pocket`          | `profile: ElemRef`, `depth: Double > 0`                      |                                                |
| `Offset`          | `distance: Double != 0`                                      |                                                |
| `Thicken`         | `thickness: Double != 0`                                     |                                                |
| `SplitBody`       | `point: Pnt`, `normal: Dir`                                  |                                                |
| `VariableFillet`  | `edge: ElemRef<Edge>`, `startRadius`, `endRadius`            | Both > 0                                       |
| `MakeFace`        | (none)                                                       | Converts the wire base into a face             |
| `Combine`         | `shapes: ElemRefList` (any kind)                             | Compound build                                 |
| `Rib`             | `direction: Dir`, `thickness: Double > 0`, `profile: ElemRef`|                                                |

## Adding a new feature type

1. Add the dispatch case in
   [`src/feature/feature.cpp`](../src/feature/feature.cpp) — that is
   what wires the parsed params into an OCCT call.
2. Register the schema in `FeatureSchemaRegistry`'s constructor in
   [`feature_schema.cpp`](../src/feature/feature_schema.cpp). Use the
   convenience builders (`dbl`, `dblPositive`, `vec`, `elemRef`, etc.)
   so the registration table stays readable.
3. Add at least one validating + one rejecting test in
   [`test_feature_schema.cpp`](../tests/test_feature/test_feature_schema.cpp).
4. Document the new type in the catalogue above.

## Cloud-collab edits

The C ABI exposes the schema implicitly via the per-type setters on
`OreoFeatureBuilder` (`set_double`, `set_int`, `set_vec`, …). A server
that receives an "edit feature" request can:

* **Update a single double:**
  `oreo_ctx_feature_tree_set_param_double(tree, fid, "radius", 2.5)`
* **Update a vector:**
  `oreo_ctx_feature_tree_set_param_vec(tree, fid, "direction", 0,0,1)`
* **Validate without executing:**
  `oreo_ctx_feature_tree_validate(tree)` — runs the schema check on
  every feature and returns OK / first-error code.

Validation always re-runs at replay time (`executeFeature()` calls
`validateFeature()` first), so a "validate then replay" flow can
short-circuit early when a parameter regresses.
