/*!
 * \file RetiredConfigKeys.cpp
 * \brief The scan of a configuration for retired keys and its warnings.
 *
 * The table itself is in include/config/RetiredConfigKeys.hpp, which says why it is not in
 * this file.  This file holds no key name.
 */

#include "config/RetiredConfigKeys.hpp"
#include "config/GnssConfigJsonHelpers.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

namespace GnssProcessor {

    namespace {

        /// The longest value text a warning carries; a longer value is cut and marked.
        constexpr std::size_t kMaxValueText = 60;

        /// A JSON value as compact text, cut to @ref kMaxValueText characters.
        std::string valueText(const nlohmann::json& v) {
            std::string s = v.dump();
            if (s.size() > kMaxValueText) s = s.substr(0, kMaxValueText - 3) + "...";
            return s;
        }

        /// The objects of the array at @p array_path that hold @p member: their count goes to
        /// @p hit.entries and the first one's value to @p hit.value.
        void findArrayMember(const nlohmann::json& root, const std::string& array_path,
                             const std::string& member, RetiredKeyHit& hit) {
            const nlohmann::json* list = detail::resolvePath(root, array_path);
            if (!list || !list->is_array()) return;
            for (const auto& entry : *list) {
                if (!entry.is_object() || !entry.contains(member)) continue;
                if (hit.entries == 0) hit.value = valueText(entry.at(member));
                ++hit.entries;
            }
        }

    }  // namespace

    std::vector<RetiredKeyHit> findRetiredConfigKeys(const nlohmann::json& root) {
        std::vector<RetiredKeyHit> hits;
        if (!root.is_object()) return hits;
        for (const RetiredConfigKey& key : kRetiredConfigKeys) {
            RetiredKeyHit hit;
            hit.key = &key;
            const std::string path(key.path);
            const std::string::size_type array_mark = path.find("[].");
            if (array_mark != std::string::npos) {
                findArrayMember(root, path.substr(0, array_mark), path.substr(array_mark + 3), hit);
                if (hit.entries > 0) hit.spellings.push_back(path);
            } else {
                // The old loader's order: the dotted path first, then the flat key at the top.
                if (const nlohmann::json* v = detail::resolvePath(root, path)) {
                    hit.spellings.push_back(path);
                    hit.value = valueText(*v);
                }
                const std::string flat(key.flat);
                if (!flat.empty() && root.contains(flat)) {
                    if (hit.spellings.empty()) hit.value = valueText(root.at(flat));
                    hit.spellings.push_back(flat);
                }
            }
            if (!hit.spellings.empty()) hits.push_back(std::move(hit));
        }
        return hits;
    }

    std::string retiredConfigKeyMessage(const RetiredKeyHit& hit) {
        const RetiredConfigKey& key = *hit.key;
        const RetiredKeyFamily& family = *key.family;
        std::string how;
        if (hit.entries > 0) {
            const std::string_view array_path = key.path.substr(0, key.path.find("[]."));
            how = fmt::format("set in {} {} of {}, first value {}", hit.entries,
                              hit.entries == 1 ? "entry" : "entries", array_path, hit.value);
        } else if (hit.spellings.size() == 1 && hit.spellings.front() == key.path) {
            how = fmt::format("value {}", hit.value);
        } else {
            how = fmt::format("spelled {}, value {}", detail::joinStrings(hit.spellings, " and "),
                              hit.value);
        }
        return fmt::format("Config: retired key {} ({}) is ignored. Card {} deleted it on {} with {} "
                           "(commit {}); the key has no effect.",
                           key.path, how, family.card, family.date, family.what, family.commit);
    }

    std::size_t warnRetiredConfigKeys(const nlohmann::json& root, spdlog::logger& log) {
        const std::vector<RetiredKeyHit> hits = findRetiredConfigKeys(root);
        for (const RetiredKeyHit& hit : hits) log.warn("{}", retiredConfigKeyMessage(hit));
        return hits.size();
    }

}  // namespace GnssProcessor
