// metrics.h — CC-6: lightweight in-process metrics registry.
//
// Header-only so callers pay only for what they include. Designed for
// low-friction instrumentation inside the kernel: counters for discrete
// events (e.g. "boolean.union.calls") and nanosecond observations for
// durations (e.g. "boolean.union.ns" — paired with ProfileScope in
// profile.h).
//
// Thread safety: the registry itself is guarded by an internal mutex
// for map structure changes (inserts). Individual counter reads and
// updates use std::atomic<uint64_t>, so two threads incrementing the
// same existing counter never block each other. Only first-time
// insertion of a new name pays the mutex cost.
//
// Not intended as a replacement for Prometheus/OpenTelemetry — this is
// purely an in-process introspection hook. snapshot() exposes the
// current counter values for diagnostic sinks or tests.

#ifndef OREO_METRICS_H
#define OREO_METRICS_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace oreo {

class Metrics {
public:
    Metrics() = default;
    Metrics(const Metrics&) = delete;
    Metrics& operator=(const Metrics&) = delete;

    // Increment a monotonic counter. Creates the counter on first use.
    void increment(const std::string& name, uint64_t n = 1) {
        std::atomic<uint64_t>* cell = findOrCreate(counters_, name);
        cell->fetch_add(n, std::memory_order_relaxed);
    }

    // Record a nanosecond observation. Accumulates total ns and the
    // number of observations so averageNs() can divide them on demand.
    // Intended to be fed by ProfileScope; callers can also push manual
    // durations measured elsewhere.
    void observeNanoseconds(const std::string& name, uint64_t ns) {
        std::atomic<uint64_t>* sum = findOrCreate(gaugesTotalNs_, name);
        std::atomic<uint64_t>* cnt = findOrCreate(gaugesCount_, name);
        sum->fetch_add(ns, std::memory_order_relaxed);
        cnt->fetch_add(1, std::memory_order_relaxed);
    }

    // Return the current value of a counter, or 0 if unknown.
    uint64_t counter(const std::string& name) const {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = counters_.find(name);
        if (it == counters_.end()) return 0;
        return it->second.load(std::memory_order_relaxed);
    }

    // Average nanoseconds across all observations recorded for `name`.
    // Returns 0.0 when no observations have been recorded.
    double averageNs(const std::string& name) const {
        std::lock_guard<std::mutex> lk(mtx_);
        auto sIt = gaugesTotalNs_.find(name);
        auto cIt = gaugesCount_.find(name);
        if (sIt == gaugesTotalNs_.end() || cIt == gaugesCount_.end()) return 0.0;
        const uint64_t cnt = cIt->second.load(std::memory_order_relaxed);
        if (cnt == 0) return 0.0;
        const uint64_t sum = sIt->second.load(std::memory_order_relaxed);
        return static_cast<double>(sum) / static_cast<double>(cnt);
    }

    // Snapshot of every counter. Does not include timing gauges (those
    // are exposed via averageNs per-name). Copies under the mutex so the
    // caller owns an immutable view safe to iterate without locking.
    std::unordered_map<std::string, uint64_t> snapshot() const {
        std::lock_guard<std::mutex> lk(mtx_);
        std::unordered_map<std::string, uint64_t> out;
        out.reserve(counters_.size());
        for (const auto& kv : counters_) {
            out.emplace(kv.first, kv.second.load(std::memory_order_relaxed));
        }
        return out;
    }

private:
    // std::atomic<T> is neither copyable nor movable, so the entries
    // in these maps cannot be rehashed to a different bucket once
    // inserted. That is fine for unordered_map — rehash moves the *node*
    // (which holds the atomic), not the atomic value itself. However we
    // still serialize insertion under mtx_ so concurrent first-time
    // insertions don't race the rehash.
    mutable std::mutex mtx_;
    std::unordered_map<std::string, std::atomic<uint64_t>> counters_;
    std::unordered_map<std::string, std::atomic<uint64_t>> gaugesTotalNs_;
    std::unordered_map<std::string, std::atomic<uint64_t>> gaugesCount_;

    std::atomic<uint64_t>* findOrCreate(
        std::unordered_map<std::string, std::atomic<uint64_t>>& m,
        const std::string& name) {
        // Fast path: lookup under mutex (necessary — another thread
        // could be inserting and rehashing at the same moment).
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = m.find(name);
        if (it != m.end()) return &it->second;
        auto emplaced = m.emplace(std::piecewise_construct,
                                  std::forward_as_tuple(name),
                                  std::forward_as_tuple(0));
        return &emplaced.first->second;
    }
};

} // namespace oreo

#endif // OREO_METRICS_H
