# Workspaces: branching and three-way merge

**Status:** Landed with oreo-kernel v0.10.0.
**Supersedes:** the "v1 is strictly linear — fork the document" anti-pattern
note in [part-studio-model.md](part-studio-model.md). That note is no
longer current; this doc is the replacement.

## 1. Why this exists

Onshape's collaborative model rests on three concepts the transcript
lists side by side (lines 47–51):

> *"[Edits] flexible as to when they can be applied ... This simple
> flexibility forms the foundation of simultaneous editing because for
> most pairs of edits it doesn't matter which one comes first. It also
> forms the basis of branching and merging as we can take all the
> edits on one branch and apply them to another."*

The data model in oreo-kernel v0.9.0-rc1 already satisfied the
prerequisite — the feature-tree edit stream is append-only and every
edit is a minimal delta — but the only public operation on it was
linear replay. You could fork a document by creating a second
`PartStudio` with a new `documentId`, but there was no way to get the
two back together. This change adds:

* **`Workspace`** — a branch-identifying wrapper around a FeatureTree.
* **`threeWayMerge`** — computes the merged tree given a common
  ancestor and two descendants. Returns conflicts where automatic
  merge is ambiguous.
* **`applyResolutions`** — given user decisions about conflicts, emits
  the final merged tree.
* C ABI surface for all three.

## 2. Data model

### 2.1 `Workspace`

```cpp
class Workspace {
public:
    Workspace(std::shared_ptr<KernelContext> ctx, std::string name);

    const std::string& name() const noexcept;
    const std::string& parentName() const noexcept;   // empty on root

    FeatureTree&       tree()       noexcept;
    const FeatureTree& tree() const noexcept;

    // The tree snapshot at fork time. This is the common ancestor
    // for a future threeWayMerge against another Workspace forked
    // from the same parent state. Empty tree on root workspaces.
    const FeatureTree& baseSnapshot() const noexcept;

    // Fork returns a NEW workspace whose:
    //   - tree is a deep copy of *this* tree at fork time
    //   - baseSnapshot is a deep copy of *this* tree at fork time
    //   - parentName is this->name()
    //   - ctx is a shared_ptr to the SAME KernelContext (docId survives
    //     the branch — cross-branch identity stays coherent)
    std::unique_ptr<Workspace> fork(std::string newName) const;

    std::string toJSON() const;
    static WorkspaceFromJsonResult fromJSON(std::shared_ptr<KernelContext> ctx,
                                            const std::string& json);
};
```

Two branches of the same `PartStudio` share a `KernelContext` (and
therefore a `documentId`). `ShapeIdentity.counter` values **will**
diverge — each branch appends its own features and the counter sequence
is append order. That divergence is *correct*: downstream features
reference ancestors by `MappedName` (stable string), not raw counter,
so a reference written in branch A that was never deleted in the merge
resolves against the merged tree after replay. The test surface locks
this property down (§5.3).

### 2.2 `MergeConflict`

```cpp
enum class MergeConflictKind {
    ParamConflict,        // ours and theirs both modified the same param, to different values
    SuppressionConflict,  // ours and theirs set suppressed to different booleans
    AddRemoveConflict,    // added in one side, removed in the other
    PositionConflict,     // moved to different indices in each side
    TypeConflict,         // a feature with the same id has a different type on each side
};

struct MergeConflict {
    MergeConflictKind kind;
    std::string featureId;
    std::string paramName;       // empty unless kind == ParamConflict

    // Best-effort snapshots of the three versions for display / resolution.
    // For PositionConflict these hold positions encoded as Int ParamValues.
    // For TypeConflict oursValue/theirsValue hold String ParamValues naming types.
    ParamValue baseValue;
    ParamValue oursValue;
    ParamValue theirsValue;

    std::string message;         // human-readable summary
};

struct MergeResult {
    FeatureTree merged;
    std::vector<MergeConflict> conflicts;
    bool clean() const { return conflicts.empty(); }
};
```

