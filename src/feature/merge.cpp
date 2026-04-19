// SPDX-License-Identifier: LGPL-2.1-or-later

// merge.cpp — three-way merge of FeatureTree snapshots.
//
// Design:
//   1. Index features by id (base, ours, theirs).
//   2. For each id appearing in any side, classify the change from
//      base → ours and base → theirs, combine per the rule table in
//      docs/branching-merging.md §3, and either apply or record a
//      conflict.
//   3. For feature ordering, favour the base order for surviving
//      features; added-only features append in the order they appear
//      on their side.
//
// The implementation is plain code — no OCCT calls, no geometry —
// because merge is a structural operation over feature metadata. Replay
// happens at the caller, on the resulting tree.

#include "merge.h"
#include "param_json.h"
#include "core/diagnostic.h"

#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace oreo {

namespace {

// Slow but general equality: two ParamValues are equal iff their
// canonical JSON encoding is byte-identical. Acceptable because merge
// is not a hot path — the win is that we don't have to special-case
// OCCT geometric types (which lack operator==).
bool paramValueEquals(const ParamValue& a, const ParamValue& b) {
    return paramValueToJson(a) == paramValueToJson(b);
}

// Two features are content-equal when params + suppression + type
// match. Id and status are meta-state; they are explicitly excluded.
bool featureContentEquals(const Feature& a, const Feature& b) {
    if (a.type != b.type) return false;
    if (a.suppressed != b.suppressed) return false;
    if (a.params.size() != b.params.size()) return false;
    for (const auto& kv : a.params) {
        auto it = b.params.find(kv.first);
        if (it == b.params.end()) return false;
        if (!paramValueEquals(kv.second, it->second)) return false;
    }
    return true;
}

// Index a feature vector by id. Duplicate ids in the input are a
// bug upstream; we silently keep the last occurrence (the caller's
// validation is responsible for rejecting duplicates earlier).
std::unordered_map<std::string, std::size_t>
indexById(const std::vector<Feature>& feats) {
    std::unordered_map<std::string, std::size_t> map;
    map.reserve(feats.size());
    for (std::size_t i = 0; i < feats.size(); ++i) {
        map[feats[i].id] = i;
    }
    return map;
}

// Helper: build a conflict.
MergeConflict makeConflict(MergeConflictKind kind,
                           std::string featureId,
                           std::string paramName,
                           ParamValue baseV,
                           ParamValue oursV,
                           ParamValue theirsV,
                           std::string msg) {
    MergeConflict c;
    c.kind        = kind;
    c.featureId   = std::move(featureId);
    c.paramName   = std::move(paramName);
    c.baseValue   = std::move(baseV);
    c.oursValue   = std::move(oursV);
    c.theirsValue = std::move(theirsV);
    c.message     = std::move(msg);
    return c;
}

// Helper: build an AddRemoveConflict complete with the feature
// snapshots needed by applyResolutions. Either oursF or theirsF is
// null — the non-null one is the "modified" side that survives the
// per-side diff; the null one is the "removed" side. baseF is the
// common ancestor from which both sides diverged.
MergeConflict makeAddRemoveConflict(const std::string& id,
                                    const Feature* baseF,
                                    const Feature* oursF,
                                    const Feature* theirsF,
                                    int insertIndexHint,
                                    const std::string& msg) {
    MergeConflict c;
    c.kind      = MergeConflictKind::AddRemoveConflict;
    c.featureId = id;
    c.baseValue = ParamValue{std::string{}};
    c.oursValue   = ParamValue{std::string(oursF   ? "<modified>" : "<removed>")};
    c.theirsValue = ParamValue{std::string(theirsF ? "<modified>" : "<removed>")};
    if (baseF)   c.baseSnapshot   = *baseF;
    if (oursF)   c.oursSnapshot   = *oursF;
    if (theirsF) c.theirsSnapshot = *theirsF;
    c.insertIndexHint = insertIndexHint;
    c.message = msg;
    return c;
}

// Merge a single param (name, maybeBase, maybeOurs, maybeTheirs).
// Outputs:
//   - outValue: final value to place in the merged feature (may be
//     "unset" meaning "delete this param")
//   - outUnset: if true, the param should be absent in merged
//   - conflictOut: populated iff this merge produced a conflict; then
//     the caller should record it and keep the base value in merged.
// Returns true if a conflict was produced.
struct ParamMergeOutcome {
    bool        exists;     // if false, param should be deleted in merged
    ParamValue  value;
    bool        conflict;
    ParamValue  baseV;
    ParamValue  oursV;
    ParamValue  theirsV;
};

ParamMergeOutcome mergeOneParam(const ParamValue* base,
                                const ParamValue* ours,
                                const ParamValue* theirs) {
    ParamMergeOutcome out;
    out.conflict = false;

    // Helper: value-or-default for building the conflict record.
    auto v = [](const ParamValue* p) -> ParamValue {
        return p ? *p : ParamValue{std::string{}};
    };

    bool oursChanged   = (base == nullptr) ? (ours   != nullptr)
                       : (ours == nullptr) ? true
                       : !paramValueEquals(*base, *ours);
    bool theirsChanged = (base == nullptr) ? (theirs != nullptr)
                       : (theirs == nullptr) ? true
                       : !paramValueEquals(*base, *theirs);

    if (!oursChanged && !theirsChanged) {
        // Unchanged: keep base (or nothing).
        if (base) { out.exists = true; out.value = *base; }
        else      { out.exists = false; }
        return out;
    }
    if (oursChanged && !theirsChanged) {
        if (ours) { out.exists = true;  out.value = *ours; }
        else      { out.exists = false; }
        return out;
    }
    if (!oursChanged && theirsChanged) {
        if (theirs) { out.exists = true;  out.value = *theirs; }
        else        { out.exists = false; }
        return out;
    }
    // Both changed.
    bool oursGone   = (ours   == nullptr);
    bool theirsGone = (theirs == nullptr);
    bool samePresence = (oursGone == theirsGone);
    if (samePresence) {
        if (oursGone) { out.exists = false; return out; }  // both deleted
        if (paramValueEquals(*ours, *theirs)) {
            out.exists = true; out.value = *ours; return out;
        }
    }
    // Conflict — neither clean-apply path matched.
    out.conflict = true;
    out.baseV    = v(base);
    out.oursV    = v(ours);
    out.theirsV  = v(theirs);
    // Keep base value in merged (or empty string sentinel if no base).
    if (base) { out.exists = true; out.value = *base; }
    else      { out.exists = false; }
    return out;
}

} // anonymous namespace

