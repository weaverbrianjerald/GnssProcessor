/*!
 * \file MultiRinexSync.cpp
 * \brief Implementation of the multi-station RINEX loader and synchronizer.
 */

#include "rinex/MultiRinexSync.hpp"
#include "math/TimeUtils.hpp"
#include "common/PppUtils.hpp"   // card OWD: system names for retainSystems
#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#include <utility>
#include <vector>
#ifdef _OPENMP
#include <omp.h>       // omp_get_max_threads — per-station RINEX parse parallelism
#endif
#include <Eigen/Core>  // Eigen::nbThreads / setNbThreads — pin to 1 inside the region

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

        // ------------------------------------------------------------------
        // Gather stable per-station handles into an index-ordered vector.
        //
        // std::map is not random-access, so an OpenMP `parallel for` (which
        // needs an integer loop counter) cannot iterate it directly.  We
        // snapshot a {name, StationData*} pair per entry in map order.  The
        // node addresses are stable for the lifetime of stations_ (std::map
        // never relocates nodes), so each worker may freely mutate the
        // StationData it is handed without touching the map.  The index order
        // is the map's key order — exactly the deterministic order the
        // post-parse summary must preserve.
        // ------------------------------------------------------------------
        struct Item { const std::string* name; StationData* sd; };
        std::vector<Item> items;
        items.reserve(stations_.size());
        for (auto& [name, sd] : stations_) items.push_back(Item{ &name, &sd });
        const int n_sta_total = static_cast<int>(items.size());

        // ------------------------------------------------------------------
        // Parallel-on decision + worker-thread count.
        //
        // Mirrors the network producer's per-station gate
        // (ApplicationNetwork.cpp) so ingestion shares one parallelism story
        // with the solve: parallelize only when the operator enabled it, the
        // roster strictly exceeds the min-station threshold, and the hardware
        // offers more than one thread.  base_config_ already carries the
        // NetworkConfig knobs, so no extra plumbing is required.
        // ------------------------------------------------------------------
        const NetworkConfig& nc = base_config_.getNetworkConfig();

        int hw_max_threads = 1;
#ifdef _OPENMP
        hw_max_threads = omp_get_max_threads();
#endif
        const bool parallel_on = nc.parallel_stations
                                 && n_sta_total > nc.parallel_min_stations
                                 && hw_max_threads > 1;
        const int n_threads = parallel_on
            ? (nc.parallel_max_threads > 0
                   ? std::min(nc.parallel_max_threads, hw_max_threads)
                   : hw_max_threads)
            : 1;

#ifndef _OPENMP
        if (nc.parallel_stations && n_sta_total > nc.parallel_min_stations) {
            log->warn("Parallel RINEX ingestion requested ({} stations) but this "
                      "build has no OpenMP; loading serially.", n_sta_total);
        }
#endif
        if (parallel_on) {
            log->info("Parallel RINEX ingestion: {} stations across {} threads "
                      "(thread-count-invariant; results + logs ordered by station).",
                      n_sta_total, n_threads);
        }

        // ------------------------------------------------------------------
        // Logger prewarm — the one shared-mutable hazard on the parse path.
        //
        // RinexParser re-fetches getLogger("rinex_parser") (uncached) on every
        // log line.  getLogger()'s first-touch CREATE mutates the shared
        // loggers_ map, while the existing-logger fetch is a lock-free find; a
        // find racing that insert is UB (LogManager.cpp documents the prewarm
        // contract).  Creating "rinex_parser" single-threaded now guarantees
        // only the lock-free fast path runs inside the region.  ("main_log" is
        // already warm — fetched above; "product_parser", touched only in the
        // parse-exception catch, is created at ProductParser construction.)
        // ------------------------------------------------------------------
        if (parallel_on) (void)log_manager_.getLogger("rinex_parser");

        // RINEX parsing does not exercise Eigen, so pinning is defensive: keep
        // any incidental Eigen call from spawning nested threads that
        // oversubscribe the station pool.  Guarded on parallel_on so the serial
        // path is byte-identical to the pre-parallel code.
        const int eigen_threads_saved = Eigen::nbThreads();
        if (parallel_on) Eigen::setNbThreads(1);

        // ------------------------------------------------------------------
        // Per-station result slots (indexed, never push_back).
        //
        // Each worker writes ONLY its own slot i, capturing every datum the
        // deterministic post-region summary needs, so the parallel body emits
        // NO logs and touches NO shared state.  `ok` is a plain bool in a POD
        // (NOT a std::vector<bool>, whose packed bits would race on adjacent
        // writes).
        // ------------------------------------------------------------------
        struct Slot {
            bool        ok{ false };       ///< parse() succeeded.
            bool        threw{ false };    ///< An exception escaped parse() (caught per-iteration).
            std::string err;               ///< Exception message when threw.
            std::string obs_path;          ///< Observation path (for the "Loading..." line).
            std::size_t obs_count{ 0 };    ///< Observation count for the summary line.
            double      interval{ 0.0 };   ///< Header sampling interval (s) for the summary line.
            std::string marker;            ///< Header marker name for the summary line.
        };
        std::vector<Slot> slots(static_cast<std::size_t>(n_sta_total));

        // Wall-clock the parse region only (not product load / synchronize) so
        // the reported number isolates ingestion cost — and is directly
        // comparable parallel-vs-serial, since the gate that serializes the
        // later epoch loop does not affect this bracket.
        const auto t_parse_start = std::chrono::steady_clock::now();

        // ------------------------------------------------------------------
        // PARALLEL PRODUCE — pure per-station compute into the owned slot.
        //
        // Standard pragma form for this codebase: parallel for +
        // schedule(dynamic) (station files vary widely in size) + num_threads
        // bound to the cap + if(parallel_on) so the SAME loop runs serially
        // below the gate with no separate branch.  Every exception is caught
        // here — letting one escape an OpenMP structured block is UB.
        // ------------------------------------------------------------------