## 3. Algorithm

### 3.1 Feature-level diff

`diff(base, branch)` classifies each feature id into one of:

| Classification   | Condition                                       |
|------------------|-------------------------------------------------|
| Unchanged        | id present in both, identical params + suppressed + type + index |
| ParamModified    | id present in both, params differ               |
| SuppressToggled  | id present in both, suppressed flag differs     |
| Moved            | id present in both, index differs               |
| TypeChanged      | id present in both, type differs                |
| Added            | id present in branch, absent in base            |
| Removed          | id present in base, absent in branch            |

The diff uses ordered ids (features carry unique string ids assigned
by the caller). Features are compared by **id**, not by array position
— moving a feature changes its index, not its identity.

### 3.2 Three-way merge

Two passes:

**Pass 1 — merge present features.**
For every feature id that appears in at least two of {base, ours, theirs}:

```
  classify change on OURS side:   oursKind
  classify change on THEIRS side: theirsKind

  if both Unchanged: keep base's feature.
  if one Unchanged: take the change from the other side.
  if both identical changes: apply once (no conflict).
  otherwise: walk the conflict table below.
```

Conflict table (rows = ours change kind, columns = theirs change kind;
`—` = no conflict, `Kind` = emit conflict of that kind):

|                   | ParamMod       | Suppr        | Moved         | TypeCh   | Added | Removed        |
|-------------------|----------------|--------------|---------------|----------|-------|----------------|
| **ParamMod**      | ParamConflict† | —            | —             | TypeC    | n/a   | AddRemoveC     |
| **Suppr**         | —              | Suppressionx | —             | TypeC    | n/a   | AddRemoveC     |
| **Moved**         | —              | —            | PositionC†    | TypeC    | n/a   | AddRemoveC     |
| **TypeCh**        | TypeC          | TypeC        | TypeC         | TypeC†   | n/a   | AddRemoveC     |
| **Added**         | n/a            | n/a          | n/a           | n/a      | clean‡| n/a            |
| **Removed**       | AddRemoveC     | AddRemoveC   | AddRemoveC    | AddRemoveC| n/a  | —              |

† conflict only when both sides produced *different* results; identical
changes merge silently.

‡ both sides added the same id — if the feature content is identical
  the feature merges in cleanly; otherwise TypeConflict or ParamConflict
  depending on what differs.

**Pass 2 — merge feature ordering.**

The canonical ordering of the merged tree is derived by:

1. Start with `base`'s order as the reference sequence.
2. For every feature still present in the merged tree:
   * If only one side moved it, adopt that side's index (relative to the
     still-present neighbours).
   * If both sides moved it and they disagree, emit `PositionConflict`
     and leave the feature at base's position.
   * If only one side *added* it, insert at that side's requested index.
   * If both sides added it with different indices, insert at the
     smaller index and emit `PositionConflict`.

The net effect: for disjoint non-conflicting edits the merged tree
behaves exactly like a `git merge` on two branches that touched
different features — clean merge. For overlapping edits, conflicts
surface at the point where human judgment is required.

### 3.3 `applyResolutions`

```cpp
enum class ResolveChoice { Ours, Theirs, Base, Custom };

struct Resolution {
    std::string featureId;
    std::string paramName;     // "" for non-ParamConflict kinds
    ResolveChoice choice;
    ParamValue   customValue;  // used when choice == Custom
};

FeatureTree applyResolutions(const FeatureTree& mergedBase,
                             const std::vector<MergeConflict>& conflicts,
                             const std::vector<Resolution>& resolutions);
```

Every conflict in `conflicts` must be matched by exactly one
`Resolution` with matching `featureId` (+ `paramName` where relevant).
Unmatched conflicts leave the feature in its pre-conflict state — a
diagnostic surfaces, and the returned tree will *still have the
unresolved conflict state*.

## 4. JSON wire format

### 4.1 Workspace envelope

