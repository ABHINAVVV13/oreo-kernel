// localization.cpp — implementation of MessageCatalog (see localization.h).

#include "localization.h"

#include <cctype>
#include <mutex>
#include <string>
#include <utility>

namespace oreo {

MessageCatalog& MessageCatalog::instance() {
    static MessageCatalog s;
    return s;
}

void MessageCatalog::setLocale(std::string locale) {
    std::unique_lock<std::shared_mutex> lock(mtx_);
    locale_ = std::move(locale);
}

std::string MessageCatalog::locale() const {
    std::shared_lock<std::shared_mutex> lock(mtx_);
    return locale_;
}

void MessageCatalog::add(const std::string& locale,
                         const std::string& id,
                         std::string text) {
    std::unique_lock<std::shared_mutex> lock(mtx_);
    catalog_[locale][id] = std::move(text);
}

// Substitute {N} placeholders in `tmpl` using `args`. Doubled braces ({{, }})
// emit a literal brace. Unknown indices and malformed specifiers are left
// as-is — lookup failures must not crash the caller.
static std::string substitute(const std::string& tmpl,
                              std::initializer_list<std::string_view> args) {
    std::string out;
    out.reserve(tmpl.size());

    const char* const data = tmpl.data();
    const std::size_t n = tmpl.size();

    for (std::size_t i = 0; i < n; ) {
        const char c = data[i];

        // Escape: {{ -> {
        if (c == '{' && i + 1 < n && data[i + 1] == '{') {
            out.push_back('{');
            i += 2;
            continue;
        }
        // Escape: }} -> }
        if (c == '}' && i + 1 < n && data[i + 1] == '}') {
            out.push_back('}');
            i += 2;
            continue;
        }

        if (c == '{') {
            // Try to parse {N}
            std::size_t j = i + 1;
            std::size_t idx = 0;
            bool haveDigit = false;
            while (j < n && data[j] >= '0' && data[j] <= '9') {
                idx = idx * 10 + static_cast<std::size_t>(data[j] - '0');
                haveDigit = true;
                ++j;
                if (idx > (1u << 20)) break;  // sanity guard
            }
            if (haveDigit && j < n && data[j] == '}') {
                if (idx < args.size()) {
                    const std::string_view sv = *(args.begin() + idx);
                    out.append(sv.data(), sv.size());
                } else {
                    // Out-of-range — keep the literal text so the caller
                    // can tell something is off without us throwing.
                    out.append(data + i, (j + 1) - i);
                }
                i = j + 1;
                continue;
            }
            // Malformed specifier — emit the brace literally.
        }

        out.push_back(c);
        ++i;
    }

    return out;
}

std::string MessageCatalog::get(const std::string& id,
                                std::initializer_list<std::string_view> args) const {
    std::string localeSnapshot;
    {
        std::shared_lock<std::shared_mutex> lock(mtx_);
        localeSnapshot = locale_;

        // 1. Active locale.
        auto it = catalog_.find(localeSnapshot);
        if (it != catalog_.end()) {
            auto m = it->second.find(id);
            if (m != it->second.end()) {
                return substitute(m->second, args);
            }
        }

        // 2. English fallback (unless we already tried it above).
        if (localeSnapshot != "en") {
            auto enIt = catalog_.find("en");
            if (enIt != catalog_.end()) {
                auto m = enIt->second.find(id);
                if (m != enIt->second.end()) {
                    return substitute(m->second, args);
                }
            }
        }
    }
    // 3. Give up — return the raw id so callers still see *something*
    //    useful instead of an empty string.
    return id;
}

std::string tr(const std::string& id,
               std::initializer_list<std::string_view> args) {
    return MessageCatalog::instance().get(id, args);
}

} // namespace oreo
