#pragma once
/**
 * @file ReferenceConfigCache.hpp
 * @brief Test support: a reference configuration parsed once per process, and a station
 *        registry cut to the stations a configuration names, so that a test loading the field
 *        reference configuration many times does not parse the full registry each time.
 *
 * Where the time of a load goes.  config/golden/golden_field_refound.json (and the BeiDou
 * lineage) arm ocean tide loading from the station registry (model_corrections.otl_from_sif).
 * The loader then auto-discovers every file under <internal_data_root>/static/sif/ and parses
 * it whole: about 28 MB of JSON (igs20_SIF.json and ncn20_SIF.json, several thousand stations).
 * In the Debug test program that parse is about 13 s of a 13.5 s load; the rest of the loader,
 * with its 58-station roster, takes well under a second.
 *
 * What the cut changes, and what it does not.  withRosterStationInfo() writes, once per process
 * and per roster, a copy of each registry file that keeps the file's text outside the STATIONS
 * array byte for byte (its METADATA) and, inside it, the entries of the stations whose ID is
 * one of the configuration's string values (the roster and the held-out station), each entry
 * copied verbatim.  It then lists those copies in files.station_info_paths, in the order the
 * loader's own discovery would list the originals.  Inside the loader the registry is read
 * only through its station count (zero or not) and, under network.use_sif_coordinates, through
 * the roster's own entries, which the copies hold unchanged.  So a load differs from a load of
 * the committed file in exactly two places: the member getStationInfoPaths (the copies' paths)
 * and the station count of the info line "Station info: loaded N stations ...".  A test that
 * compares two loads made the same way, or reads anything other than those two, sees no
 * difference.  A test that pins the committed file as it is on disk (the member dump of every
 * configuration file) does not use the cut.
 *
 * A configuration that would not load the registry (neither processing.enable_station_info nor
 * ocean tide loading from it), names its registry files explicitly, or has no station of the
 * registry in its roster is returned unchanged.
 */

#include <nlohmann/json.hpp>