```json
{
  "_schema":  "oreo.workspace",
  "_version": {"major": 1, "minor": 0, "patch": 0},
  "name":     "feature/skirt-redesign",
  "parentName": "main",
  "studio":   { /* nested PartStudio envelope — see part-studio-as-function.md §4.2 */ },
  "baseSnapshot": { /* FeatureTree JSON at fork time — same schema as studio.tree */ }
}
```

`schema::WORKSPACE = {1,0,0}` (new). `baseSnapshot` is omitted on root
workspaces.

### 4.2 MergeResult envelope

Merge results are not typically persisted — they exist to be
immediately resolved. For diagnostic / replay purposes there is a
round-trippable form:

```json
{
  "_schema":  "oreo.merge_result",
  "_version": {"major": 1, "minor": 0, "patch": 0},
  "merged":   { /* FeatureTree JSON */ },
  "conflicts": [
    {
      "kind": "ParamConflict",
      "featureId": "F3",
      "paramName": "radius",
      "base":   { "type": "double", "value": 2.0 },
      "ours":   { "type": "double", "value": 3.0 },
      "theirs": { "type": "double", "value": 2.5 },
      "message": "..."
    }
  ]
}
```

## 5. Acceptance guarantees

A change is not *done* per the project's production measurement bar
until the guarantees below hold end-to-end, verified by adversarial
tests.

### 5.1 Clean merge is commutative-ish

For two forks that touched *disjoint* features:

```
  merge(base, A, B).merged  ≡  merge(base, B, A).merged
```

(≡ = same feature ids in the same order with the same params). Locked
down by `test_branch_merge.cpp::CleanMergeIsCommutative`.

### 5.2 Conflicting merges fail-closed

No merge with conflicts produces a silently-corrupt tree. Either:
* every conflict appears in `MergeResult.conflicts`, or
* the caller's `applyResolutions` populated every conflict, and the
  returned tree replays to a valid shape.

No path in the merge code produces a tree that has applied only *some*
of a conflicted feature's changes without surfacing a conflict.

### 5.3 Identity coherence across branches

Two workspaces sharing a `KernelContext` share `documentId`. A merged
tree replayed on that same context allocates `ShapeIdentity` values
under the shared `documentId`. Consequence: exported shapes from `main`
before and after a merge from `feature/X` look like they come from the
same document. Verified by
`test_branch_merge.cpp::MergedTreeIdentityStaysInSameDocument`.

### 5.4 Merge + replay = round-trippable

```
  merged = threeWayMerge(base, ours, theirs)
  studio.tree() = merged.merged
  output = studio.executeWithDefaults()
  output.status in {Ok, Partial}  // never Failed on a clean merge
```

Locked down by `test_branch_merge.cpp::CleanMergeRepliesSuccessfully`.

## 6. Non-goals (explicit)

* **No operational transforms.** The edit stream is diff-based, not
  log-based. Real-time collab (concurrent keystrokes) needs an explicit
  log, which is a separate project.
* **No tree-level history / commits / refs.** A workspace knows its
  parent *name* and its baseSnapshot, but there's no commit graph.
  Higher-level VCS-style semantics are out of scope.
* **No cross-document merge.** Two workspaces with different
  `documentId` cannot merge — shape identities would alias. The API
  returns `OREO_INVALID_INPUT` on that attempt.
* **No automatic conflict resolution strategies.** "Take ours" /
  "take theirs" / "take base" / "custom value" are the four user-facing
  choices. Anything smarter (last-write-wins, three-way text merge on
  strings, etc.) is out of scope.

## 7. C ABI

The ABI mirrors the C++ surface documented in §2–§3 — including
`applyResolutions`, which lets an app driving the kernel from a
non-C++ language round-trip merge→pick→finalize end to end without
touching `merge.h` directly.

