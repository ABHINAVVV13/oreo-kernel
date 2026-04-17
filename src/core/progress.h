// progress.h — Progress reporting callback for long-running kernel operations.
//
// ProgressCallback lets the kernel report progress during operations that
// are expected to take noticeable wall-clock time (large boolean ops,
// meshing, file import). A UI can attach a callback via
// KernelContext::setProgressCallback and receive fraction-complete updates
// along with a human-readable phase label.
//
// Contract:
//   - fraction is clamped to [0.0, 1.0]. Monotonic increase is expected but
//     not enforced (complex operations may reset between phases).
//   - phase is a short label such as "meshing", "boolean fuse", "export".
//   - Callbacks are invoked on the calling thread — they must be quick and
//     must not throw. If a callback needs to post to a UI event loop, the
//     caller is responsible for that marshalling.

#ifndef OREO_PROGRESS_H
#define OREO_PROGRESS_H

#include <functional>
#include <string>

namespace oreo {

using ProgressCallback = std::function<void(double fraction, const std::string& phase)>;

} // namespace oreo

#endif // OREO_PROGRESS_H
