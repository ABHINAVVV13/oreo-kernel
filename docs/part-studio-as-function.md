# PartStudio as a pure typed function

**Status:** Landed with this change (oreo-kernel v0.10.0).
**Supersedes:** the "PartStudio is a thin aggregate" framing in
[part-studio-model.md](part-studio-model.md). That doc is still accurate
about the internal timeline; this doc adds the function-shaped outer
contract on top of it.

## 1. Why this exists

The Onshape architecture talk states explicitly:

> *"A part studio is itself actually a FeatureScript function that
> produces a 3D model when it executes."*
> — Ilya Baran, Onshape architecture session, line 53 of
> `architecture-transcrption-of-onshape.txt`.

Three downstream capabilities fall out of that one sentence, and none
of them can be retrofitted onto an untyped JSON timeline without
breaking every stored document:

1. **Configurations** — parametric variants of a part studio. When the
   studio is a function, configurations are just *arguments*.
2. **Custom features calling part studios** — the composability that
   lets Premier Kitchen build an entire kitchen out of nested studios.
3. **FeatureScript as a frontend later** — a DSL that compiles to a
   stable runtime ABI. Without the ABI first, the DSL has nothing to
   target.

oreo-kernel v0.9.0-rc1 shipped `PartStudio` as a named wrapper around
`FeatureTree`, with no typed inputs and exactly one output (the final
`NamedShape`). This change makes it a function:

```
  PartStudio(inputs: ConfigSchema) -> Outputs { finalShape, named... }
```

## 2. Surface

### 2.1 `ConfigSchema`

Declarative list of typed inputs, one `ConfigInputSpec` per input:

```cpp
struct ConfigInputSpec {
    std::string name;          // must be unique within the schema
    ParamType   type;          // reuses the feature-schema ParamType enum
    ParamValue  defaultValue;  // must match `type`
    std::string description;
    // Numeric bounds (Double/Int only, optional)
    std::optional<double> minInclusive;
    std::optional<double> maxInclusive;
};

class ConfigSchema {
    void addInput(ConfigInputSpec spec);
    const ConfigInputSpec* find(const std::string& name) const;
    const std::vector<ConfigInputSpec>& inputs() const;
};
```

Rationale for reusing `ParamType` rather than introducing a separate
`ConfigType` enum: the set of legal shapes of data that can cross the
FFI boundary is the same for config inputs and feature params, so two
enums would be pure duplication. `ElementRef` / `ElementRefList` are
*not* valid config input types (configs are inputs to the studio, not
references into its own output) and this is enforced at
`addInput` time with an `INVALID_INPUT` diagnostic.

### 2.2 `ConfigValue`

Runtime binding of values to a `ConfigSchema`:

```cpp
class ConfigValue {
    // Constructs with every input set to its schema default.
    static ConfigValue fromSchemaDefaults(const ConfigSchema& schema);

    // Per-type setters; return false + diagnostic on name/type mismatch.
    bool setDouble(KernelContext& ctx, const std::string& name, double v);
    bool setInt   (KernelContext& ctx, const std::string& name, int v);
    // ... etc for every ParamType except ElementRef variants

    // Read: returns nullptr if unset (caller should fall back to schema default).
    const ParamValue* find(const std::string& name) const;
};
```

`ConfigValue` is data, not behaviour. It deliberately does not carry a
back-reference to the schema — callers pass `(schema, configValue)`
together when they need validation. This keeps the type cheaply copyable
and serialisable.

### 2.3 `ConfigRef` — the placeholder in feature params

Feature parameters gain a new `ParamValue` variant alternative:

```cpp
struct ConfigRef {
    std::string name;  // must match a ConfigInputSpec.name in the studio schema
};

using ParamValue = std::variant<
    double, int, bool, std::string,
    gp_Vec, gp_Pnt, gp_Dir, gp_Ax1, gp_Ax2, gp_Pln,
    ElementRef, std::vector<ElementRef>,
    ConfigRef                          // NEW — index 12
>;
```

`ParamType::ConfigRef` enters the `ParamType` enum at value `12`,
matching the variant index. The invariant *"v.index() == ParamType
integer"* in `feature_schema.cpp:validateOne` is preserved.