// ─── mergeConflictKindName ───────────────────────────────────────

const char* mergeConflictKindName(MergeConflictKind k) {
    switch (k) {
        case MergeConflictKind::ParamConflict:       return "ParamConflict";
        case MergeConflictKind::SuppressionConflict: return "SuppressionConflict";
        case MergeConflictKind::AddRemoveConflict:   return "AddRemoveConflict";
        case MergeConflictKind::PositionConflict:    return "PositionConflict";
        case MergeConflictKind::TypeConflict:        return "TypeConflict";
    }
    return "Unknown";
}

// ─── threeWayMerge ───────────────────────────────────────────────

MergeResult threeWayMerge(std::shared_ptr<KernelContext> ctx,
                          const std::vector<Feature>& base,
                          const std::vector<Feature>& ours,
                          const std::vector<Feature>& theirs) {
    MergeResult result(ctx);

    auto baseIdx   = indexById(base);
    auto oursIdx   = indexById(ours);
    auto theirsIdx = indexById(theirs);

    // ─── Pass 1: compute every feature id's fate ──────────────
    //
    // For each id, decide:
    //   - do we keep it in merged?
    //   - what's its content (params + suppression + type)?
    //   - collect conflicts for the user.
    //
    // We store the merged Feature by id in `mergedById` and materialise
    // the ordered vector in pass 2.
    // Sole source of truth for "did this id survive the merge?":
    // ordering pass 2 reads `mergedById.count(id)` below. Any earlier
    // "also tracked in deletedIds" set is dead weight; features that
    // don't make it into mergedById are dropped.
    std::unordered_map<std::string, Feature> mergedById;

    // Build the union of ids. Iteration order = base order, then ours-
    // only new ids in ours order, then theirs-only new ids in theirs order.
    std::vector<std::string> orderedIds;
    std::unordered_set<std::string> seen;
    orderedIds.reserve(base.size() + ours.size() + theirs.size());
    for (const auto& f : base) {
        if (seen.insert(f.id).second) orderedIds.push_back(f.id);
    }
    for (const auto& f : ours) {
        if (seen.insert(f.id).second) orderedIds.push_back(f.id);
    }
    for (const auto& f : theirs) {
        if (seen.insert(f.id).second) orderedIds.push_back(f.id);
    }

    for (const auto& id : orderedIds) {
        const Feature* baseF   = baseIdx.count(id)   ? &base[baseIdx[id]]       : nullptr;
        const Feature* oursF   = oursIdx.count(id)   ? &ours[oursIdx[id]]       : nullptr;
        const Feature* theirsF = theirsIdx.count(id) ? &theirs[theirsIdx[id]]   : nullptr;

        // ── Added-only cases ─────────────────────────────────
        if (!baseF && oursF && !theirsF) {
            mergedById[id] = *oursF;
            continue;
        }
        if (!baseF && !oursF && theirsF) {
            mergedById[id] = *theirsF;
            continue;
        }

        // ── Added on both sides ──────────────────────────────
        if (!baseF && oursF && theirsF) {
            // If identical, clean merge. Else conflict.
            if (featureContentEquals(*oursF, *theirsF)) {
                mergedById[id] = *oursF;
                continue;
            }
            if (oursF->type != theirsF->type) {
                result.conflicts.push_back(makeConflict(
                    MergeConflictKind::TypeConflict, id, {},
                    ParamValue{std::string{}},
                    ParamValue{oursF->type},
                    ParamValue{theirsF->type},
                    "Feature '" + id + "' added independently on both sides "
                    "with different types: ours='" + oursF->type
                    + "', theirs='" + theirsF->type + "'"));
                // Pick ours — an arbitrary deterministic choice.
                mergedById[id] = *oursF;
                continue;
            }
            // Same type, different content → record conflicts against
            // the ours version and keep ours as the seed for merged.
            // (Arbitrary deterministic choice — caller can swap via
            // applyResolutions with ResolveChoice::Theirs.)
            Feature merged = *oursF;
            // Suppression
            if (oursF->suppressed != theirsF->suppressed) {
                result.conflicts.push_back(makeConflict(
                    MergeConflictKind::SuppressionConflict, id, {},
                    ParamValue{oursF->suppressed},
                    ParamValue{oursF->suppressed},
                    ParamValue{theirsF->suppressed},
                    "Feature '" + id + "' added with different suppressed flags"));
            }
            // Params (treat synthBase == oursF as the "reference" so
            // only theirs's divergences show up as conflicts; both
            // sides' values populate the conflict record).
            std::unordered_set<std::string> paramNames;
            for (const auto& kv : oursF->params) paramNames.insert(kv.first);
            for (const auto& kv : theirsF->params) paramNames.insert(kv.first);
            for (const auto& pn : paramNames) {
                auto oit = oursF->params.find(pn);
                auto tit = theirsF->params.find(pn);
                const ParamValue* ov = (oit != oursF->params.end()) ? &oit->second : nullptr;
                const ParamValue* tv = (tit != theirsF->params.end()) ? &tit->second : nullptr;
                if (ov && tv && paramValueEquals(*ov, *tv)) continue;
                // Divergence in a both-added feature = ParamConflict
                result.conflicts.push_back(makeConflict(
                    MergeConflictKind::ParamConflict, id, pn,
                    ov ? *ov : ParamValue{std::string{}},
                    ov ? *ov : ParamValue{std::string{}},
                    tv ? *tv : ParamValue{std::string{}},
                    "Feature '" + id + "' (both added): param '" + pn
                    + "' differs between ours and theirs"));
            }
            mergedById[id] = merged;  // defaults to ours
            continue;
        }

        // ── Removed on one side ──────────────────────────────
        //
        // When one side removed a feature and the other modified it,
        // we cannot pick a winner automatically. The pre-fix code
        // silently deleted the feature and left applyResolutions with
        // no way to restore the modified side. Now we:
        //   (a) include both feature snapshots in the conflict, so
        //       applyResolutions can reinsert either side;
        //   (b) keep the BASE version in merged (if any) as the
        //       conservative default, matching the ParamConflict
        //       pattern: "pre-merged tree holds base; resolution
        //       picks a side explicitly."
        //
        // insertIndexHint = base position — applyResolutions uses it
        // when it needs to re-insert after the merge stripped the
        // feature because the resolution chose removal+reinsert of
        // a different version.
        auto baseIdxOf = [&](const std::string& fid) -> int {
            auto bit = baseIdx.find(fid);
            return bit == baseIdx.end() ? -1
                                        : static_cast<int>(bit->second);
        };

        if (baseF && !oursF && theirsF) {
            if (featureContentEquals(*baseF, *theirsF)) {
                // Clean delete: theirs didn't change what ours removed,
                // so honour the removal. Feature does not enter mergedById.
                continue;
            }
            result.conflicts.push_back(makeAddRemoveConflict(
                id, baseF, /*oursF=*/nullptr, theirsF,
                baseIdxOf(id),
                "Feature '" + id + "': ours removed, theirs modified"));
            // Conservative default: keep base state in merged (same
            // policy as ParamConflict). applyResolutions overrides.
            mergedById[id] = *baseF;
            continue;
        }
        if (baseF && oursF && !theirsF) {
            if (featureContentEquals(*baseF, *oursF)) {
                continue;  // clean delete (theirs removed, ours unchanged)
            }
            result.conflicts.push_back(makeAddRemoveConflict(
                id, baseF, oursF, /*theirsF=*/nullptr,
                baseIdxOf(id),
                "Feature '" + id + "': theirs removed, ours modified"));
            mergedById[id] = *baseF;
            continue;
        }
        if (baseF && !oursF && !theirsF) {
            // Both deleted — drop by not inserting into mergedById.
            continue;
        }

        // ── Present in all three → merge content ────────────
        if (baseF && oursF && theirsF) {
            Feature merged = *baseF;  // start from base

            // Type: if it changed on either side (rare), conflict.
            if (oursF->type != baseF->type && theirsF->type != baseF->type) {
                if (oursF->type == theirsF->type) {
                    merged.type = oursF->type;  // same new type on both sides
                } else {
                    result.conflicts.push_back(makeConflict(
                        MergeConflictKind::TypeConflict, id, {},
                        ParamValue{baseF->type},
                        ParamValue{oursF->type},
                        ParamValue{theirsF->type},
                        "Feature '" + id + "' type changed differently on each side"));
                    merged.type = baseF->type;
                }
            } else if (oursF->type != baseF->type) {
                merged.type = oursF->type;
            } else if (theirsF->type != baseF->type) {
                merged.type = theirsF->type;
            }

            // Suppression.
            bool oursSupChanged   = (oursF->suppressed != baseF->suppressed);
            bool theirsSupChanged = (theirsF->suppressed != baseF->suppressed);
            if (oursSupChanged && theirsSupChanged &&
                oursF->suppressed != theirsF->suppressed) {
                result.conflicts.push_back(makeConflict(
                    MergeConflictKind::SuppressionConflict, id, {},
                    ParamValue{baseF->suppressed},
                    ParamValue{oursF->suppressed},
                    ParamValue{theirsF->suppressed},
                    "Feature '" + id + "': suppressed flag toggled differently"));
                merged.suppressed = baseF->suppressed;
            } else if (oursSupChanged) {
                merged.suppressed = oursF->suppressed;
            } else if (theirsSupChanged) {
                merged.suppressed = theirsF->suppressed;
            }

            // Params — walk every name that appears anywhere.
            std::unordered_set<std::string> paramNames;
            for (const auto& kv : baseF->params)   paramNames.insert(kv.first);
            for (const auto& kv : oursF->params)   paramNames.insert(kv.first);
            for (const auto& kv : theirsF->params) paramNames.insert(kv.first);

            for (const auto& pn : paramNames) {
                auto bit = baseF->params.find(pn);
                auto oit = oursF->params.find(pn);
                auto tit = theirsF->params.find(pn);
                const ParamValue* bv = (bit != baseF->params.end())   ? &bit->second : nullptr;
                const ParamValue* ov = (oit != oursF->params.end())   ? &oit->second : nullptr;
                const ParamValue* tv = (tit != theirsF->params.end()) ? &tit->second : nullptr;

                ParamMergeOutcome pmo = mergeOneParam(bv, ov, tv);
                if (pmo.conflict) {
                    result.conflicts.push_back(makeConflict(
                        MergeConflictKind::ParamConflict, id, pn,
                        pmo.baseV, pmo.oursV, pmo.theirsV,
                        "Feature '" + id + "': param '" + pn
                        + "' modified differently on each side"));
                    // Merged keeps the base value (already in merged.params
                    // via the `merged = *baseF` copy).
                    continue;
                }
                if (pmo.exists) {
                    merged.params[pn] = pmo.value;
                } else {
                    merged.params.erase(pn);
                }
            }
            mergedById[id] = std::move(merged);
            continue;
        }
    }

    // ─── Pass 2: compute the merged feature ordering ──────────
    //
    // Ordering rules (v1):
    //   1. Retained base features appear in base order, UNLESS both
    //      ours and theirs agreed on moving them to the same new
    //      position (relative to other retained base features).
    //   2. If only one side moved a retained feature, adopt that side's
    //      relative position.
    //   3. If both sides moved a retained feature to different indices,
    //      emit PositionConflict and keep base order.
    //   4. Features added only in ours are appended in ours-order after
    //      all base-retained features.
    //   5. Features added only in theirs are appended in theirs-order
    //      after the ours-added block.
    //   6. Features added in both (if the content merged cleanly) appear
    //      in ours' requested position; the conflict for different
    //      additions has already been recorded.

    // Start from base's order for retained features.
    std::vector<std::string> mergedOrder;
    mergedOrder.reserve(mergedById.size());
    std::unordered_set<std::string> placed;

    // 1+2+3: place retained base features.
    //
    // Compute each side's relative index among retained base features.
    std::vector<std::string> retainedBaseIds;
    retainedBaseIds.reserve(base.size());
    for (const auto& f : base) {
        if (mergedById.count(f.id)) retainedBaseIds.push_back(f.id);
    }
    // For each retained-base id, look up its position on ours/theirs
    // if present (the feature must be present; otherwise we classified
    // it as deleted). We use the relative position within retained-base
    // ids on each side to detect moves.
    auto relativeIndex = [&](const std::vector<Feature>& side) {
        std::unordered_map<std::string, std::size_t> rel;
        std::size_t k = 0;
        for (const auto& f : side) {
            if (!mergedById.count(f.id)) continue;
            if (std::find(retainedBaseIds.begin(), retainedBaseIds.end(),
                          f.id) == retainedBaseIds.end()) continue;
            rel[f.id] = k++;
        }
        return rel;
    };
    auto oursRel   = relativeIndex(ours);
    auto theirsRel = relativeIndex(theirs);

    // Detect position conflicts.
    for (std::size_t i = 0; i < retainedBaseIds.size(); ++i) {
        const auto& id = retainedBaseIds[i];
        bool oursMoved   = oursRel.count(id)   && oursRel[id]   != i;
        bool theirsMoved = theirsRel.count(id) && theirsRel[id] != i;
        if (oursMoved && theirsMoved && oursRel[id] != theirsRel[id]) {
            result.conflicts.push_back(makeConflict(
                MergeConflictKind::PositionConflict, id, {},
                ParamValue{static_cast<int>(i)},
                ParamValue{static_cast<int>(oursRel[id])},
                ParamValue{static_cast<int>(theirsRel[id])},
                "Feature '" + id + "': moved to different positions on each side"));
        }
    }
    // For v1, always place retained base features in base order
    // (position conflicts just surface a warning). This is the simple,
    // deterministic choice.
    for (const auto& id : retainedBaseIds) {
        mergedOrder.push_back(id);
        placed.insert(id);
    }

    // 4: append ours-only additions in ours order.
    for (const auto& f : ours) {
        if (placed.count(f.id)) continue;
        if (!mergedById.count(f.id)) continue;
        mergedOrder.push_back(f.id);
        placed.insert(f.id);
    }
    // 5: append theirs-only additions in theirs order.
    for (const auto& f : theirs) {
        if (placed.count(f.id)) continue;
        if (!mergedById.count(f.id)) continue;
        mergedOrder.push_back(f.id);
        placed.insert(f.id);
    }

    // ─── Pass 3: materialise into the live FeatureTree ────────

    for (const auto& id : mergedOrder) {
        // mergedOrder/placed already deduplicate by id, so the
        // [[nodiscard]] return is always true. Defend the invariant
        // anyway — a silent false here would mean an upstream bug.
        if (!result.merged.addFeature(mergedById[id])) {
            // Can only happen if mergedOrder violates the uniqueness
            // invariant maintained by the placed set above.
            result.merged.context().diag().error(
                ErrorCode::INTERNAL_ERROR,
                "threeWayMerge: merged tree rejected id '" + id
                + "' — internal ordering invariant violated");
            break;
        }
    }
    return result;
}

