// localization.h — CC-9: message catalog for localized diagnostics.
//
// MessageCatalog is a process-wide singleton that maps a locale + message-ID
// to a translated string. Messages may contain numbered placeholders {0},
// {1}, ... which are substituted at lookup time from the args list passed
// to get() / tr().
//
// Lookup policy:
//   1. Try the current locale.
//   2. Fall back to "en".
//   3. Return the raw id if no translation exists.
//
// Thread-safety: reads take a shared lock; mutations take an exclusive lock.
// Safe to call from multiple worker threads simultaneously.

#ifndef OREO_LOCALIZATION_H
#define OREO_LOCALIZATION_H

#pragma once

#include <initializer_list>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace oreo {

class MessageCatalog {
public:
    // Access the process-wide catalog. The first call constructs it; safe
    // from any thread (C++11 magic-static guarantees).
    static MessageCatalog& instance();

    // Set the active locale (e.g. "en", "de", "ja"). Takes an exclusive
    // lock for the duration of the update.
    void setLocale(std::string locale);

    // Read the active locale. Returns a copy to avoid exposing internal
    // storage past the lock.
    std::string locale() const;

    // Register a translation. Subsequent add() calls with the same
    // (locale, id) overwrite the existing entry.
    void add(const std::string& locale,
             const std::string& id,
             std::string text);

    // Look up a translation and substitute numbered placeholders.
    //
    // Placeholder syntax: {N} where N is a decimal index into args. Unknown
    // indices are left as-is. To emit a literal brace, double it — "{{" → "{",
    // "}}" → "}".
    //
    // Returns the raw id if neither the current locale nor the "en" fallback
    // contains an entry.
    std::string get(const std::string& id,
                    std::initializer_list<std::string_view> args = {}) const;

private:
    mutable std::shared_mutex mtx_;
    std::string locale_ = "en";
    // locale -> id -> text
    std::unordered_map<std::string,
                       std::unordered_map<std::string, std::string>> catalog_;
};

// Free-function convenience around MessageCatalog::instance().get().
std::string tr(const std::string& id,
               std::initializer_list<std::string_view> args = {});

} // namespace oreo

#endif // OREO_LOCALIZATION_H
