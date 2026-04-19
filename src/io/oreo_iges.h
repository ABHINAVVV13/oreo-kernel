// SPDX-License-Identifier: LGPL-2.1-or-later

// oreo_iges.h — IGES 5.3 import / export through OCCT's IGESCAFControl.
//
// IGES is surface-and-wire oriented (not solid-first like STEP); the
// reader handles Trimmed Surface (144), Rational B-Spline Surface
// (128), Plane (108), Line (110), etc. and attempts to build a shell
// from the imported entities. Solid reconstruction is best-effort —
// if your downstream workflow requires a clean BRep solid, prefer STEP.
//
// The XDE reader preserves colors and part names when the file carries
// them; the basic reader is the fallback when XDE processing fails
// (mirrors oreo_step.cpp's contract).
//
// Every operation takes a KernelContext& as its first parameter.

#ifndef OREO_IGES_H
#define OREO_IGES_H

#include "core/kernel_context.h"
#include "core/operation_result.h"
#include "naming/named_shape.h"

#include <cstdint>
#include <string>
#include <vector>

namespace oreo {

// Import an IGES file from a memory buffer. Uses XDE when colors/names
// are present; falls back to basic IGES otherwise. Runs a ShapeFix
// pass before returning.
OperationResult<NamedShape> importIges    (KernelContext& ctx,
                                            const std::uint8_t* data,
                                            std::size_t len);
OperationResult<NamedShape> importIgesFile(KernelContext& ctx,
                                            const std::string& path);

// Export shapes to an IGES file. Always writes IGES 5.3. Any non-null
// shapes are accepted; nulls in the input vector are silently skipped
// so callers can reuse their selection set without filtering.
OperationResult<bool>                       exportIgesFile(KernelContext& ctx,
                                                            const std::vector<NamedShape>& shapes,
                                                            const std::string& path);
OperationResult<std::vector<std::uint8_t>>  exportIges    (KernelContext& ctx,
                                                            const std::vector<NamedShape>& shapes);

} // namespace oreo

#endif // OREO_IGES_H
