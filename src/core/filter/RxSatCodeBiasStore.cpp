/**
 * @file RxSatCodeBiasStore.cpp
 * @brief The receiver-satellite code-bias store: identity checks, the age rule and the JSON file
 *        (see filter/RxSatCodeBiasStore.hpp).
 */

#include "filter/RxSatCodeBiasStore.hpp"

#include "filter/StateKeySchema.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>
#include <system_error>

namespace GnssProcessor {

namespace {

/// A character of the long IGS product naming: the id and the start epoch are fixed width.
constexpr std::size_t kProductIdLength   = 10;   ///< "COD0MGXFIN"
constexpr std::size_t kStartEpochLength  = 11;   ///< "20241450000"

/// Whether @p text is the eleven-digit start epoch of the long IGS naming.
bool isStartEpochField(const std::string& text) {
    return text.size() == kStartEpochLength
        && std::all_of(text.begin(), text.end(),
                       [](unsigned char c) { return std::isdigit(c) != 0; });
}

/// Writes a failure reason when the caller asked for one.
std::nullopt_t refuse(std::string* why, const std::string& reason) {
    if (why != nullptr) *why = reason;
    return std::nullopt;
}

} // namespace

std::string normalizeIdentityText(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string();
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string biasSourceIdentity(const std::string& bia_file_name) {
    const std::string name = normalizeIdentityText(bia_file_name);
    if (name.empty()) return "none";
    const auto first = name.find('_');
    if (first == kProductIdLength) {
        const auto second = name.find('_', first + 1);
        if (second != std::string::npos
            && isStartEpochField(name.substr(first + 1, second - first - 1)))
            return name.substr(0, first);
    }
    return name;
}

RxSatCodeBiasVerdict checkRxSatCodeBiasStore(const RxSatCodeBiasStore& store,
                                             const RxSatCodeBiasIdentity& run) {
    const auto same = [](const std::string& a, const std::string& b) {
        return normalizeIdentityText(a) == normalizeIdentityText(b);
    };
    if (!same(store.identity.receiver_type, run.receiver_type))
        return RxSatCodeBiasVerdict::ReceiverTypeDiffers;
    if (!same(store.identity.receiver_version, run.receiver_version))
        return RxSatCodeBiasVerdict::ReceiverVersionDiffers;
    if (!same(store.identity.bias_source, run.bias_source))
        return RxSatCodeBiasVerdict::BiasSourceDiffers;
    return RxSatCodeBiasVerdict::Usable;
}

const char* rxSatCodeBiasVerdictText(RxSatCodeBiasVerdict verdict) {
    switch (verdict) {
        case RxSatCodeBiasVerdict::Usable:                 return "usable";
        case RxSatCodeBiasVerdict::ReceiverTypeDiffers:    return "receiver type differs";
        case RxSatCodeBiasVerdict::ReceiverVersionDiffers: return "receiver version differs";
        case RxSatCodeBiasVerdict::BiasSourceDiffers:      return "bias source differs";
    }
    return "?";
}

double ageInflatedVariance(double sigma_m, double q_m2_per_s, double age_s) {
    return sigma_m * sigma_m + q_m2_per_s * std::max(age_s, 0.0);
}

double secondsBetween(const GnssTime& later, const GnssTime& earlier) {
    return static_cast<double>(later.week - earlier.week) * static_cast<double>(TimeUtils::SECONDS_IN_WEEK)
         + (later.tow - earlier.tow);
}

std::optional<RxSatCodeBiasPrior> storedRxSatCodeBiasPrior(const RxSatCodeBiasStore& store,
                                                           const std::string& key,
                                                           const GnssTime& now, double q_m2_per_s) {
    const auto it = store.entries.find(key);
    if (it == store.entries.end()) return std::nullopt;
    const RxSatCodeBiasEntry& e = it->second;
    return RxSatCodeBiasPrior{ e.value_m,
                               ageInflatedVariance(e.sigma_m, q_m2_per_s, secondsBetween(now, e.updated)) };
}

std::optional<RxSatCodeBiasStore> readRxSatCodeBiasStore(const std::filesystem::path& path,
                                                         std::string* why) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return refuse(why, "the file does not exist");
    std::ifstream in(path);
    if (!in) return refuse(why, "the file cannot be opened");

    nlohmann::json doc;
    try {
        in >> doc;
    } catch (const std::exception& e) {
        return refuse(why, std::string("not valid JSON: ") + e.what());
    }
    try {
        if (!doc.is_object() || doc.value("marker", std::string()) != kRxSatCodeBiasStoreMarker)
            return refuse(why, "not a receiver-satellite code-bias store (marker)");
        if (doc.value("schema", 0) != kRxSatCodeBiasStoreSchema)
            return refuse(why, "another store schema");

        RxSatCodeBiasStore store;
        store.identity.receiver_type    = normalizeIdentityText(doc.value("receiver_type", std::string()));
        store.identity.receiver_version = normalizeIdentityText(doc.value("receiver_version", std::string()));
        store.identity.bias_source      = normalizeIdentityText(doc.value("bias_source", std::string()));

        for (const auto& item : doc.at("entries")) {
            RxSatCodeBiasEntry e;
            e.satellite  = item.at("satellite").get<std::string>();
            e.signal     = item.at("signal").get<std::string>();
            e.value_m    = item.at("value_m").get<double>();
            e.sigma_m    = item.at("sigma_m").get<double>();
            e.updated.week = item.at("week").get<int>();
            e.updated.tow  = item.at("tow").get<double>();
            const std::string key = StateKeys::makeRxSatCodeBiasKey(e.satellite, e.signal);
            if (!StateKeys::parseRxSatCodeBiasKey(key))
                return refuse(why, "an entry with a key that is no receiver-satellite code-bias key: " + key);
            if (!std::isfinite(e.value_m) || !std::isfinite(e.sigma_m) || !(e.sigma_m > 0.0))
                return refuse(why, "an entry with a non-finite value or a non-positive sigma: " + key);
            store.entries[key] = e;
        }
        return store;
    } catch (const std::exception& e) {
        return refuse(why, std::string("malformed store: ") + e.what());
    }
}

bool writeRxSatCodeBiasStore(const std::filesystem::path& path, const RxSatCodeBiasStore& store) {
    nlohmann::json doc;
    doc["marker"]           = kRxSatCodeBiasStoreMarker;
    doc["schema"]           = kRxSatCodeBiasStoreSchema;
    doc["receiver_type"]    = store.identity.receiver_type;
    doc["receiver_version"] = store.identity.receiver_version;
    doc["bias_source"]      = store.identity.bias_source;
    nlohmann::json entries = nlohmann::json::array();
    for (const auto& [key, e] : store.entries) {
        nlohmann::json item;
        item["satellite"] = e.satellite;
        item["signal"]    = e.signal;
        item["value_m"]   = e.value_m;
        item["sigma_m"]   = e.sigma_m;
        item["week"]      = e.updated.week;
        item["tow"]       = e.updated.tow;
        entries.push_back(std::move(item));
    }
    doc["entries"] = std::move(entries);

    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        out << doc.dump(2) << "\n";
        if (!out) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        // A rename over an existing file can be refused; replace it explicitly.
        std::filesystem::remove(path, ec);
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    return true;
}

} // namespace GnssProcessor
