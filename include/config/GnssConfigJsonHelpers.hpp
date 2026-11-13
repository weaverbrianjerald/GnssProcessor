#pragma once

/**
 * @file GnssConfigJsonHelpers.hpp
 * @brief Internal JSON-parsing helpers shared by the GnssConfig section
 *        loaders.
 *
 * Shared by @c GnssConfig.cpp, @c GnssConfigLoad.cpp and
 * @c GnssConfigLoadNetwork.cpp, so every config-loading translation unit
 * applies the same "<i>new path or legacy key, otherwise default</i>"
 * semantics.  The simulator's @c SimConfig.cpp also uses @c jval and
 * @c resolvePath, so both config dialects read JSON the same way.
 *
 * The helpers live in the @c GnssProcessor::detail namespace; nothing
 * here is intended for use outside these config loaders.
 */

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace GnssProcessor {
namespace detail {

/// @brief ASCII-only lowercase, used for case-insensitive enum string matching.
/// @param s String to convert, taken by value so the caller's copy is untouched.
/// @return @p s with every ASCII letter lower-cased; other bytes are unchanged.
inline std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// @brief Join a vector of strings with a separator.
/// @param v   Strings to join, in order.  A separator is written only once the
///            result is non-empty, so leading empty entries add no separator.
/// @param sep Separator inserted between consecutive entries (default ", ").
/// @return The joined string; empty when @p v is empty.
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
/// The merge rule for @c "extends" config inheritance.  The rule is
/// asymmetric on purpose:
///  - **object vs object** -> merge key-by-key, recursively.  This lets a delta
///    config set @c network.raim without restating the rest of the @c network
///    section.
///  - **everything else** -> the overlay value replaces the base value outright.
///
/// Arrays replace rather than concatenate, and that is a safety property: the
/// roster @c network.stations is an array, and concatenating a base roster with
/// a child roster would produce duplicate stations, a different station order
/// (the warm-resume fingerprint hashes the roster in order), and a station count
/// that matches no intended regime.  The same holds for
/// @c processing.active_systems and @c signal_priorities.  A child that needs
/// "the base roster plus one" states the whole roster.
///
/// A null overlay value replaces too, so a child can explicitly blank a string
/// the base set (e.g. reset an inherited @c state_snapshot_path to "").
///
/// @param[in,out] base    Inherited config (the parent); receives the merged result.
///                        Replaced outright when either side is not an object.
/// @param         overlay Child config whose values win over @p base; left unchanged.
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

/// @brief Navigate into a nested JSON object using a dot-separated path.
/// Returns a pointer to the value, or nullptr if any segment is missing.
///
/// A key that is present with a JSON null value counts as found.  An empty
/// path resolves to @p root itself.
///
/// @param root    JSON document to search; must outlive the returned pointer.
/// @param dotPath Dot-separated key path such as @c "network.process_noise".
/// @return Pointer into @p root for the addressed value, or nullptr when a segment
///         is missing or an intermediate node is not an object.
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

/// @brief Read a scalar value from JSON trying @p newPath (dot-separated) first,
/// then @p legacyKey (top-level), then returning @p defaultVal.
///
/// Every section loader uses this to accept both the hierarchical and the
/// legacy flat JSON layouts from a single settings line.
///
/// A key that is present is always converted, so a value of the wrong JSON type
/// throws @c nlohmann::json::exception rather than falling back to the default.
///
/// @tparam T Value type to read (for example @c bool, @c int, @c double, @c std::string).
/// @param root       JSON document to read from.
/// @param newPath    Dot-separated path of the current (hierarchical) key.
/// @param legacyKey  Top-level key of the older flat layout; empty to skip the fallback.
/// @param defaultVal Value returned when neither spelling is present.
/// @return The value under @p newPath if present, else under @p legacyKey, else @p defaultVal.
template <typename T>
T jval(const nlohmann::json& root, const std::string& newPath,
       const std::string& legacyKey, const T& defaultVal) {
    if (auto* p = resolvePath(root, newPath))
        return p->get<T>();
    if (!legacyKey.empty() && root.contains(legacyKey))
        return root[legacyKey].get<T>();
    return defaultVal;
}

/// @brief Report whether a scalar is explicitly set, under either spelling.
///
/// Same lookup order as @c jval, but reports presence instead of a value.
/// A precedence rule needs this when "absent" and "set to the default value"
/// must be told apart (e.g. deciding whether a scattered legacy key should
/// shadow the scoped @c network.process_noise table).
///
/// @param root      JSON document to search.
/// @param newPath   Dot-separated path of the current (hierarchical) key.
/// @param legacyKey Top-level key of the older flat layout; empty to skip the fallback.
/// @return True when @p newPath resolves or @p legacyKey is a top-level key of
///         @p root; false when neither is present.
inline bool jhas(const nlohmann::json& root, const std::string& newPath,
                 const std::string& legacyKey) {
    if (resolvePath(root, newPath)) return true;
    return !legacyKey.empty() && root.contains(legacyKey);
}

/// @brief Return a reference to a nested JSON object (new path or legacy key).
/// Falls back to a static empty object so callers can use @c .value(...)
/// against the result without checking @c is_object first.
///
/// @param root      JSON document to search; must outlive the returned reference.
/// @param newPath   Dot-separated path of the current (hierarchical) section.
/// @param legacyKey Top-level key of the older flat layout; empty (default) to skip the fallback.
/// @return Reference into @p root for the section found under @p newPath, else
///         under @p legacyKey, else a shared empty JSON object.  The result is
///         not guaranteed to be an object when the key holds another JSON type.
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
