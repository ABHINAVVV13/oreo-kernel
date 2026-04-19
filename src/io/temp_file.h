// SPDX-License-Identifier: LGPL-2.1-or-later

// temp_file.h — Cross-process-safe RAII temp file path generator.
//
// Used by every I/O module that needs to round-trip an OCCT reader
// or writer through a disk path (STEP, IGES, STL). The name is
// composed from:
//   * a caller-supplied prefix (subsystem name),
//   * the process id,
//   * the calling thread's id hash,
//   * a process-wide atomic counter (monotonic),
//   * a 64-bit non-deterministic random nonce,
//   * a caller-supplied extension.
//
// On top of that, the constructor performs an exclusive-create
// (fopen "wxb" — C11 mandated) in a retry loop: if the path
// already exists (another process somehow generated the same name),
// the attempt is rejected and a fresh random nonce is rolled.
//
// Previous revisions used only (atomic-counter + thread-hash), which
// was safe for threads sharing a process but could collide across
// independent server worker processes that each start their counter
// at 0. The PID component alone fixes that; the random nonce + retry
// defends against worst-case (e.g. PID reuse after wraparound on
// long-lived hosts) and against future subtle sources of correlation.
//
// The destructor removes the file on scope exit; rename it or copy
// the bytes out before the TempFile goes out of scope.

#ifndef OREO_IO_TEMP_FILE_H
#define OREO_IO_TEMP_FILE_H

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

#ifdef _WIN32
#  include <process.h>
#else
#  include <unistd.h>
#endif

namespace oreo::io_detail {

class TempFile {
public:
    TempFile(const std::string& prefix, const std::string& ext) {
        static std::atomic<std::uint64_t> counter{0};
        // thread_local so every thread's rng has its own state — avoids
        // lock contention on random_device when many IO calls race.
        thread_local std::mt19937_64 rng{
            static_cast<std::uint64_t>(std::random_device{}()) ^
            (static_cast<std::uint64_t>(std::random_device{}()) << 32)
        };
        const auto tidHash =
            std::hash<std::thread::id>{}(std::this_thread::get_id());
        const auto pid = processId();
        const auto dir = std::filesystem::temp_directory_path();

        constexpr int kMaxAttempts = 32;
        std::string lastCandidate;
        for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
            const std::uint64_t n =
                counter.fetch_add(1, std::memory_order_relaxed);
            const std::uint64_t nonce = rng();

            std::ostringstream name;
            name << prefix
                 << '_' << pid
                 << '_' << static_cast<std::uint64_t>(tidHash)
                 << '_' << n
                 << '_' << std::hex << std::setw(16) << std::setfill('0') << nonce
                 << ext;
            auto candidate = (dir / name.str()).string();
            lastCandidate = candidate;

            if (tryExclusiveCreate(candidate)) {
                path_ = std::move(candidate);
                return;
            }
        }
        throw std::runtime_error(
            "oreo::io_detail::TempFile: could not reserve unique path "
            "after " + std::to_string(kMaxAttempts) +
            " attempts (last candidate: " + lastCandidate + ")");
    }

    ~TempFile() { remove(); }

    // Non-copyable, non-movable — semantics on a path owner are
    // ambiguous (which end does the delete?), and every caller can
    // live with a stack-local value.
    TempFile(const TempFile&)            = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&&)                 = delete;
    TempFile& operator=(TempFile&&)      = delete;

    const std::string& path() const noexcept { return path_; }

    // Release ownership — caller takes responsibility for removing
    // the file (useful if it's been moved/renamed elsewhere).
    void release() noexcept { path_.clear(); }

    void remove() noexcept {
        if (path_.empty()) return;
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        path_.clear();
    }

private:
    static unsigned long long processId() noexcept {
#ifdef _WIN32
        return static_cast<unsigned long long>(::_getpid());
#else
        return static_cast<unsigned long long>(::getpid());
#endif
    }

    // C11 fopen "x" mode: create OR fail if the file exists. Combined
    // with "b" for binary and "w" for write. MSVC CRT, glibc and
    // libc++ all honour this flag.
    //
    // We create an empty placeholder, close it, and hand the path to
    // the caller. OCCT will reopen with truncate-write semantics on
    // its own; that is safe because the path is ours now.
    static bool tryExclusiveCreate(const std::string& p) noexcept {
        std::FILE* f = std::fopen(p.c_str(), "wxb");
        if (!f) return false;
        std::fclose(f);
        return true;
    }

    std::string path_;
};

} // namespace oreo::io_detail

#endif // OREO_IO_TEMP_FILE_H
