#pragma once

/**
 * @file StationInfoFile.hpp
 * @brief Loader and registry for NGS Station Information Files (SIFs).
 *
 * A SIF is a JSON bundle of per-station GNSS metadata (see @c StationInfo for
 * the data model and provenance).  @c StationInfoFile parses one or more SIF
 * JSON files once at startup, normalises units into SI, and exposes a
 * station-name lookup so that the ocean-loading models, the a-priori
 * coordinate path, and the PPP-RTK network datum can all share a single
 * authoritative source of station metadata.
 *
 * ## Indexing and lookup
 *
 * Stations are indexed three ways for robust joining against whatever
 * identifier a caller has on hand (RINEX marker name, configured station
 * name, baseline-fixture code):
 *
 *  - by full 9-char ID (@c SSSS00CCC), upper-cased;
 *  - by 4-char station prefix (the @c SSSS of the ID), upper-cased;
 *  - by DOMES number.
 *
 * @c find() tries them in that order, case-insensitively.  This lets a
 * configured "CIT100USA", a RINEX marker "CIT1", or a DOMES "40451S006"
 * all resolve to the same record.
 *
 * ## Memory
 *
 * The full SIF DOM (tens of MB for the NCN file) is parsed transiently and
 * discarded; only the compact @c StationInfo structs are retained.  Multiple
 * @c load() calls accumulate (e.g. the IGS and NCN SIFs together).
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
     * epoch: the OCCUPATION reference coordinate propagated by the station
     * velocity, plus the optional periodic (seasonal) and post-seismic terms.
     * The covariance is the position (3x3) block propagated from the OCCUPATION
     * 6x6 coordinate/velocity covariance to the query epoch.  All in the SIF
     * reference frame (typically ITRF2020) and SI units (m, m^2).
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
     * Selects the OCCUPATION window covering @p decimal_year (falling back to
     * the nearest non-flagged solution), then forms
     * @f$ x(t) = x_{ref} + v\,(t - t_{ref}) + \sum \text{periodic}(t) + \sum \text{PSD}(t) @f$
     * following the NGS / M-PAGES convention.  The periodic phase reference and
     * the post-seismic relaxation models match the SIF generator.
     *
     * @param s              Station record.
     * @param decimal_year   Observation epoch as a decimal year.
     * @param with_periodic  Include annual/semiannual seasonal terms.
     * @param with_psd        Include post-seismic deformation terms.
     * @return Resolved coordinate; @c valid is false if @p s has no usable
     *         occupation (caller should fall back to its own a-priori).
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
         * amplitudes, mm/y->m/y for velocities, mm^2->m^2 for covariances).
         * On a parse error the file is skipped with a logged warning.
         *
         * @param sif_json Path to a SIF JSON file.
         * @return Number of stations successfully ingested (0 on failure).
         */
        std::size_t load(const std::filesystem::path& sif_json);

        /**
         * @brief Resolve a station record by identifier.
         *
         * Tries, in order: full 9-char ID, DOMES number, 4-char station
         * prefix.  Matching is case-insensitive; surrounding whitespace is
         * trimmed.
         *
         * @param station_key Marker name, station ID/prefix, or DOMES number.
         * @return Pointer to the matching record, or @c nullptr if not found.
         *         The pointer is stable for the lifetime of this registry.
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
     * @brief RTA-12: a network-roster station coordinate resolved from a SIF.
     *
     * Thin result carrier for @ref resolveRosterCoordinate: the survey-grade ECEF
     * at the requested run date plus that date itself (the propagation epoch a
     * product contract should declare as @c coord_epoch).
     */
    struct SifRosterCoordinate {
        Eigen::Vector3d ecef{ Eigen::Vector3d::Zero() };  ///< ECEF at @ref epoch_year [m], SIF reference frame (typically ITRF2020).
        double          epoch_year{ 0.0 };                ///< Decimal year the coordinate was propagated to.
    };

    /**
     * @brief RTA-12: resolve a network-roster station's fixed ECEF by name at a run date.
     *
     * Composition of @c StationInfoFile::find (9-char ID / 4-char prefix / DOMES,
     * case-insensitive) and @ref resolveStationCoordinate with periodic and
     * post-seismic terms enabled — the full SIF coordinate function
     * @f$ x(t) = x_{ref} + v\,(t - t_{ref}) + \sum \text{periodic}(t) + \sum \text{PSD}(t) @f$
     * evaluated at @p decimal_year.  Used by the config loader when
     * @c network.use_sif_coordinates fills roster stations whose @c position_ecef
     * was not typed in the JSON; factored out so the resolution is unit-testable
     * in isolation from the loader.
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