### 2.4 `PartStudio::execute`

The authoritative pure-function entry point:

```cpp
struct PartStudioOutputs {
    NamedShape finalShape;                         // end-of-tree shape
    std::map<std::string, NamedShape> namedParts;  // keyed on feature id
    std::vector<const Feature*> brokenFeatures;    // non-owning; into the tree
    enum class Status { Ok, Partial, Failed };
    Status status = Status::Ok;
};

class PartStudio {
    ConfigSchema&       configSchema()       noexcept;
    const ConfigSchema& configSchema() const noexcept;

    // The function. Deterministic: same (tree, schema, config) ⇒ identical
    // shape identities + topology every call.
    PartStudioOutputs execute(const ConfigValue& config);
    PartStudioOutputs executeWithDefaults() {
        return execute(ConfigValue::fromSchemaDefaults(schema_));
    }
};
```

Pre-existing `replay()` is kept as a thin alias that calls
`executeWithDefaults().finalShape` — no downstream caller breaks, and
anyone who wants the typed function contract opts in.

## 3. Execution semantics

```
  execute(config):
    ┌──────────────────────────────────────────────────────────┐
    │ 1. validateConfig(schema, config, ctx.diag)              │
    │    ├─ every required input present?                      │
    │    ├─ every value's variant index matches spec.type?     │
    │    └─ numeric bounds?                                    │
    │ 2. tree.replayWithConfig(schema, config)                 │
    │    for each feature f in tree:                           │
    │       resolved = resolveConfigRefs(f, schema, config)    │
    │       validateFeature(ctx, resolved)                     │
    │       executeFeature(ctx, resolved, currentShape, ...)   │
    │       cache[f.id] = result                               │
    │ 3. collect outputs:                                      │
    │    finalShape = last non-suppressed non-null cache entry │
    │    namedParts = every cache entry of a feature whose     │
    │                 type is "Export" (future)                │
    └──────────────────────────────────────────────────────────┘
```

### 3.1 `resolveConfigRefs`

Produces a *copy* of the feature with every `ConfigRef` param replaced
by the resolved value from the `ConfigValue`. The stored feature in the
tree is never mutated — so a single tree can be executed against many
configs without state drift.

Resolution algorithm (per param):
1. If value is not `ConfigRef`, emit unchanged.
2. If value is `ConfigRef{name}`:
   a. Find the input spec `spec = schema.find(name)`.
   b. If no spec → emit a `Feature` with `status = BrokenReference` and
      `errorMessage = "ConfigRef points to unknown input '" + name + "'"`.
      Downstream validation + execution bail on the first broken feature.
   c. If `config.find(name)` has a value → emit that value.
   d. Otherwise → emit `spec.defaultValue`.
3. A `ConfigRef` never flows through to `validateFeature` — the invariant
   "feature params are fully resolved before validation" is load-bearing.

### 3.2 Determinism

`execute` inherits every determinism property already documented in
[`part-studio-model.md §Identity stability`](part-studio-model.md):

* same docId + same resolved param values ⇒ identical `ShapeIdentity`
  allocation sequence,
* incremental replay uses `allocatorSnapshotsBefore_` to rewind,
* `test_replay_golden.cpp` already locks identity bags down.

New determinism surface added by this change:

* a fresh `ConfigValue::fromSchemaDefaults` produces bit-identical
  parameter bindings every time,
* `resolveConfigRefs` is a pure function of `(feature, schema, config)`
  and therefore replaying the same studio with the same config is
  byte-identical.

### 3.3 Status calculation

* `Ok` — every non-suppressed feature succeeded (`status == OK`).
* `Partial` — at least one feature is `BrokenReference` or
  `ExecutionFailed`, but a non-null final shape was produced.
* `Failed` — no non-null final shape (e.g. the very first primitive
  feature failed).

## 4. JSON wire format (v2)

### 4.1 Version bump

`schema::FEATURE_TREE` goes **1.0.0 → 2.0.0** because `ConfigRef` is a
new fundamental primitive in the param variant. A v1 reader would
silently turn a `ConfigRef` into an empty string and then fail the
feature schema check — fail-closed, but not informative. A v2 reader
rejects v1 documents cleanly only if they were emitted on a v1 writer
that accidentally wrote v2 content, which cannot happen (writers write
the current version).

