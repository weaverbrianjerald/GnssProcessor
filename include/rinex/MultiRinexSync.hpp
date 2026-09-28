#pragma once

/**
 * @file MultiRinexSync.hpp
 * @brief Multi-station RINEX loader and per-epoch synchronizer.
 *
 * Used by double-differenced baseline / network processing.  Owns one
 * RinexParser per configured station (reference + rovers) and produces
 * a time-aligned vector of @c SyncedEpoch records so that the DD
 * measurement model can iterate over epochs where multiple receivers
 * observed simultaneously.
 */

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "rinex/RinexParser.hpp"
#include "products/ProductParser.hpp"
#include "math/TimeUtils.hpp"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace GnssProcessor {

    /**
     * @brief Observations from multiple receivers aligned to a single GPS epoch.
     *
     * Produced by @c MultiRinexSync::synchronize().  The @c time field is
     * taken from the reference station's epoch; rover observations
     * matched within @c BaselineConfig::epoch_sync_tolerance_s are entered
     * into @c by_station under their station name.  Stations whose
     * observations fall outside the tolerance at this epoch are absent
     * from the map; the caller decides whether to process a partial epoch
     * or skip it.
     */
    struct SyncedEpoch {
        GnssTime time{};                                                 ///< Reference epoch time (GPS scale).
        std::map<std::string, std::vector<RinexObservation>> by_station; ///< Station name -> satellite observations.
    };

    /**
     * @brief Loads and aligns RINEX observations from multiple receivers.
     *
     * Construction only wires up per-station configs and parsers; no file
     * I/O happens until @c load() is called.  @c synchronize() must be
     * called after a successful load.  Both are separable so the caller
     * can report load errors before doing the (comparatively cheap)
     * alignment work.
     *
     * Ownership model: each station keeps its own copy of the base
     * GnssConfig with @c observation_path swapped, so the parser's
     * @c const GnssConfig& reference remains valid for the lifetime of
     * the @c MultiRinexSync instance.
     */
    class MultiRinexSync {
    public:
        /**
         * @brief Construct the synchronizer.
         *
         * @param base_config      Global configuration (copied per station).
         * @param baseline_config  Stations to load and align.
         * @param log_manager      Shared log manager.
         * @param product_parser   Shared product parser required by RinexParser constructors.
         */
        MultiRinexSync(const GnssConfig& base_config,
                       const BaselineConfig& baseline_config,
                       LogManager& log_manager,
                       ProductParser& product_parser);

        /**
         * @brief Clip every station's parse to the first @p span_s seconds.
         *
         * Forwarded to each station's @c RinexParser before @c load().  A
         * caller that consumes only a short data span (e.g. a network solve
         * bounded by @c processing.max_duration_s) then skips parsing, and the
         * synchronizer skips copying, the unused remainder of full-day files.
         * Must be called before @c load() to take effect.  A value @c <= 0
         * (the default) means "no window" (parse full files), so the baseline
         * DD path is unchanged unless it opts in.
         *
         * @param span_s Maximum span in seconds from each file's first epoch.
         */
        void setParseWindowSeconds(double span_s) { parse_window_s_ = span_s; }

        /**
         * @brief Parse each station's RINEX observation file.
         *
         * A station that fails to parse is logged and left @c loaded=false; it
         * is skipped by @c synchronize() rather than sinking the whole roster.
         * @return True if every station parsed successfully; false if any
         *         station failed (callers that tolerate partial rosters should
         *         check @c parserOf() for the stations they require instead of
         *         treating a false return as fatal).
         */
        bool load();

        /**
         * @brief Build the time-aligned SyncedEpoch vector.
         *
         * Walks the reference station's epochs as the schedule.  For each
         * reference epoch, searches each rover's epoch list (forward from
         * a cursor position) for a match within
         * @c BaselineConfig::epoch_sync_tolerance_s.  Rover epochs with no
         * matching reference epoch are skipped entirely (no DD is possible
         * without the reference).
         *
         * Stations that failed to load are skipped with a warning; the
         * schedule is built from the stations that did load.
         *
         * @return True on success; false when the reference station is not loaded
         *         or has no observations.
         */
        bool synchronize();

        /// Access the synchronized epoch vector populated by @c synchronize().
        const std::vector<SyncedEpoch>& getSyncedEpochs() const { return synced_; }

        /**
         * @brief Keep only the observations of the active systems (card OWD, O4).
         *
         * The network producer's single admission point for constellations.  The RINEX
         * parser keeps every system and the network admits an observation by its signal
         * priority list, while the receiver inter-system bias is allocated by
         * @c processing.active_systems.  Removing the inactive systems once, right after
         * the sync, makes the two agree for every downstream reader of the synced epochs
         * (the model, the admission censuses, the closed-loop consumer feed).  Station
         * keys stay in place even when a station's vector empties, so a station with no
         * usable observation in an epoch is represented the same way as without the
         * filter.
         *
         * @param epochs The synced epochs, filtered in place.
         * @param active The active system names (@c processing.active_systems: "GPS",
         *               "Galileo", "GLONASS", "BeiDou", "QZSS", "SBAS").
         * @return The number of observations removed per system name (systems with
         *         removals only; empty when every observation was already active).
         */
        static std::map<std::string, std::size_t> retainSystems(
            std::vector<SyncedEpoch>& epochs, const std::vector<std::string>& active);

        /// @ref retainSystems applied to this instance's synced epochs (card OWD, O4).
        /// @param active The active system names.
        /// @return The number of observations removed per system name.
        std::map<std::string, std::size_t> retainSystems(const std::vector<std::string>& active) {
            return retainSystems(synced_, active);
        }

        /**
         * @brief Card HLT: remove the observations a predicate selects from synced epochs.
         *
         * The second door beside @ref retainSystems, for a rule that depends on the epoch
         * time (the broadcast health in force).  Station keys stay in place, as there.
         *
         * @param epochs The synced epochs, filtered in place.
         * @param drop   Called with the epoch time and each observation; true removes it.
         * @return The number of observations removed.
         */
        static std::size_t removeObservationsIf(
            std::vector<SyncedEpoch>& epochs,
            const std::function<bool(const GnssTime&, const RinexObservation&)>& drop);

        /// Card HLT: @ref removeObservationsIf applied to this instance's synced epochs.
        /// @param drop Called with the epoch time and each observation; true removes it.
        /// @return The number of observations removed.
        std::size_t removeObservationsIf(
            const std::function<bool(const GnssTime&, const RinexObservation&)>& drop) {
            return removeObservationsIf(synced_, drop);
        }

        /// Access the parsed RINEX header for a station (by name).
        const RinexHeader& headerOf(const std::string& station) const;

        /// Access the underlying RinexParser for a station (by name).
        /// Returns nullptr when the station is unknown or not loaded.
        /// Exposed so downstream consumers (e.g. Geometry) can reach per-station
        /// header metadata such as the GLONASS frequency slot table.
        const RinexParser* parserOf(const std::string& station) const;

        /// Reference station name (convenience accessor).
        const std::string& referenceName() const { return baseline_config_.reference.name; }

    private:
        /// Per-station holding struct.  Stored in @c stations_ map so the
        /// @c config member's address is stable for the parser's reference.
        struct StationData {
            GnssConfig config;
            std::unique_ptr<RinexParser> parser;
            bool loaded{ false };
        };

        /// Per-epoch observation bucket: (time, observations at that time).
        using PerEpochObs = std::vector<std::pair<GnssTime, std::vector<RinexObservation>>>;

        /// Group a flat observation vector (sorted by time) into per-epoch buckets.
        static PerEpochObs groupByEpoch(const std::vector<RinexObservation>& obs);

        /// Locate the first rover epoch whose time is within @p tol_s of @p t,
        /// starting the search at @p start_hint (forward only).  Returns
        /// @c rover_epochs.size() when no match exists.
        static size_t findEpochWithin(const PerEpochObs& rover_epochs,
                                      size_t start_hint,
                                      const GnssTime& t,
                                      double tol_s);

        const GnssConfig& base_config_;
        const BaselineConfig& baseline_config_;
        LogManager& log_manager_;
        ProductParser& product_parser_;

        std::map<std::string, StationData> stations_;
        std::vector<SyncedEpoch> synced_;
        double parse_window_s_{ 0.0 };  ///< Per-file parse-window span (s); <=0 means unbounded.
    };

} // namespace GnssProcessor