#include "TestTempDir.hpp"   // detail::processRootPath(): the per-process scratch root

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace gnss_test {
namespace reference_config_detail {

/** @brief Every string value anywhere in @p j (object values and array elements, recursively). */
inline void collectStrings(const nlohmann::json& j, std::set<std::string>& out) {
    if (j.is_string()) {
        out.insert(j.get<std::string>());
    } else if (j.is_array() || j.is_object()) {
        for (const auto& e : j) collectStrings(e, out);
    }
}

/** @brief The whole file at @p p as bytes; empty when it cannot be read. */
inline std::string readBytes(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

inline bool isSpace(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

/**
 * @brief The registry text @p text with its STATIONS array cut to the entries whose top-level
 *        "ID" is in @p keep; every byte outside the array, and every kept entry, is copied as is.
 *
 * A string-aware scan of the text (no parse), so cutting the 28 MB registry costs a fraction of
 * a second in Debug.
 *
 * @param text  The registry file's text.
 * @param keep  The station IDs to keep.
 * @param out   Receives the cut text.
 * @param kept  Receives the number of entries kept.
 * @return False when the text has no STATIONS array of objects the scan can follow.
 */
inline bool cutStations(const std::string& text, const std::set<std::string>& keep,
                        std::string& out, std::size_t& kept) {
    const std::size_t key = text.find("\"STATIONS\"");
    if (key == std::string::npos) return false;
    const std::size_t open = text.find('[', key);
    if (open == std::string::npos) return false;
    const char* s = text.data();
    const std::size_t n = text.size();

    std::string body;
    kept = 0;
    std::size_t i = open + 1;
    for (;;) {
        while (i < n && (isSpace(s[i]) || s[i] == ',')) ++i;
        if (i >= n) return false;
        if (s[i] == ']') break;
        if (s[i] != '{') return false;

        const std::size_t start = i;
        int depth = 0;
        bool in_string = false;
        std::string id;
        std::size_t j = i;
        for (; j < n; ++j) {
            const char c = s[j];
            if (in_string) {
                if (c == '\\') ++j;
                else if (c == '"') in_string = false;
                continue;
            }
            if (c == '"') {
                // The entry's own "ID" key (depth 1), followed by ':' and a string value.
                if (depth == 1 && id.empty() && j + 3 < n && s[j + 1] == 'I' && s[j + 2] == 'D'
                    && s[j + 3] == '"') {
                    std::size_t k = j + 4;
                    while (k < n && isSpace(s[k])) ++k;
                    if (k < n && s[k] == ':') {
                        ++k;
                        while (k < n && isSpace(s[k])) ++k;
                        if (k < n && s[k] == '"') {
                            const std::size_t e = text.find('"', k + 1);
                            if (e != std::string::npos) id = text.substr(k + 1, e - k - 1);
                        }
                    }
                }
                in_string = true;
                continue;
            }
            if (c == '{' || c == '[') {
                ++depth;
            } else if (c == '}' || c == ']') {
                if (--depth == 0) break;
            }
        }
        if (j >= n) return false;
        if (keep.count(id) != 0) {
            if (kept != 0) body += ", ";
            body.append(text, start, j + 1 - start);
            ++kept;
        }
        i = j + 1;
    }
    out = text.substr(0, open + 1) + body + text.substr(i);   // i is at the closing ']'
    return true;
}

/** @brief The loader's internal data root for @p j (files.internal_data_root, then the legacy key). */
inline std::filesystem::path internalDataRoot(const nlohmann::json& j) {
    std::string raw;
    if (j.contains("files") && j["files"].is_object() && j["files"].contains("internal_data_root"))
        raw = j["files"]["internal_data_root"].get<std::string>();
    else if (j.contains("internal_data_root"))
        raw = j["internal_data_root"].get<std::string>();
    const std::filesystem::path project_root = GNSS_PROJECT_ROOT;
    if (raw.empty()) return project_root / "data";
    const std::filesystem::path p(raw);
    return p.is_relative() ? project_root / p : p;
}

/** @brief True when loading @p j would auto-discover and parse the registry files. */
inline bool loadsDiscoveredRegistry(const nlohmann::json& j) {
    const auto flag = [&j](const char* section, const char* key, bool dflt) {
        if (j.contains(section) && j[section].is_object() && j[section].contains(key)
            && j[section][key].is_boolean())
            return j[section][key].get<bool>();
        return dflt;
    };
    const bool station_info = flag("processing", "enable_station_info", false)
                              || (j.contains("enable_station_info") && j["enable_station_info"].is_boolean()
                                  && j["enable_station_info"].get<bool>());
    const bool otl_from_sif = flag("model_corrections", "enable_ocean_tide_loading", true)
                              && flag("model_corrections", "otl_from_sif", false);
    if (!station_info && !otl_from_sif) return false;
    const bool explicit_paths =
        (j.contains("files") && j["files"].is_object()
         && (j["files"].contains("station_info_paths") || j["files"].contains("station_info_path")))
        || j.contains("station_info_path");
    return !explicit_paths;
}

}  // namespace reference_config_detail

/**
 * @brief @p j with files.station_info_paths naming registry copies cut to its stations (see the
 *        file comment); @p j unchanged when the cut does not apply.
 *
 * The copies are written once per process and roster, under the process scratch root.
 *
 * @param j A configuration document.
 * @return The configuration to load in its place.
 */
inline nlohmann::json withRosterStationInfo(nlohmann::json j) {
    namespace fs = std::filesystem;
    namespace d = reference_config_detail;
    if (!d::loadsDiscoveredRegistry(j)) return j;

    // The registry files as the loader discovers them: *.json under static/sif, sorted.
    const fs::path sif_dir = d::internalDataRoot(j) / "static" / "sif";
    std::vector<fs::path> originals;
    std::error_code ec;
    if (!fs::is_directory(sif_dir, ec)) return j;
    for (const auto& e : fs::directory_iterator(sif_dir, ec))
        if (e.is_regular_file() && e.path().extension() == ".json") originals.push_back(e.path());
    std::sort(originals.begin(), originals.end());
    if (originals.empty()) return j;

    std::set<std::string> names;
    d::collectStrings(j, names);
    std::string key = sif_dir.generic_string();
    for (const std::string& s : names) key += "|" + s;

    static std::map<std::string, std::vector<std::string>> cut_paths;   // roster key -> copies
    auto it = cut_paths.find(key);
    if (it == cut_paths.end()) {
        tempDir();   // creates the process root on first use
        char tag[32];
        std::snprintf(tag, sizeof(tag), "roster_sif_%08zx",
                      std::hash<std::string>{}(key) & 0xFFFFFFFFu);
        const fs::path dir = detail::processRootPath() / tag;
        fs::create_directories(dir, ec);
        std::vector<std::string> paths;
        std::size_t total = 0;
        for (const fs::path& src : originals) {
            std::string cut;
            std::size_t kept = 0;
            if (!d::cutStations(d::readBytes(src), names, cut, kept)) return j;
            const fs::path dst = dir / src.filename();
            std::ofstream(dst, std::ios::binary) << cut;
            paths.push_back(dst.generic_string());
            total += kept;
        }
        if (total == 0) paths.clear();   // no roster station in the registry: keep the original
        it = cut_paths.emplace(key, std::move(paths)).first;
    }
    if (it->second.empty()) return j;
    j["files"]["station_info_paths"] = it->second;
    return j;
}

/**
 * @brief A configuration file under config/ parsed once per process (comments allowed, as the
 *        loader allows them), with withRosterStationInfo() applied.
 *
 * @param relative Path under config/, e.g. "golden/golden_field_refound.json".
 * @return A copy of the cached document, free to edit.
 */
inline nlohmann::json referenceConfigWithRosterStationInfo(const std::string& relative) {
    static std::map<std::string, nlohmann::json> cache;
    auto it = cache.find(relative);
    if (it == cache.end()) {
        const std::filesystem::path src =
            std::filesystem::path(GNSS_PROJECT_ROOT) / "config" / relative;
        std::ifstream in(src);
        nlohmann::json parsed = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/true,
                                                      /*ignore_comments=*/true);
        it = cache.emplace(relative, withRosterStationInfo(std::move(parsed))).first;
    }
    return it->second;
}

}  // namespace gnss_test
