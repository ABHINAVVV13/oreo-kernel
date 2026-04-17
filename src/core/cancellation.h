// SPDX-License-Identifier: LGPL-2.1-or-later

// cancellation.h — Cooperative cancellation token for long-running operations.
//
// CancellationToken is a thread-safe flag that can be polled by worker
// operations to allow graceful early exit. Used by KernelContext to
// propagate user-requested cancellation into geometry kernels that can
// honour a "please stop" request at logical checkpoints.
//
// Thread safety: a single CancellationToken is safe to share across
// threads — cancel()/isCancelled()/reset() are atomic. Typical usage:
// one producer thread (UI) flips cancel(); many worker threads poll
// isCancelled() and bail out.

#ifndef OREO_CANCELLATION_H
#define OREO_CANCELLATION_H

#include <atomic>
#include <memory>

namespace oreo {

class CancellationToken {
public:
    CancellationToken() = default;

    // Disable copy / move: the token identity matters. Share via
    // CancellationTokenPtr (shared_ptr) when multiple owners are needed.
    CancellationToken(const CancellationToken&) = delete;
    CancellationToken& operator=(const CancellationToken&) = delete;
    CancellationToken(CancellationToken&&) = delete;
    CancellationToken& operator=(CancellationToken&&) = delete;

    // Request cancellation. Any subsequent call to isCancelled() returns true.
    void cancel() noexcept {
        cancelled_.store(true, std::memory_order_release);
    }

    // Poll cancellation state. Workers should call this at logical checkpoints.
    bool isCancelled() const noexcept {
        return cancelled_.load(std::memory_order_acquire);
    }

    // Clear the cancellation flag (for token reuse across operations).
    void reset() noexcept {
        cancelled_.store(false, std::memory_order_release);
    }

private:
    std::atomic<bool> cancelled_{false};
};

using CancellationTokenPtr = std::shared_ptr<CancellationToken>;

} // namespace oreo

#endif // OREO_CANCELLATION_H
