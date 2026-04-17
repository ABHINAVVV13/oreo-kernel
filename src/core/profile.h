// SPDX-License-Identifier: LGPL-2.1-or-later

// profile.h — CC-7: RAII duration sampler paired with Metrics.
//
// Drop an OREO_PROFILE(ctx.metrics(), "op.name") at the top of any
// function; the scope's destructor pushes the measured nanoseconds into
// Metrics::observeNanoseconds. Use ProfileScope directly when you need
// to name the variable (e.g. to end the measurement early by letting it
// go out of scope deliberately).
//
// Header-only. Pays a std::chrono::steady_clock::now() on entry and
// exit; no allocations. Exceptions raised during the scope still flush
// the measurement because the destructor always runs.

#ifndef OREO_PROFILE_H
#define OREO_PROFILE_H

#include "metrics.h"

#include <chrono>
#include <string>
#include <utility>

namespace oreo {

class ProfileScope {
public:
    ProfileScope(Metrics& m, std::string name)
        : metrics_(m),
          name_(std::move(name)),
          start_(std::chrono::steady_clock::now()) {}

    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;

    ~ProfileScope() {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start_).count();
        // ns is a signed count from chrono; steady_clock guarantees
        // monotonicity so it cannot go negative under normal use, but
        // clamp defensively.
        const uint64_t safeNs = (ns < 0) ? 0 : static_cast<uint64_t>(ns);
        metrics_.observeNanoseconds(name_, safeNs);
    }

private:
    Metrics& metrics_;
    std::string name_;
    std::chrono::steady_clock::time_point start_;
};

// Two-level macro indirection so __LINE__ expands before ## concat.
#define OREO_PROFILE_CONCAT2(a, b) a##b
#define OREO_PROFILE_CONCAT(a, b) OREO_PROFILE_CONCAT2(a, b)
#define OREO_PROFILE(metrics, name) \
    ::oreo::ProfileScope OREO_PROFILE_CONCAT(_oreo_profile_, __LINE__)((metrics), (name))

} // namespace oreo

#endif // OREO_PROFILE_H
