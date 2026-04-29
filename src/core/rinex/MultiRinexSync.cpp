/*!
 * \file MultiRinexSync.cpp
 * \brief Implementation of the multi-station RINEX loader and synchronizer.
 */

#include "rinex/MultiRinexSync.hpp"
#include "math/TimeUtils.hpp"
#include <algorithm>
#include <cmath>
#include <utility>

namespace GnssProcessor {

    // ===================================================================
    // Construction
    // ===================================================================

    MultiRinexSync::MultiRinexSync(const GnssConfig& base_config,
                                   const BaselineConfig& baseline_config,
                                   LogManager& log_manager,
                                   ProductParser& product_parser)
        : base_config_(base_config),
          baseline_config_(baseline_config),
          log_manager_(log_manager),
          product_parser_(product_parser) {

        auto addStation = [&](const StationConfig& sc) {
            // Emplace first, then initialise in-place so the RinexParser's
            // const-reference to StationData::config remains valid for the
            // lifetime of the map entry (std::map does not invalidate
            // addresses on subsequent inserts).
            auto [it, inserted] = stations_.emplace(sc.name, StationData{});
            if (!inserted) {
                log_manager_.getLogger("main_log")->warn(
                    "Duplicate station name '{}' in baseline config; ignoring second entry",
                    sc.name);
                return;
            }
            it->second.config = base_config_;
            it->second.config.setObservationPath(sc.rinex_path);
            it->second.parser = std::make_unique<RinexParser>(
                it->second.config, log_manager_, product_parser_);
        };

        addStation(baseline_config_.reference);
        for (const auto& rv : baseline_config_.rovers) {
            addStation(rv);
        }
    }

    // ===================================================================
    // RINEX loading
    // ===================================================================

    bool MultiRinexSync::load() {
        auto log = log_manager_.getLogger("main_log");
        bool ok = true;
        for (auto& [name, sd] : stations_) {
            log->info("Loading RINEX for station '{}': {}",
                      name, sd.config.getObservationPath().string());
            if (!sd.parser->parse()) {
                log->error("Failed to parse RINEX for station '{}'", name);
                ok = false;
                continue;
            }
            sd.loaded = true;
            const auto& h = sd.parser->getHeader();
            log->info("  station '{}': {} observations, interval={}s, marker='{}'",
                      name,
                      sd.parser->getObservations().size(),
                      h.interval.value_or(0.0),
                      h.marker_name);
        }
        return ok;
    }

    // ===================================================================
    // Per-epoch grouping
    // ===================================================================

    MultiRinexSync::PerEpochObs
    MultiRinexSync::groupByEpoch(const std::vector<RinexObservation>& obs) {
        PerEpochObs out;
        if (obs.empty()) return out;

        GnssTime current = obs.front().time;
        std::vector<RinexObservation> bucket;

        auto sameEpoch = [](const GnssTime& a, const GnssTime& b) {
            // Observations at the same epoch share an identical RINEX time tag.
            // Allow 1 µs slack for floating-point round-trip.
            return a.week == b.week && std::abs(a.tow - b.tow) < 1e-6;
        };

        for (const auto& o : obs) {
            if (!sameEpoch(o.time, current)) {
                if (!bucket.empty()) out.emplace_back(current, std::move(bucket));
                bucket.clear();
                current = o.time;
            }
            bucket.push_back(o);
        }
        if (!bucket.empty()) out.emplace_back(current, std::move(bucket));
        return out;
    }

    // ===================================================================
    // Forward-only epoch matching
    // ===================================================================

    size_t MultiRinexSync::findEpochWithin(const PerEpochObs& rover_epochs,
                                           size_t start_hint,
                                           const GnssTime& t,
                                           double tol_s) {
        const double t_sec = TimeUtils::continuousGpsSeconds(t);
        for (size_t i = start_hint; i < rover_epochs.size(); ++i) {
            const double d = TimeUtils::continuousGpsSeconds(rover_epochs[i].first) - t_sec;
            if (std::abs(d) <= tol_s) return i;
            if (d > tol_s) break;   // rover has moved past reference; no match at this t.
        }
        return rover_epochs.size();
    }

    // ===================================================================
    // Synchronization
    // ===================================================================

    bool MultiRinexSync::synchronize() {
        auto log = log_manager_.getLogger("main_log");

        // Group each station's observations by epoch time.
        std::map<std::string, PerEpochObs> per_station;
        for (const auto& [name, sd] : stations_) {
            if (!sd.loaded) {
                log->error("MultiRinexSync::synchronize(): station '{}' not loaded", name);
                return false;
            }
            per_station.emplace(name, groupByEpoch(sd.parser->getObservations()));
        }

        const auto& ref_name = baseline_config_.reference.name;
        auto ref_it = per_station.find(ref_name);
        if (ref_it == per_station.end() || ref_it->second.empty()) {
            log->error("MultiRinexSync::synchronize(): reference station '{}' has no observations",
                       ref_name);
            return false;
        }
        const auto& ref_epochs = ref_it->second;

        const double tol = baseline_config_.epoch_sync_tolerance_s;

        // One forward cursor per rover station.
        std::map<std::string, size_t> cursors;
        for (const auto& [name, _] : per_station) cursors[name] = 0;

        synced_.clear();
        synced_.reserve(ref_epochs.size());

        std::map<std::string, size_t> rover_hits;
        std::map<std::string, size_t> rover_misses;
        for (const auto& [name, _] : per_station) {
            if (name != ref_name) { rover_hits[name] = 0; rover_misses[name] = 0; }
        }

        for (const auto& [t, ref_obs] : ref_epochs) {
            SyncedEpoch se;
            se.time = t;
            se.by_station[ref_name] = ref_obs;

            for (auto& [name, epochs] : per_station) {
                if (name == ref_name) continue;
                size_t& cur = cursors[name];
                // Advance past rover epochs that are earlier than (t - tol)
                // so the search starts at a meaningful position.
                const double t_lo = TimeUtils::continuousGpsSeconds(t) - tol;
                while (cur < epochs.size()
                       && TimeUtils::continuousGpsSeconds(epochs[cur].first) < t_lo) {
                    ++cur;
                }
                const size_t idx = findEpochWithin(epochs, cur, t, tol);
                if (idx < epochs.size()) {
                    se.by_station[name] = epochs[idx].second;
                    cur = idx + 1;
                    ++rover_hits[name];
                } else {
                    ++rover_misses[name];
                }
            }
            synced_.push_back(std::move(se));
        }

        log->info("MultiRinexSync: aligned {} epochs against reference '{}'",
                  synced_.size(), ref_name);
        for (const auto& [name, hits] : rover_hits) {
            const size_t misses = rover_misses[name];
            const double pct = ref_epochs.empty() ? 0.0 : 100.0 * hits / ref_epochs.size();
            log->info("  rover '{}': {}/{} matched ({:.1f}%), {} missing",
                      name, hits, ref_epochs.size(), pct, misses);
        }
        return true;
    }

    // ===================================================================
    // Header access
    // ===================================================================

    const RinexHeader& MultiRinexSync::headerOf(const std::string& station) const {
        static const RinexHeader empty;
        auto it = stations_.find(station);
        if (it == stations_.end() || !it->second.parser) return empty;
        return it->second.parser->getHeader();
    }

    const RinexParser* MultiRinexSync::parserOf(const std::string& station) const {
        auto it = stations_.find(station);
        if (it == stations_.end() || !it->second.loaded) return nullptr;
        return it->second.parser.get();
    }

} // namespace GnssProcessor