```c
typedef struct OreoWorkspace_T*     OreoWorkspace;
typedef struct OreoMergeResult_T*   OreoMergeResult;
typedef struct OreoResolutionSet_T* OreoResolutionSet;

// Lifecycle
OREO_API OreoWorkspace   oreo_ctx_workspace_create(OreoContext ctx, const char* name);
OREO_API void            oreo_ctx_workspace_free  (OreoWorkspace ws);

// Branch
OREO_API OreoWorkspace   oreo_ctx_workspace_fork  (OreoWorkspace ws, const char* newName);

// Tree accessor (non-owning — owned by ws)
OREO_API OreoFeatureTree oreo_ctx_workspace_tree  (OreoWorkspace ws);

// Three-way merge. `base` is the common ancestor, typically taken as
// ours.baseSnapshot or theirs.baseSnapshot; passing a value that is NOT
// an ancestor of BOTH ours and theirs yields undefined results (the
// algorithm assumes ancestor-of-both and does not verify).
OREO_API OreoMergeResult oreo_ctx_workspace_merge(OreoWorkspace base,
                                                   OreoWorkspace ours,
                                                   OreoWorkspace theirs);

// Merge result — inspection
OREO_API int             oreo_merge_result_is_clean        (OreoMergeResult mr);
OREO_API int             oreo_merge_result_conflict_count  (OreoMergeResult mr);
OREO_API int             oreo_merge_result_conflict_kind   (OreoMergeResult mr, int i);
OREO_API int             oreo_merge_result_conflict_feature_id(OreoMergeResult mr,
                                                                int i, char* buf,
                                                                size_t buflen,
                                                                size_t* needed);
OREO_API int             oreo_merge_result_conflict_param_name(OreoMergeResult mr,
                                                                int i, char* buf,
                                                                size_t buflen,
                                                                size_t* needed);
OREO_API int             oreo_merge_result_conflict_message (OreoMergeResult mr,
                                                              int i, char* buf,
                                                              size_t buflen,
                                                              size_t* needed);
// Non-owning — the merge result owns the tree it exposes here.
OREO_API OreoFeatureTree oreo_merge_result_tree            (OreoMergeResult mr);
OREO_API void            oreo_merge_result_free            (OreoMergeResult mr);

// Resolution set — build up user decisions, then apply.
OREO_API OreoResolutionSet oreo_resolution_set_create(void);
OREO_API void              oreo_resolution_set_free(OreoResolutionSet rs);
OREO_API int               oreo_resolution_set_size(OreoResolutionSet rs);

// `choice`: 0 = Ours, 1 = Theirs, 2 = Base, 3 = Custom.
// paramName may be NULL (treated as "") for non-ParamConflict kinds.
OREO_API int oreo_resolution_set_add(OreoResolutionSet rs,
                                      const char* featureId,
                                      const char* paramName,
                                      int choice);

// For Custom (choice=3) resolutions, attach the value after the add().
// The setter must match the conflict's ParamValue variant — use the
// same kind of setter you would use on OreoFeatureBuilder for that param.
OREO_API int oreo_resolution_set_last_set_double(OreoResolutionSet rs, double v);
OREO_API int oreo_resolution_set_last_set_int   (OreoResolutionSet rs, int v);
OREO_API int oreo_resolution_set_last_set_bool  (OreoResolutionSet rs, int v);
OREO_API int oreo_resolution_set_last_set_string(OreoResolutionSet rs, const char* v);
OREO_API int oreo_resolution_set_last_set_vec   (OreoResolutionSet rs,
                                                  double x, double y, double z);

// Apply resolutions to the merge result. Returns a freshly-OWNED
// OreoFeatureTree the caller must free with oreo_ctx_feature_tree_free.
// The returned tree is independent of `mr`; `mr` remains in its
// pre-resolution state.
OREO_API OreoFeatureTree oreo_merge_result_apply_resolutions(
    OreoMergeResult mr,
    OreoResolutionSet rs);
```

No legacy-API gate — workspaces are explicitly a multi-context concept
and simply don't exist in the singleton-context legacy surface.