`schema::PART_STUDIO` is introduced at `2.0.0` (there was no explicit
constant before; `part_studio.cpp` hard-coded `"oreo.part_studio"` with
the tree's version).

### 4.2 PartStudio envelope

```json
{
  "_schema":  "oreo.part_studio",
  "_version": {"major": 2, "minor": 0, "patch": 0},
  "name":     "Part 1",
  "documentId": "12379813813131293137",
  "configSchema": [
    {
      "name": "length",
      "type": "Double",
      "default": 100.0,
      "description": "Box length along +X",
      "min": 1.0
    },
    {
      "name": "material",
      "type": "String",
      "default": "aluminum"
    }
  ],
  "tree": { "features": [ ... ] }
}
```

### 4.3 ConfigRef encoding in feature params

```json
{
  "params": {
    "dimensions": { "type": "configRef", "name": "length" }
  }
}
```

The `"type": "configRef"` discriminator lives in the same namespace as
`"point"` / `"vec"` / `"ax1"` / etc. — no collision because
`"configRef"` was previously unused.

## 5. C ABI

New opaque handle:

```c
typedef struct OreoConfig_T* OreoConfig;
typedef struct OrePartStudio_T* OreoPartStudio;  // was implicit; now real
```

New functions (all ctx-aware, always-on — no legacy gate):

| Function | Purpose |
|----------|---------|
| `oreo_ctx_part_studio_create` | Create a studio bound to a ctx. |
| `oreo_ctx_part_studio_free` | Destroy a studio. |
| `oreo_ctx_part_studio_tree` | Non-owning tree handle (for add/remove features). |
| `oreo_ctx_part_studio_add_input_double` | Declare a Double config input. |
| `oreo_ctx_part_studio_add_input_int` | Declare an Int input. |
| `oreo_ctx_part_studio_add_input_bool` | Declare a Bool input. |
| `oreo_ctx_part_studio_add_input_string` | Declare a String input. |
| `oreo_ctx_part_studio_add_input_vec` | Declare a Vec input. |
| `oreo_config_create` | Create a ConfigValue populated with the studio's defaults. |
| `oreo_config_free` | Destroy a config. |
| `oreo_config_set_double` / `_int` / `_bool` / `_string` / `_vec` | Override one value. |
| `oreo_ctx_part_studio_execute` | Run — returns the final `OreoSolid` or NULL. |
| `oreo_feature_builder_set_config_ref` | Bind a feature param to a ConfigRef. |

Every function routes diagnostics through the bound context — consistent
with the rest of the ctx-aware surface ([server-safe-api.md](server-safe-api.md)).

## 6. What this does NOT do (explicit non-goals)

* **No FeatureScript DSL.** The runtime ABI is what matters. A DSL that
  compiles to this ABI is a separate project ([decision logged in
  best-next-steps; deferred](branching-merging.md) sibling).
* **No custom features defined in user code.** The feature-type
  registry is still process-internal. What this change enables is
  `PartStudio A` being *called by* `PartStudio B` as a composed unit in
  a later change — the function shape is the prerequisite.
* **No derived-feature tracking across calls.** If studio B calls
  studio A and A's shape changes, B has to be re-executed. No
  listen/invalidate infrastructure is in scope.
* **No output-hashing cache.** Determinism means a cache is *possible*
  later; the hashing + storage layer is out of scope for this change.

## 7. Migration path for existing stored documents

v1 PartStudio / FeatureTree JSON without `configSchema` loads cleanly:

* `fromJSON` detects the missing `configSchema` key, constructs an
  empty `ConfigSchema`, and loads the tree as before.
* v1 feature params never contain `ConfigRef`, so `resolveConfigRefs`
  is a no-op and `executeWithDefaults` reduces to the old `replay()`.
* The version stamp on write is always the current one (v2), so a
  round-trip promotes the document.

Tests locking this down:
`test_part_studio_function.cpp::LoadsV1DocumentsUnchanged`.