#pragma omp parallel for schedule(dynamic) num_threads(n_threads) if(parallel_on)
        for (int i = 0; i < n_sta_total; ++i) {
            StationData* sd = items[static_cast<std::size_t>(i)].sd;
            Slot&        r  = slots[static_cast<std::size_t>(i)];
            r.obs_path = sd->config.getObservationPath().string();
            try {
                sd->parser->setMaxObservationSpanSeconds(parse_window_s_);
                if (sd->parser->parse()) {
                    sd->loaded = true;  // worker mutates ONLY its own StationData.
                    const auto& h = sd->parser->getHeader();
                    r.ok        = true;
                    r.obs_count = sd->parser->getObservations().size();
                    r.interval  = h.interval.value_or(0.0);
                    r.marker    = h.marker_name;
                }
            } catch (const std::exception& e) {
                r.threw = true;
                r.err   = e.what();
            } catch (...) {
                r.threw = true;
                r.err   = "unknown exception";
            }
        }

        if (parallel_on) Eigen::setNbThreads(eigen_threads_saved);

        const double parse_s =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t_parse_start).count();
        log->info("RINEX ingestion: parsed {} stations in {:.3f} s ({}).",
                  n_sta_total, parse_s, parallel_on ? "parallel" : "serial");

        // ------------------------------------------------------------------
        // SERIAL DRAIN — emit every log line in deterministic map order and
        // fold the aggregate result.  Reproduces the EXACT pre-parallel log
        // lines, just relocated out of the worker so ordering is
        // thread-count-invariant.
        // ------------------------------------------------------------------
        bool ok = true;
        for (int i = 0; i < n_sta_total; ++i) {
            const std::string& name = *items[static_cast<std::size_t>(i)].name;
            const Slot&        r    = slots[static_cast<std::size_t>(i)];

            log->info("Loading RINEX for station '{}': {}", name, r.obs_path);

            if (r.threw) {
                // A worker caught an exception the serial path would have let
                // propagate; surface it as a parse failure rather than
                // re-throwing, keeping load()'s partial-roster contract.
                log->error("Failed to parse RINEX for station '{}': {}", name, r.err);
                ok = false;
                continue;
            }
            if (!r.ok) {
                log->error("Failed to parse RINEX for station '{}'", name);
                ok = false;
                continue;
            }
            log->info("  station '{}': {} observations, interval={}s, marker='{}'",
                      name, r.obs_count, r.interval, r.marker);
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
                // A failed station is skipped, not fatal: a large roster should
                // still align on the stations that did load.  The reference is
                // guarded explicitly below (its absence from per_station is the
                // genuine fatal case — no schedule can be built without it).
                log->warn("MultiRinexSync::synchronize(): station '{}' not loaded; "
                          "excluding it from the aligned schedule", name);
                continue;
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

    std::map<std::string, std::size_t> MultiRinexSync::retainSystems(
            std::vector<SyncedEpoch>& epochs, const std::vector<std::string>& active) {
        std::map<std::string, std::size_t> removed;
        const std::set<std::string> keep(active.begin(), active.end());
        for (auto& se : epochs) {
            for (auto& [station, obs] : se.by_station) {
                (void)station;
                obs.erase(std::remove_if(obs.begin(), obs.end(),
                                         [&](const RinexObservation& o) {
                                             const std::string sys =
                                                 PppUtils::getSystemString(o.system);
                                             if (keep.count(sys)) return false;
                                             ++removed[sys];
                                             return true;
                                         }),
                          obs.end());
            }
        }
        return removed;
    }

    std::size_t MultiRinexSync::removeObservationsIf(
            std::vector<SyncedEpoch>& epochs,
            const std::function<bool(const GnssTime&, const RinexObservation&)>& drop) {
        std::size_t removed = 0;
        for (auto& se : epochs) {
            for (auto& [station, obs] : se.by_station) {
                (void)station;
                const auto keep_end = std::remove_if(
                    obs.begin(), obs.end(),
                    [&](const RinexObservation& o) { return drop(se.time, o); });
                removed += static_cast<std::size_t>(obs.end() - keep_end);
                obs.erase(keep_end, obs.end());
            }
        }
        return removed;
    }

} // namespace GnssProcessor
