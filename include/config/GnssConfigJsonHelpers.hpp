#pragma once

/**
 * @file GnssConfigJsonHelpers.hpp
 * @brief Internal JSON-parsing helpers shared by the GnssConfig section
 *        loaders.
 *
 * Extracted from the original anonymous namespace at the top of
 * @c GnssConfig.cpp so that @c GnssConfigLoad.cpp and any future
 * config-loading translation units can share the exact same
 * "<i>new path or legacy key, otherwise default</i>" semantics.
 *
 * The helpers live in the @c GnssProcessor::detail namespace; nothing
 * here is intended for use outside the config loader.
 */

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace GnssProcessor {
namespace detail {

/// ASCII-only lowercase, used for case-insensitive enum string matching.
inline std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// Join a vector of strings with a separator.
inline std::string joinStrings(const std::vector<std::string>& v,
                               const char* sep = ", ") {
    std::string r;
    for (const auto& s : v) {
        if (!r.empty()) r += sep;
        r += s;
    }
    return r;
}

/// @brief Recursively overlay @p overlay onto @p base, in place.
///
/// The merge rule for @c "extends" config inheritance, and the rule is
/// deliberately asymmetric:
///  - **object vs object** -> merge key-by-key, recursively.  This is what makes
///    a delta config able to set @c network.raim without restating the other
///    ~80 keys of the @c network section.
///  - **everything else** -> the overlay value REPLACES the base value outright.
///
/// Arrays replace rather than concatenate, and that is a safety property, not a
/// convenience: @c network.stations is an array, and a base roster silently
/// concatenating with a child roster would produce duplicate stations, a
/// different station ORDER (the warm-resume fingerprint hashes the roster in
/// order), and a station count that matches no intended regime.  The same goes
/// for @c processing.active_systems and @c signal_priorities.  A child that
/// wants "the base roster plus one" must state the whole roster.
///
/// A null overlay value replaces too, so a child can explicitly blank a string
/// the base set (e.g. reset an inherited @c state_snapshot_path to "").
inline void deepMergeJson(nlohmann::json& base, const nlohmann::json& overlay) {
    if (!base.is_object() || !overlay.is_object()) {
        base = overlay;
        return;
    }
    for (auto it = overlay.begin(); it != overlay.end(); ++it) {
        if (base.contains(it.key()) && base[it.key()].is_object() && it.value().is_object())
            deepMergeJson(base[it.key()], it.value());
        else
            base[it.key()] = it.value();
    }
}

/// Navigate into a nested JSON object using a dot-separated path.
/// Returns a pointer to the value, or nullptr if any segment is missing.
inline const nlohmann::json* resolvePath(const nlohmann::json& root,
                                         const std::string& dotPath) {
    const nlohmann::json* node = &root;
    std::string::size_type start = 0;
    while (start < dotPath.size()) {
        auto dot = dotPath.find('.', start);
        auto key = dotPath.substr(start, dot - start);
        if (!node->is_object() || !node->contains(key)) return nullptr;
        node = &(*node)[key];
        start = (dot == std::string::npos) ? dotPath.size() : dot + 1;
    }
    return node;
}

/// Read a scalar value from JSON trying @p newPath (dot-separated) first,
/// then @p legacyKey (top-level), then returning @p defaultVal.
///
/// This is the workhorse used by every section loader to keep the
/// hierarchical and legacy-flat JSON layouts both working from a single
/// settings line.
template <typename T>
T jval(const nlohmann::json& root, const std::string& newPath,
       const std::string& legacyKey, const T& defaultVal) {
    if (auto* p = resolvePath(root, newPath))
        return p->get<T>();
    if (!legacyKey.empty() && root.contains(legacyKey))
        return root[legacyKey].get<T>();
    return defaultVal;
}

/// @brief Was a scalar EXPLICITLY set, under either spelling?
///
/// Same lookup order as @ref jval, but reports presence instead of a value —
/// the discriminator a precedence rule needs when "absent" and "set to the
/// default value" must be told apart (e.g. deciding whether a scattered legacy
/// key should shadow the scoped @c network.process_noise table).
inline bool jhas(const nlohmann::json& root, const std::string& newPath,
                 const std::string& legacyKey) {
    if (resolvePath(root, newPath)) return true;
    return !legacyKey.empty() && root.contains(legacyKey);
}

/// Return a reference to a nested JSON object (new path or legacy key).
/// Falls back to a static empty object so callers can use @c .value(...)
/// against the result without checking @c is_object first.
inline const nlohmann::json& jsection(const nlohmann::json& root,
                                      const std::string& newPath,
                                      const std::string& legacyKey = "") {
    if (auto* p = resolvePath(root, newPath))
        return *p;
    if (!legacyKey.empty() && root.contains(legacyKey))
        return root[legacyKey];
    static const nlohmann::json empty = nlohmann::json::object();
    return empty;
}

}  // namespace detail
}  // namespace GnssProcessor
