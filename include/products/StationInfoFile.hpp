#pragma once

/**
 * @file StationInfoFile.hpp
 * @brief Loader and registry for NGS Station Information Files (SIFs).
 *
 * A SIF is a JSON bundle of per-station GNSS metadata (see @c StationInfo for
 * the data model and provenance).  @c StationInfoFile parses one or more SIF
 * JSON files once at startup, normalises units to SI, and provides a
 * station-name lookup, so the ocean-loading models, the a-priori coordinate
 * path and the PPP-RTK network datum all read station metadata from one
 * source.
 *
 * ## Indexing and lookup
 *
 * Stations are indexed three ways so that a record can be joined against the
 * identifier a caller holds (RINEX marker name, configured station name,
 * baseline-fixture code):
 *
 *  - by full 9-char ID (@c SSSS00CCC), upper-cased;
 *  - by DOMES number;
 *  - by 4-char station prefix (the @c SSSS of the ID), upper-cased.
 *
 * @c find() tries them in that order, case-insensitively, so a configured
 * "CIT100USA", a RINEX marker "CIT1" and a DOMES "40451S006" all resolve to
 * the same record.
 *
 * ## Memory
 *
 * The full SIF JSON document (tens of MB for the NCN file) is parsed
 * transiently and discarded; only the compact @c StationInfo structs are
 * retained.  Multiple @c load() calls accumulate (for example the IGS and NCN
 * SIFs together).
 */

#include "common/StationInfo.hpp"
#include "logging/LogManager.hpp"
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace GnssProcessor {

    /**
     * @brief A station coordinate resolved to a specific observation epoch.
     *
     * The position is the full SIF coordinate function evaluated at the query
     * epoch: the reference coordinate of the selected OCCUPATION record,
     * propagated by the station velocity, plus the optional periodic (seasonal)
     * and post-seismic terms.  The covariance is the position (3x3) block
     * propagated from the occupation's 6x6 coordinate/velocity covariance to the
     * query epoch.  Everything is in the SIF reference frame (typically ITRF2020)
     * and SI units (m, m^2).
     */
    struct ResolvedStationCoord {
        Eigen::Vector3d ecef{ Eigen::Vector3d::Zero() };       ///< ECEF position at the query epoch [m].
        Eigen::Matrix3d covariance{ Eigen::Matrix3d::Zero() }; ///< Propagated position covariance [m^2].
        bool has_covariance{ false };                          ///< True if @c covariance is meaningful.
        bool valid{ false };                                   ///< True if a usable occupation was found.
    };

    /**
     * @brief Evaluate a station's SIF coordinate function at a given epoch.
     *
     * Selects the non-flagged OCCUPATION record whose window covers
     * @p decimal_year, or, when none covers it, the non-flagged record whose
     * window midpoint is nearest.  Then forms
     * @f$ x(t) = x_{ref} + v\,(t - t_{ref}) + \sum \text{periodic}(t) + \sum \text{PSD}(t) @f$
     * following the NGS / M-PAGES convention.  The periodic phase reference and
     * the post-seismic relaxation models match the SIF generator.
     *
     * @param s              Station record.
     * @param decimal_year   Observation epoch as a decimal year.
     * @param with_periodic  Include the periodic (seasonal) terms, typically annual
     *                       and semiannual.
     * @param with_psd       Include post-seismic deformation terms.
     * @return Resolved coordinate; @c valid is false if @p s has no usable
     *         occupation, in which case the caller falls back to its own a-priori.
     */
    ResolvedStationCoord resolveStationCoordinate(const StationInfo& s,
                                                  double decimal_year,
                                                  bool with_periodic = true,
                                                  bool with_psd = true);

    /**
     * @class StationInfoFile
     * @brief Parses SIF JSON files and resolves per-station metadata by name.
     */
    class StationInfoFile {
    public:
        /**
         * @brief Construct an empty registry.
         * @param log_manager Logging facility for parse diagnostics.
         */
        explicit StationInfoFile(LogManager& log_manager);

        /**
         * @brief Parse one SIF JSON file, appending its stations to the registry.
         *
         * Units are normalised to SI during parsing (mm->m for periodic
         * amplitudes, mm/y->m/y for velocities and velocity sigmas, mm^2->m^2
         * for the off-diagonal covariance terms; coordinate sigmas are already
         * in metres).  If the file cannot be opened or parsed, or has no
         * STATIONS array, it is skipped and an error is logged.
         *
         * @param sif_json Path to a SIF JSON file.
         * @return Number of stations successfully ingested (0 on failure).
         */
        std::size_t load(const std::filesystem::path& sif_json);

        /**
         * @brief Resolve a station record by identifier.
         *
         * Tries, in order: full 9-char ID, DOMES number, 4-char station
         * prefix (the first four characters of the key).  Matching is
         * case-insensitive; surrounding whitespace is trimmed.
         *
         * @param station_key Marker name, station ID/prefix, or DOMES number.
         * @return Pointer to the matching record, or @c nullptr if not found.
         *         The pointer stays valid until the next @c load() call, which
         *         may reallocate the store.
         */
        const StationInfo* find(const std::string& station_key) const;

        /** @brief Number of stations currently held. */
        std::size_t size() const { return stations_.size(); }

        /** @brief True if no stations have been loaded. */
        bool empty() const { return stations_.empty(); }

    private:
        LogManager& log_manager_;
        std::vector<StationInfo> stations_;                 ///< Owning store; pointers from find() index into this.
        std::unordered_map<std::string, std::size_t> by_id_;    ///< Full 9-char ID (upper-case) -> index.
        std::unordered_map<std::string, std::size_t> by_id4_;   ///< 4-char prefix (upper-case) -> index (first wins).
        std::unordered_map<std::string, std::size_t> by_domes_; ///< DOMES number -> index.
    };

    /**
     * @brief A network-roster station coordinate resolved from a SIF (RTA-12).
     *
     * Result carrier for @ref resolveRosterCoordinate: the survey-grade ECEF at
     * the requested run date plus that date, which is the propagation epoch a
     * product contract declares as @c coord_epoch.
     */
    struct SifRosterCoordinate {
        Eigen::Vector3d ecef{ Eigen::Vector3d::Zero() };  ///< ECEF at @ref epoch_year [m], SIF reference frame (typically ITRF2020).
        double          epoch_year{ 0.0 };                ///< Decimal year the coordinate was propagated to.
    };

    /**
     * @brief Resolve a network-roster station's fixed ECEF by name at a run date (RTA-12).
     *
     * Composes @c StationInfoFile::find (9-char ID / DOMES / 4-char prefix,
     * case-insensitive) with @ref resolveStationCoordinate, periodic and
     * post-seismic terms enabled, which evaluates the full SIF coordinate function
     * @f$ x(t) = x_{ref} + v\,(t - t_{ref}) + \sum \text{periodic}(t) + \sum \text{PSD}(t) @f$
     * at @p decimal_year.  The config loader calls it when
     * @c network.use_sif_coordinates fills roster stations whose @c position_ecef
     * is absent from the JSON; it is a separate function so the resolution can be
     * unit-tested without the loader.
     *
     * @param registry     Loaded SIF registry.
     * @param station_name Station identifier (marker name, ID/prefix, or DOMES).
     * @param decimal_year Run date to propagate the occupation coordinate to.
     * @return ECEF + propagation epoch, or @c std::nullopt when the station is
     *         unknown to the registry or has no usable occupation.
     */
    std::optional<SifRosterCoordinate> resolveRosterCoordinate(const StationInfoFile& registry,
                                                               const std::string& station_name,
                                                               double decimal_year);

} // namespace GnssProcessor
