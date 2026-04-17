# Public error code reference

The kernel exposes a stable, 32-bit-int-valued enum
[`OreoErrorCode`](../include/oreo_kernel.h) on the C ABI. Every
internal `oreo::ErrorCode` value (declared in
[`src/core/diagnostic.h`](../src/core/diagnostic.h)) shares the same
numeric value as its public counterpart; the static_asserts in
[`src/capi/oreo_capi.cpp`](../src/capi/oreo_capi.cpp) trap drift at
compile time.

## Code blocks

Codes are organised in numeric blocks so future additions stay
predictable and so consumers can switch on a range when handling
families of errors.

| Block | Range  | Meaning                                                |
|-------|--------|--------------------------------------------------------|
| 0     | 0      | Success                                                |
| 1–9   | 1–9    | Geometry / OCCT / sketch failures                      |
| 10–19 | 10–19  | I/O (STEP, serialize, identity v2 hardening)           |
| 20–29 | 20–29  | Internal-state errors                                  |
| 100s  | 100–199| General-purpose codes (state, range, timeout, quotas)  |
| 200s  | 200–299| Internal markers (diagnostic-collector overflow, etc.) |

## Mapping

| Public                                  | Value | Internal                              | When it fires                                                                |
|-----------------------------------------|------:|---------------------------------------|------------------------------------------------------------------------------|
| `OREO_OK`                               | 0     | `ErrorCode::OK`                       | Successful return                                                            |
| `OREO_INVALID_INPUT`                    | 1     | `ErrorCode::INVALID_INPUT`            | Null shape, zero vector, missing required parameter                          |
| `OREO_OCCT_FAILURE`                     | 2     | `ErrorCode::OCCT_FAILURE`             | OCCT operation returned `!IsDone()` or threw `Standard_Failure`              |
| `OREO_BOOLEAN_FAILED`                   | 3     | `ErrorCode::BOOLEAN_FAILED`           | Boolean op produced an invalid result (degenerate intersection, etc.)        |
| `OREO_SHAPE_INVALID`                    | 4     | `ErrorCode::SHAPE_INVALID`            | `BRepCheck_Analyzer` reports the resulting shape is invalid                  |
| `OREO_SHAPE_FIX_FAILED`                 | 5     | `ErrorCode::SHAPE_FIX_FAILED`         | `ShapeFix` was invoked but could not repair the shape                        |
| `OREO_SKETCH_SOLVE_FAILED`              | 6     | `ErrorCode::SKETCH_SOLVE_FAILED`      | PlaneGCS solver did not converge                                             |
| `OREO_SKETCH_REDUNDANT`                 | 7     | `ErrorCode::SKETCH_REDUNDANT`         | Sketch is over-constrained                                                   |
| `OREO_SKETCH_CONFLICTING`               | 8     | `ErrorCode::SKETCH_CONFLICTING`       | Sketch has conflicting constraints                                           |
| `OREO_STEP_IMPORT_FAILED`               | 10    | `ErrorCode::STEP_IMPORT_FAILED`       | STEP file could not be read or produced no shapes                            |
| `OREO_STEP_EXPORT_FAILED`               | 11    | `ErrorCode::STEP_EXPORT_FAILED`       | STEP writer rejected the input                                               |
| `OREO_SERIALIZE_FAILED`                 | 12    | `ErrorCode::SERIALIZE_FAILED`         | Binary serialize could not encode the shape                                  |
| `OREO_DESERIALIZE_FAILED`               | 13    | `ErrorCode::DESERIALIZE_FAILED`       | Binary deserialize: malformed buffer, checksum mismatch, truncation          |
| `OREO_LEGACY_IDENTITY_DOWNGRADE`        | 14    | `ErrorCode::LEGACY_IDENTITY_DOWNGRADE` | (Warning) v1 tag read; high docId bits inferred from the calling ctx        |
| `OREO_V2_IDENTITY_NOT_REPRESENTABLE`    | 15    | `ErrorCode::V2_IDENTITY_NOT_REPRESENTABLE` | v2 identity (counter > UINT32_MAX) cannot fit in v1 scalar format       |
| `OREO_MALFORMED_ELEMENT_NAME`           | 16    | `ErrorCode::MALFORMED_ELEMENT_NAME`   | (Warning) `;:P` / `;:H` payload could not be parsed                          |
| `OREO_BUFFER_TOO_SMALL`                 | 17    | `ErrorCode::BUFFER_TOO_SMALL`         | Caller-supplied output buffer too small (size-probe protocol)                |
| `OREO_NOT_INITIALIZED`                  | 20    | `ErrorCode::NOT_INITIALIZED`          | `oreo_init()` not called (legacy API only)                                   |
| `OREO_INTERNAL_ERROR`                   | 21    | `ErrorCode::INTERNAL_ERROR`           | Unhandled C++ exception or unknown internal failure                          |
| `OREO_NOT_SUPPORTED`                    | 100   | `ErrorCode::NOT_SUPPORTED`            | Operation not implemented for this input                                     |
| `OREO_INVALID_STATE`                    | 101   | `ErrorCode::INVALID_STATE`            | Caller invoked the API in the wrong state                                    |
| `OREO_OUT_OF_RANGE`                     | 102   | `ErrorCode::OUT_OF_RANGE`             | Numeric or index argument out of range                                       |
| `OREO_TIMEOUT`                          | 103   | `ErrorCode::TIMEOUT`                  | Operation exceeded its deadline                                              |
| `OREO_CANCELLED`                        | 104   | `ErrorCode::CANCELLED`                | Operation was cancelled via `KernelContext::cancellationToken`               |
| `OREO_RESOURCE_EXHAUSTED`               | 105   | `ErrorCode::RESOURCE_EXHAUSTED`       | Quota tripped (mesh / GLB / STEP / serialize / features / diagnostics)       |
| `OREO_DIAG_TRUNCATED`                   | 200   | `ErrorCode::DIAG_TRUNCATED`           | (Internal) Diagnostic collector reached `maxDiagnostics`                     |

## Severity

Diagnostics carry an independent `Severity` (Debug / Trace / Info /
Warning / Error / Fatal). The same `OreoErrorCode` may appear with
different severities depending on context. As a rule of thumb:

* `OREO_LEGACY_IDENTITY_DOWNGRADE` and `OREO_MALFORMED_ELEMENT_NAME`
  are emitted as `Warning` — the operation continues with degraded
  metadata.
* `OREO_DIAG_TRUNCATED` is emitted as `Info` (it is metadata about
  the collector itself, not an operation failure).
* All other codes are `Error` or `Fatal`.

Use `oreo_context_diagnostic_count()` plus
`oreo_context_diagnostic_code()` / `oreo_context_diagnostic_message()`
to iterate the structured diagnostic stream from a no-legacy build.

## Adding a new code

1. Add the public symbol to `OreoErrorCode` in
   [`include/oreo_kernel.h`](../include/oreo_kernel.h) at the
   appropriate block, with an explicit numeric value.
2. Add the matching `ErrorCode::<name>` entry in
   [`src/core/diagnostic.h`](../src/core/diagnostic.h) with the same
   number.
3. Add a `static_assert` in
   [`src/capi/oreo_capi.cpp`](../src/capi/oreo_capi.cpp) so any
   future drift is caught at compile time.
4. Update the table above and document when the code fires.
