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
 * "CIT100USA", a RINEX marker "CIT1" and a DOMES "40400S201" all resolve to
 * the same record.
 *
 * An exact ID or DOMES match is authoritative.  A 4-char prefix match is only
 * a CANDIDATE, because a 9-char ID absent from the registry can share its
 * first four characters with an unrelated station: GAMB00PYF (Gambier, French
 * Polynesia) is in neither the IGS nor the NCN SIF, while GAMB00USA, 7,688 km
 * away, is.  Legitimate renames also exist (STHL00GBR is now STHL00SHN, same
 * monument).  The candidate is therefore accepted only when it is confirmed:
 * by an approximate position within @c SIF_MAX_PREFIX_DISTANCE_M of the
 * candidate when the caller supplies one, or, without a position, only for a
 * bare 4-char key (which carries no conflicting 9-char suffix).  Every
 * rejection is logged at warning level on the @c station_info logger.
 *
 * Known limit of the no-position rule: a bare 4-char key is trusted as is, and
 * RINEX headers often carry one.  The GAMB00PYF and MELI00ESP files carry the
 * markers "GAMB" and "MELI"; neither station is in the SIF, so without a usable
 * position those markers resolve to GAMB00USA (7,688 km away) and MELI00USA
 * (5,356 km away).  With the RINEX APPROX position supplied, as on the PPP
 * a-priori, OTL and OPT paths, both candidates are rejected.
 *
 * ## Extrapolation
 *
 * @c resolveStationCoordinate keeps the established occupation selection
 * (covering window, else nearest midpoint) but reports when no window covers
 * the epoch: how far past the selected segment the coordinate is
 * extrapolated, and whether the antenna or receiver changed in that gap.  A
 * closed segment does not by itself mean the equipment changed (in the
 * bundled igs20 SIF, 32 of the 266 closed last segments are followed by an
 * antenna or receiver change, 6 of them within two days of the END), but an
 * equipment change after a closed segment invalidates the extrapolated
 * coordinate (for example NIUM00NIU: last segment ends 2024-01-07, antenna
 * model change on 2024-02-27), so the flags report it.
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
     * @brief Largest distance [m] between a caller's approximate position and a
     *        4-char prefix candidate at which @c StationInfoFile::find accepts it.
     *
     * 10 km is generous against a RINEX APPROX header (metres to a few hundred
     * metres off) and a marker-to-ARP offset, yet far below the separation of
     * any two distinct stations that share a 4-char code across networks
     * (GAMB00PYF vs GAMB00USA: 7,688 km).
     */
    static constexpr double SIF_MAX_PREFIX_DISTANCE_M = 10000.0;

    /**
     * @brief Tolerance [yr] (2 days) by which the extrapolation gap of
     *        @c resolveStationCoordinate reaches back inside the selected segment.
     *
     * Equipment changes are logged to the minute while occupation windows are
     * cut at day boundaries, so a change that closed the segment can start a few
     * minutes to hours BEFORE the segment's END (CHUR00CAN: receiver type change
     * at 2024-02-13T23:43, segment END 2024-02-13T23:59:30).
     */
    static constexpr double SIF_GAP_CHANGE_TOLERANCE_YR = 2.0 / 365.25;

    /**
     * @brief A station coordinate resolved to a specific observation epoch.
     *
     * The position is the full SIF coordinate function evaluated at the query
     * epoch: the reference coordinate of the selected OCCUPATION record,
     * propagated by the station velocity from that record's own reference
     * epoch, plus the optional periodic (seasonal) and post-seismic terms.  The
     * covariance is the position (3x3) block propagated from the occupation's
     * 6x6 coordinate/velocity covariance to the query epoch.  Everything is in
     * the SIF reference frame (typically ITRF2020) and SI units (m, m^2).
     *
     * The extrapolation fields describe how the epoch relates to the selected
     * occupation window and never change the coordinate.  When a non-flagged
     * window covers the epoch they are all false / zero (the segment bounds are
     * still filled).  Otherwise the GAP is the interval between the selected
     * segment and the epoch, reaching @c SIF_GAP_CHANGE_TOLERANCE_YR inside the
     * segment: @f$ [t_{end} - \text{tol},\ t] @f$ after the segment,
     * @f$ [t,\ t_{start} + \text{tol}] @f$ before it.
     */
    struct ResolvedStationCoord {
        Eigen::Vector3d ecef{ Eigen::Vector3d::Zero() };       ///< ECEF position of the ground reference point (monument) at the query epoch [m]; see @c reduceSifCoordinateToArp.
        Eigen::Matrix3d covariance{ Eigen::Matrix3d::Zero() }; ///< Propagated position covariance [m^2].
        bool has_covariance{ false };                          ///< True if @c covariance is meaningful.
        bool valid{ false };                                   ///< True if a usable occupation was found.
        bool extrapolated{ false };          ///< True iff no non-flagged occupation window covers the query epoch.
        double extrapolation_years{ 0.0 };   ///< Signed reach outside the selected window [yr]: t - end (t >= end), t - start (< 0, t < start), 0 when covered.
        double segment_start_year{ 0.0 };    ///< Selected occupation's window start [decimal year].
        double segment_end_year{ 0.0 };      ///< Selected occupation's window end [decimal year] (9999.0 = open-ended).
        bool antenna_change_in_gap{ false }; ///< An antenna window starting in the gap differs from its predecessor in TYPE (model + radome) or serial number.
        bool receiver_change_in_gap{ false };///< A receiver window starting in the gap differs from its predecessor in TYPE (firmware-only and serial-only updates do not count).
    };

    /**
     * @brief Evaluate a station's SIF coordinate function at a given epoch.
     *
     * Selects the non-flagged OCCUPATION record whose window covers
     * @p decimal_year, or, when none covers it, the non-flagged record whose
     * window midpoint is nearest (an open END is capped at 9000 for the
     * midpoint).  Then forms
     * @f$ x(t) = x_{ref} + v\,(t - t_{ref}) + \sum \text{periodic}(t) + \sum \text{PSD}(t) @f$
     * following the NGS / M-PAGES convention, with @f$ t_{ref} @f$ the selected
     * record's own @c SifOccupation::reference_epoch_year; the covariance is
     * propagated over the same @f$ t - t_{ref} @f$.  The periodic phase
     * reference and the post-seismic relaxation models match the SIF generator.
     *
     * When no window covers the epoch the nearest record is extrapolated as
     * before, and the result says so (@c ResolvedStationCoord::extrapolated,
     * @c extrapolation_years, and the antenna / receiver change flags over the
     * gap).  Callers that consume the coordinate log a warning in that case.
     *
     * Equipment-change rule over the gap: the antenna (respectively receiver)
     * windows are ordered by start; a window other than the first whose start
     * lies inside the gap (bounds inclusive) is a change when it differs from
     * the preceding window in the whitespace-trimmed TYPE string, model and
     * radome together, or for antennas in the trimmed serial number.  A re-log
     * of the same equipment (same TYPE and serial; for receivers, a firmware or
     * serial update) is not a change.
     *
     * @param s              Station record.
     * @param decimal_year   Observation epoch as a decimal year.
     * @param with_periodic  Include the periodic (seasonal) terms, typically annual
     *                       and semiannual.
     * @param with_psd       Include post-seismic deformation terms.
     * @return Resolved coordinate with its extrapolation diagnostics; @c valid is
     *         false if @p s has no usable occupation, in which case the caller
     *         falls back to its own a-priori.
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
         * @brief Resolve a station record by identifier, confirming a prefix match.
         *
         * Tries, in order: full 9-char ID, DOMES number, 4-char station
         * prefix (the first four characters of the key).  Matching is
         * case-insensitive; surrounding whitespace is trimmed.
         *
         * An exact ID or DOMES match is returned whatever @p approx_ecef holds.
         * A prefix match is a candidate, accepted only if:
         *  - @p approx_ecef is usable (non-null, all finite, norm > 1e6 m): the
         *    3D distance from it to the candidate's reference position is at
         *    most @p max_prefix_distance_m.  The reference position is the
         *    candidate's @c approximate_coordinates when their norm exceeds
         *    1e6 m, else the first non-DO_NOT_USE occupation's coordinates,
         *    else the first occupation's (each subject to the same norm test);
         *    a candidate with none of these is rejected.
         *  - no usable position: the trimmed key is exactly 4 characters long
         *    (a bare code such as "cit1" or "P389").  A longer key that is not an
         *    exact ID or DOMES (for example "GAMB00PYF") is rejected.  A bare
         *    code is trusted as is, so a bare RINEX marker of a station absent
         *    from the SIF still resolves here ("GAMB" to GAMB00USA); only a
         *    usable @p approx_ecef catches that case.
         *
         * Each rejected candidate is logged at warning level on the
         * @c station_info logger with the query key, the candidate ID and, when
         * known, the distance.  The one-argument form therefore still resolves
         * exact IDs, DOMES numbers and bare 4-char codes.
         *
         * @param station_key           Marker name, station ID/prefix, or DOMES number.
         * @param approx_ecef           Optional approximate ECEF position of the station
         *                              the caller means [m] (for example the RINEX APPROX
         *                              header); @c nullptr when none is known.
         * @param max_prefix_distance_m Acceptance radius for a prefix candidate when
         *                              @p approx_ecef is usable [m].
         * @return Pointer to the matching record, or @c nullptr if not found or a
         *         prefix candidate was rejected.  The pointer stays valid until the
         *         next @c load() call, which may reallocate the store.
         */
        const StationInfo* find(const std::string& station_key,
                                const Eigen::Vector3d* approx_ecef = nullptr,
                                double max_prefix_distance_m = SIF_MAX_PREFIX_DISTANCE_M) const;

        /** @brief Number of stations currently held. */
        std::size_t size() const { return stations_.size(); }

        /** @brief True if no stations have been loaded. */
        bool empty() const { return stations_.empty(); }

    private:
        LogManager& log_manager_;
        std::vector<StationInfo> stations_;                 ///< Owning store; pointers from find() index into this.
        std::unordered_map<std::string, std::size_t> by_id_;    ///< Full 9-char ID (upper-case) -> index.
        std::unordered_map<std::string, std::size_t> by_id4_;   ///< 4-char prefix (upper-case) -> index (first wins); a match here is only a candidate (see find()).
        std::unordered_map<std::string, std::size_t> by_domes_; ///< DOMES number -> index.
    };

    /**
     * @brief A network-roster station coordinate resolved from a SIF (RTA-12).
     *
     * Result carrier for @ref resolveRosterCoordinate: the survey-grade ECEF at
     * the requested run date plus that date, which is the propagation epoch a
     * product contract declares as @c coord_epoch, and the extrapolation
     * diagnostics of the underlying @c ResolvedStationCoord (same meaning).
     */
    struct SifRosterCoordinate {
        Eigen::Vector3d ecef{ Eigen::Vector3d::Zero() };  ///< ECEF at @ref epoch_year [m], SIF reference frame (typically ITRF2020).
        double          epoch_year{ 0.0 };                ///< Decimal year the coordinate was propagated to.
        bool   extrapolated{ false };           ///< True iff no non-flagged occupation window covers @ref epoch_year.
        double extrapolation_years{ 0.0 };      ///< Signed reach outside the selected window [yr] (see @c ResolvedStationCoord).
        double segment_start_year{ 0.0 };       ///< Selected occupation's window start [decimal year].
        double segment_end_year{ 0.0 };         ///< Selected occupation's window end [decimal year] (9999.0 = open-ended).
        bool   antenna_change_in_gap{ false };  ///< Antenna TYPE or serial change inside the extrapolation gap.
        bool   receiver_change_in_gap{ false }; ///< Receiver TYPE change inside the extrapolation gap.
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
     * Such a station has no position to confirm a prefix candidate with, so
     * @c find() runs without one: an exact ID, a DOMES number or a bare 4-char
     * code resolves, while a longer name that only shares its first four
     * characters with a registry ID is rejected (and logged on the
     * @c station_info logger) rather than silently taking that station's
     * coordinate.  A bare 4-char roster name is trusted as is (see @c find()),
     * so a 9-char roster name is the safer key for a station that may be
     * absent from the SIF.  The extrapolation diagnostics are carried into the
     * result for the loader to report.
     *
     * @param registry     Loaded SIF registry.
     * @param station_name Station identifier (marker name, ID/prefix, or DOMES).
     * @param decimal_year Run date to propagate the occupation coordinate to.
     * @return ECEF + propagation epoch + extrapolation diagnostics, or
     *         @c std::nullopt when the station is unknown to the registry, its
     *         prefix candidate was rejected, or it has no usable occupation.
     */
    std::optional<SifRosterCoordinate> resolveRosterCoordinate(const StationInfoFile& registry,
                                                               const std::string& station_name,
                                                               double decimal_year);

    /**
     * @brief Largest norm [m] by which a RINEX `ANTENNA: DELTA H/E/N` record and
     *        the covering SIF antenna window's @c arp_minus_grp_enu may differ
     *        before @ref reduceSifCoordinateToArp's caller reports a disagreement.
     *
     * Both sources print the eccentricity to 0.1 mm.  On the 2024-129 field
     * roster they agree exactly at every SIF-resolved station (GRAZ00AUT
     * 1.964 m up, KRGG00ATF 1.0332 / 0.0034 / 0.0004 m), so 1 mm flags a
     * genuine metadata conflict (a header from another antenna period, a
     * mis-entered survey) rather than rounding.
     */
    static constexpr double SIF_ECCENTRICITY_TOLERANCE_M = 1e-3;

    /**
     * @brief Origin of the marker -> ARP eccentricity in a @ref SifArpReduction.
     */
    enum class SifEccentricitySource {
        RinexHeader,       ///< The RINEX `ANTENNA: DELTA H/E/N` record.
        SifAntennaWindow,  ///< The SIF antenna window covering the epoch (`ARP_MINUS_GRP_ENU`).
        None               ///< Neither was available: the coordinate was not reduced.
    };

    /**
     * @brief A SIF coordinate reduced from the ground reference point to the ARP.
     *
     * Result carrier for @ref reduceSifCoordinateToArp.  The eccentricity fields
     * use the RINEX (H, E, N) = (Up, East, North) order that
     * @c Coordinates::markerToArp takes, whatever the source.
     */
    struct SifArpReduction {
        Eigen::Vector3d arp_ecef{ Eigen::Vector3d::Zero() };  ///< Antenna reference point ECEF [m]; the input coordinate unchanged when @c source is None.
        Eigen::Vector3d delta_hen{ Eigen::Vector3d::Zero() }; ///< Eccentricity applied, (H, E, N) [m]; zero when @c source is None.
        SifEccentricitySource source{ SifEccentricitySource::None }; ///< Where @c delta_hen came from.
        bool has_sif_eccentricity{ false };                   ///< True iff a SIF antenna window covers the epoch.
        Eigen::Vector3d sif_delta_hen{ Eigen::Vector3d::Zero() }; ///< That window's @c arp_minus_grp_enu reordered to (H, E, N) [m]; zero when none covers.
        double eccentricity_disagreement_m{ 0.0 };            ///< |RINEX - SIF| eccentricity norm [m] when both are available, else 0.
    };

    /**
     * @brief Reduce a resolved SIF coordinate from the monument to the antenna
     *        reference point.
     *
     * A SIF OCCUPATION coordinate (and therefore @c ResolvedStationCoord::ecef)
     * refers to the ground reference point (GRP), the monument, like the RINEX
     * `APPROX POSITION XYZ`; the SIF records the antenna's offset from it per
     * antenna window as @c SifAntennaWindow::arp_minus_grp_enu.  Every position
     * state in the engine is the ARP (ATX PCO/PCV are ARP-relative), so a SIF
     * coordinate that seeds or constrains a position state is reduced first;
     * used as given it holds the station one antenna height (plus any horizontal
     * eccentricity) below its antenna.  Measured on DOY 2024-129: GRAZ00AUT's SIF
     * coordinate lies 0.6 mm from its published monument with a 1.964 m antenna
     * height, KRGG00ATF's 1.6 mm with 1.033 m.
     *
     * The eccentricity is the RINEX header's `ANTENNA: DELTA H/E/N` when the
     * header carries the record, which is the convention of the RINEX APPROX seed
     * and of the network roster (@c network.station_marker_to_arp), and otherwise
     * the SIF antenna window covering @p decimal_year.  The covering window is the
     * first whose [start, end) contains the epoch, @c DO_NOT_USE or not (the
     * window the a-priori path's antenna QC reports on); the SIF windows do not
     * overlap.  With neither source the coordinate is returned unchanged and
     * @c source is None, for the caller to report.  The reduction itself is
     * @c Coordinates::markerToArp, an exact no-op at a zero eccentricity.
     *
     * @param s                Station record that @p grp_ecef was resolved from.
     * @param grp_ecef         Resolved SIF coordinate (monument) [m].
     * @param decimal_year     Epoch @p grp_ecef was resolved at, which selects the
     *                         antenna window.
     * @param rinex_delta_hen  RINEX `ANTENNA: DELTA H/E/N` in (H, E, N) order [m],
     *                         or @c nullptr when the header carried no such record
     *                         (a recorded 0/0/0 is a valid, zero eccentricity).
     * @return ARP coordinate with the eccentricity applied, its source, and the
     *         RINEX-vs-SIF eccentricity cross-check.
     */
    SifArpReduction reduceSifCoordinateToArp(const StationInfo& s,
                                             const Eigen::Vector3d& grp_ecef,
                                             double decimal_year,
                                             const Eigen::Vector3d* rinex_delta_hen);

} // namespace GnssProcessor
