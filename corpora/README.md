# Fuzzer corpora

Seed inputs for the libFuzzer harnesses in `fuzzers/`. libFuzzer mutates
these into novel inputs to find crashes; well-chosen seeds dramatically
shorten time-to-first-crash for new code paths.

## Layout

One directory per fuzzer harness. Filenames are arbitrary; the
extension hints at content.

```
corpora/
├── fuzz_deserialize/        # binary serialized solids
├── fuzz_step_import/        # STEP files (text)
└── fuzz_feature_tree_json/  # FeatureTree JSON serializations
```

## What to add

Each corpus should contain:

1. **Minimal valid examples** — the smallest inputs that exercise each
   distinct code path (empty tree, single feature, two features with a
   reference, etc.).
2. **Adversarial samples** — known-bad inputs we want regressions on.
   Examples: truncated headers, unterminated strings, NaN/Inf in numeric
   fields, integer overflow seeds, version mismatches.
3. **Real-world samples** — captured from actual CAD documents (with PII
   stripped). Especially valuable for the STEP corpus.

Keep individual files small (≤ 64 KB) so libFuzzer's mutation budget per
iteration stays high.

## Adding a new harness

When you add `fuzzers/fuzz_widget.cc`, also create
`corpora/fuzz_widget/` with at least one seed file. The CI workflow in
`.github/workflows/ci.yml` enumerates harnesses by name.
