# Part Studio model

oreo-kernel's collaboration unit is the **Part Studio**: an ordered
list of features that together describe one document. The model
intentionally mirrors Onshape's Part Studio history — a single linear
timeline with a rollback marker — because that semantic is the only
known editing model that a human and a server can both reason about
without ambiguity.

## Anatomy

A Part Studio aggregates four pieces of state:

```
┌─────────────────────────────────────────────────────┐
│                    PartStudio                       │
├─────────────────────────────────────────────────────┤
│   name         : string  (display name)             │
│   ctx          : KernelContext (units, docId, tags) │
│   tree         : FeatureTree                        │
│       ├── features_     : ordered Feature[]         │
│       ├── rollbackIndex_: int (-1 = end)            │
│       ├── cache_        : per-feature NamedShape    │
│       └── allocatorSnapshotsBefore_                 │
└─────────────────────────────────────────────────────┘
```

`PartStudio` is declared in
[`src/feature/part_studio.h`](../src/feature/part_studio.h). It is a
thin aggregate; every modeling operation is delegated to the embedded
`FeatureTree`. The point of the wrapper is to be the single typed
handle a server runtime hands to consumers.

## Lifecycle

```
[ create ctx (with documentId) ]
        │
        ▼
[ PartStudio studio(ctx, "Part 1") ]
        │
        ▼
┌───────── edit loop ──────────────┐
│  studio.tree().addFeature(f);    │
│  studio.replay();                │
│  studio.tree().updateParameter(  │
│      "F2", "radius", 2.5);       │
│  studio.replay();                │
│  studio.setRollbackIndex(2);     │
│  studio.replayToRollback();      │
└──────────────────────────────────┘
        │
        ▼
[ studio.toJSON() → cloud storage ]
```

## Ordered features + rollback

The `FeatureTree` enforces a linear order. Edits are always
expressed as ordered list ops:

| Operation                       | Helper                                                |
|---------------------------------|-------------------------------------------------------|
| Append                          | `tree.addFeature(f)`                                  |
| Insert at position              | `tree.insertFeature(idx, f)`                          |
| Remove                          | `tree.removeFeature(featureId)`                       |
| Suppress / unsuppress           | `tree.suppressFeature(featureId, true/false)`         |
| Move (reorder)                  | `tree.moveFeature(featureId, newIndex)`               |
| Replace ElementRef.featureId    | `tree.replaceReference(oldId, newId)`                 |

Every edit marks the affected feature and **everything downstream**
dirty so that the next replay rebuilds them. Per-feature allocator
snapshots (`allocatorSnapshotsBefore_`) make incremental replay
deterministic — the same feature, replayed from the same upstream
state, produces the same identities every time.

## Rollback index

`tree.setRollbackIndex(i)` marks a position in the timeline. A
rollback replay (`replayToRollback`) executes only features
`[0, i]` and treats everything after as hidden. This is the same
mechanism Onshape's "rollback bar" uses; it lets a user temporarily
view the state of the part before some downstream feature without
losing those features.

`-1` means "to the end" (no rollback).

## JSON wire format

`PartStudio::toJSON()` produces an envelope:

```json
{
  "_schema":  "oreo.part_studio",
  "_version": {"major": 1, "minor": 0, "patch": 0},
  "name":     "Part 1",
  "documentId": "12379813813131293137",
  "tree": { "features": [ ... ] }
}
```

`PartStudio::fromJSON(ctx, json)` accepts this envelope OR a bare
`FeatureTree` JSON for back-compat. On parse failure it returns
`ok=false` with a structured `error` string — never a silent empty
studio (see the auditor's "fail-closed" requirement).

## Identity stability

Identity-v2 (see [identity-model.md](identity-model.md)) guarantees
that:

* every feature execution allocates fresh `ShapeIdentity`s under the
  context's `documentId`;
* a feature replayed twice with the same upstream allocator snapshot
  produces the same identities each time (deterministic replay);
* moving a feature does not change identities of features that
  precede it in the new ordering.

The acceptance gate is
[`tests/test_integration/test_replay_golden.cpp`](../tests/test_integration/test_replay_golden.cpp),
which builds canonical trees and asserts identity bags match across
fresh contexts, repeated replays, and incremental replays.

## Concurrency

Two PartStudios bound to **different** contexts are safe to operate
on from different threads — the kernel's thread-safety contract is
"one ctx per thread" and PartStudio inherits it through its
`KernelContext` reference. Locking is the caller's responsibility if
two threads need to mutate the *same* studio (treat it like any
other thread-unsafe value type).

`tests/test_integration/test_concurrency.cpp` exercises N parallel
PartStudio-equivalent tree replays per ctx and asserts no diagnostic
or geometry cross-talk.

## Anti-patterns

* ~~**Branching the tree.**~~ **SUPERSEDED in v2** —
  see [`branching-merging.md`](branching-merging.md). Workspaces now
  branch natively; `Workspace::fork` and `threeWayMerge` are the
  supported path. The v1 "fork the document" workaround remains
  available but is no longer necessary.
* **Mutating `Feature` objects directly via `features()`.** The
  accessor returns `const&` for a reason: the cache + dirty bookkeeping
  assumes you go through `updateParameter` / `suppressFeature` /
  `moveFeature`.
* **Holding a `NamedShape` returned from `replay()` after another
  `addFeature`.** The cached entry remains valid, but the tree may
  have invalidated the dirty flag for that feature. Always re-fetch.
