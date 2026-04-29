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
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace GnssProcessor {

    /**
     * @brief Observations from multiple receivers aligned to a single GPS epoch.
     *
     * Produced by @c MultiRinexSync::synchronize().  The @c time field is
     * taken from the reference station's epoch — rover observations
     * matched within @c BaselineConfig::epoch_sync_tolerance_s are entered
     * into @c by_station under their station name.  Stations whose
     * observations fall outside the tolerance at this epoch are simply
     * absent from the map; the caller decides whether to process a
     * partial epoch or skip it.
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
         * @brief Parse each station's RINEX observation file.
         * @return True if every station parsed successfully; false if any station failed.
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
         * @return True on success; false if a station is unloaded or the reference is missing.
         */
        bool synchronize();

        /// Access the synchronized epoch vector populated by @c synchronize().
        const std::vector<SyncedEpoch>& getSyncedEpochs() const { return synced_; }

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
    };

} // namespace GnssProcessor
