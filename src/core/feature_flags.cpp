// SPDX-License-Identifier: LGPL-2.1-or-later

// feature_flags.cpp — implementation of FeatureFlags (see feature_flags.h).

#include "feature_flags.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#else
  extern "C" char** environ;
#endif

namespace oreo {

namespace {

constexpr const char* kEnvPrefix = "OREO_FEATURE_";
constexpr std::size_t kEnvPrefixLen = 13; // strlen("OREO_FEATURE_")

// Lowercase ASCII only — flag names are kernel-internal and always ASCII.
std::string toLowerAscii(const std::string& s) {
    std::string out;
    out.resize(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        out[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a')
                                        : static_cast<char>(c);
    }
    return out;
}

// Parse a boolean env-style string. Returns std::nullopt for unrecognised
// values so callers can distinguish "not set" from "set to nonsense".
std::optional<bool> parseBool(const char* raw) {
    if (!raw || !*raw) return std::nullopt;
    std::string v;
    for (const char* p = raw; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        v.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a')
                                           : static_cast<char>(c));
    }
    if (v == "1" || v == "true"  || v == "yes" || v == "on")  return true;
    if (v == "0" || v == "false" || v == "no"  || v == "off") return false;
    return std::nullopt;
}

#if defined(_WIN32)

// Convert a single UTF-16 env entry of the form NAME=VALUE to UTF-8.
std::string wideToUtf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string out;
    out.resize(static_cast<std::size_t>(len - 1));
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
    return out;
}

#endif

} // namespace

FeatureFlags& FeatureFlags::instance() {
    static FeatureFlags s;
    return s;
}

bool FeatureFlags::isEnabled(const std::string& flag) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = flags_.find(flag);
    return it != flags_.end() && it->second;
}

void FeatureFlags::set(const std::string& flag, bool enabled) {
    std::lock_guard<std::mutex> lock(mtx_);
    flags_[flag] = enabled;
}

void FeatureFlags::clear(const std::string& flag) {
    std::lock_guard<std::mutex> lock(mtx_);
    flags_.erase(flag);
}

std::unordered_map<std::string, bool> FeatureFlags::snapshot() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return flags_;
}

bool FeatureFlags::loadFlag(const char* envName, const char* flagName) {
    if (!envName || !flagName) return false;
    const char* raw = std::getenv(envName);
    auto parsed = parseBool(raw);
    if (!parsed.has_value()) return false;
    set(flagName, *parsed);
    return true;
}

void FeatureFlags::loadFromEnvironment() {
#if defined(_WIN32)
    // Use the wide-char API so non-ASCII env values don't get mangled, then
    // transcode only the names we care about.
    LPWCH block = GetEnvironmentStringsW();
    if (!block) return;

    for (LPWCH p = block; *p; ) {
        const std::string entry = wideToUtf8(p);

        // Advance past the current null-terminated entry.
        while (*p) ++p;
        ++p;

        if (entry.size() <= kEnvPrefixLen) continue;
        if (entry.compare(0, kEnvPrefixLen, kEnvPrefix) != 0) continue;

        const auto eq = entry.find('=', kEnvPrefixLen);
        if (eq == std::string::npos) continue;

        const std::string rawName = entry.substr(kEnvPrefixLen, eq - kEnvPrefixLen);
        const std::string value   = entry.substr(eq + 1);
        if (rawName.empty()) continue;

        const auto parsed = parseBool(value.c_str());
        if (!parsed.has_value()) continue;

        set(toLowerAscii(rawName), *parsed);
    }

    FreeEnvironmentStringsW(block);
#else
    if (!environ) return;
    for (char** e = environ; *e; ++e) {
        const char* entry = *e;
        if (std::strncmp(entry, kEnvPrefix, kEnvPrefixLen) != 0) continue;

        const char* eq = std::strchr(entry + kEnvPrefixLen, '=');
        if (!eq) continue;

        const std::string rawName(entry + kEnvPrefixLen, eq);
        const std::string value(eq + 1);
        if (rawName.empty()) continue;

        const auto parsed = parseBool(value.c_str());
        if (!parsed.has_value()) continue;

        set(toLowerAscii(rawName), *parsed);
    }
#endif
}

} // namespace oreo