// ─── applyResolutions ─────────────────────────────────────────────

std::vector<Feature> applyResolutions(KernelContext& ctx,
                                      const std::vector<Feature>& mergedFeatures,
                                      const std::vector<MergeConflict>& conflicts,
                                      const std::vector<Resolution>& resolutions) {
    // Build a lookup (featureId + paramName) → Resolution.
    struct KeyHash {
        std::size_t operator()(const std::pair<std::string, std::string>& p) const noexcept {
            return std::hash<std::string>{}(p.first) * 31u
                 + std::hash<std::string>{}(p.second);
        }
    };
    std::unordered_map<std::pair<std::string, std::string>,
                       const Resolution*, KeyHash> byKey;
    byKey.reserve(resolutions.size());
    for (const auto& r : resolutions) {
        byKey[{r.featureId, r.paramName}] = &r;
    }

    // Build a mutable working copy of the merged features.
    std::vector<Feature> out = mergedFeatures;
    auto findFeature = [&](const std::string& id) -> Feature* {
        for (auto& f : out) if (f.id == id) return &f;
        return nullptr;
    };

    for (const auto& c : conflicts) {
        auto it = byKey.find({c.featureId, c.paramName});
        if (it == byKey.end()) {
            ctx.diag().warning(ErrorCode::INVALID_INPUT,
                std::string("applyResolutions: no resolution supplied for ")
                + mergeConflictKindName(c.kind) + " on feature '"
                + c.featureId
                + (c.paramName.empty() ? "" : "' param '" + c.paramName) + "'");
            continue;
        }
        const Resolution* r = it->second;
        Feature* f = findFeature(c.featureId);

        switch (c.kind) {
            case MergeConflictKind::ParamConflict: {
                if (!f) break;
                ParamValue chosen;
                switch (r->choice) {
                    case ResolveChoice::Ours:   chosen = c.oursValue;   break;
                    case ResolveChoice::Theirs: chosen = c.theirsValue; break;
                    case ResolveChoice::Base:   chosen = c.baseValue;   break;
                    case ResolveChoice::Custom: chosen = r->customValue; break;
                }
                f->params[c.paramName] = chosen;
                break;
            }
            case MergeConflictKind::SuppressionConflict: {
                if (!f) break;
                bool chosen;
                switch (r->choice) {
                    case ResolveChoice::Ours:
                        chosen = std::get<bool>(c.oursValue); break;
                    case ResolveChoice::Theirs:
                        chosen = std::get<bool>(c.theirsValue); break;
                    case ResolveChoice::Base:
                        chosen = std::get<bool>(c.baseValue); break;
                    case ResolveChoice::Custom:
                        chosen = std::get<bool>(r->customValue); break;
                }
                f->suppressed = chosen;
                break;
            }
            case MergeConflictKind::TypeConflict: {
                if (!f) break;
                std::string chosen;
                switch (r->choice) {
                    case ResolveChoice::Ours:
                        chosen = std::get<std::string>(c.oursValue); break;
                    case ResolveChoice::Theirs:
                        chosen = std::get<std::string>(c.theirsValue); break;
                    case ResolveChoice::Base:
                        chosen = std::get<std::string>(c.baseValue); break;
                    case ResolveChoice::Custom:
                        chosen = std::get<std::string>(r->customValue); break;
                }
                f->type = chosen;
                break;
            }
            case MergeConflictKind::AddRemoveConflict: {
                // Merged tree holds the BASE version of the feature by
                // default (threeWayMerge's conservative policy). The
                // resolution semantics:
                //
                //   * which side is "remove" vs "keep-modified" is
                //     determined by the snapshot optionals on the
                //     conflict record: the one that's std::nullopt is
                //     the removing side, the populated one is the
                //     modifier. This lets the same resolver code
                //     handle both directions (ours-removed or
                //     theirs-removed) without an if-ladder over the
                //     message string.
                //   * ResolveChoice maps to an ACTION:
                //       Ours   → do what ours did (remove OR replace
                //                with ours snapshot)
                //       Theirs → symmetric
                //       Base   → restore base (the merged default;
                //                usually a no-op unless a later stage
                //                mutated the feature)
                //       Custom → not meaningful for a structural
                //                conflict; emit a diagnostic and
                //                leave merged at base.
                const bool oursRemoved   = !c.oursSnapshot.has_value();
                const bool theirsRemoved = !c.theirsSnapshot.has_value();

                auto removeFromOut = [&](const std::string& fid) {
                    for (auto iter = out.begin(); iter != out.end(); ++iter) {
                        if (iter->id == fid) { out.erase(iter); return true; }
                    }
                    return false;
                };
                auto replaceInOut = [&](const Feature& f) -> bool {
                    for (auto& existing : out) {
                        if (existing.id == f.id) { existing = f; return true; }
                    }
                    // Feature not present — insert at hint position.
                    int hint = c.insertIndexHint;
                    if (hint < 0 || hint > static_cast<int>(out.size())) {
                        out.push_back(f);
                    } else {
                        out.insert(out.begin() + hint, f);
                    }
                    return true;
                };

                switch (r->choice) {
                    case ResolveChoice::Ours:
                        if (oursRemoved) {
                            if (!removeFromOut(c.featureId)) {
                                ctx.diag().warning(ErrorCode::INVALID_STATE,
                                    "applyResolutions: AddRemoveConflict "
                                    "chose Ours (delete) for feature '"
                                    + c.featureId + "', but it was already "
                                    "absent from merged — no-op");
                            }
                        } else {
                            replaceInOut(*c.oursSnapshot);
                        }
                        break;

                    case ResolveChoice::Theirs:
                        if (theirsRemoved) {
                            if (!removeFromOut(c.featureId)) {
                                ctx.diag().warning(ErrorCode::INVALID_STATE,
                                    "applyResolutions: AddRemoveConflict "
                                    "chose Theirs (delete) for feature '"
                                    + c.featureId + "', but it was already "
                                    "absent from merged — no-op");
                            }
                        } else {
                            replaceInOut(*c.theirsSnapshot);
                        }
                        break;

                    case ResolveChoice::Base:
                        if (c.baseSnapshot.has_value()) {
                            replaceInOut(*c.baseSnapshot);
                        } else {
                            // No base snapshot means the feature was
                            // added by at least one side — "Base" is
                            // ill-defined. Remove (Base had no
                            // feature), with diagnostic.
                            ctx.diag().warning(ErrorCode::INVALID_INPUT,
                                "applyResolutions: AddRemoveConflict "
                                "chose Base for feature '" + c.featureId
                                + "', but the conflict carries no base "
                                "snapshot — removing.");
                            removeFromOut(c.featureId);
                        }
                        break;

                    case ResolveChoice::Custom:
                        // customValue is a ParamValue — not a Feature.
                        // Structural conflicts need a whole feature
                        // payload to reinsert; Custom therefore
                        // degrades to Base.
                        ctx.diag().warning(ErrorCode::INVALID_INPUT,
                            "applyResolutions: ResolveChoice::Custom is "
                            "not supported for AddRemoveConflict on "
                            "feature '" + c.featureId
                            + "' (custom payload must be a whole Feature, "
                            "not a ParamValue). Falling back to Base.");
                        if (c.baseSnapshot.has_value()) {
                            replaceInOut(*c.baseSnapshot);
                        } else {
                            removeFromOut(c.featureId);
                        }
                        break;
                }
                break;
            }
            case MergeConflictKind::PositionConflict:
                // v1 leaves retained features in base order. The resolution
                // is recorded but not acted on — use direct FeatureTree move
                // operations to enact a specific ordering.
                break;
        }
    }
    return out;
}

} // namespace oreo
