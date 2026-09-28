#pragma once

/**
 * @file BrsdTransform.hpp
 * @brief Between-receiver single-difference transform for DD baselines.
 *
 * BRSD = `(observation_rov - observation_ref)` per (satellite, signal).
 * It is the *inner* transform of the DD chain for baseline processing:
 *
 *     PppModel(rov), PppModel(ref) → BRSD → BSSD → KF
 *
 * BRSD removes the terms common to both receivers (satellite clock,
 * satellite phase bias, the common-mode atmospheric residual on short
 * baselines, satellite antenna PCO/PCV).  The per-satellite residual
 * that remains holds the rover position error, the rover-minus-reference
 * receiver clock offset, the float ambiguity term, and the small
 * residual atmosphere on longer baselines.  The downstream BSSD step
 * cancels the receiver clock offset and produces DD measurements.
 *
 * The output carries the rover-side LOS (not @c los_rov - @c los_ref)
 * because in DD baseline mode only the rover position is estimated;
 * the reference station LOS does not multiply any state variable, and
 * would cancel pairwise in the BSSD layer's geometry partial anyway.
 *
 * Stateless and pure: each @c apply() call is independent of every
 * other.  There is no caching and no member variable; the class is a
 * single static method.  Variance propagation assumes per-receiver
 * observation noise is independent.  This holds for thermal noise;
 * multipath correlation would violate it and is not modeled.
 */

#include "common/PppTypes.hpp"
#include "common/SignalBand.hpp"
#include <Eigen/Dense>
#include <optional>
#include <string>

namespace GnssProcessor {

    /**
     * @brief Output of a single between-receiver SD operation.
     *
     * Holds the differenced residual and variance, the metadata that
     * downstream BSSD pivot differencing reads, and the rover-side LOS
     * used to form the geometry partial with respect to the rover
     * position state.
     */
    struct BrsdObservation {
        std::string satellite;                            ///< Satellite ID (e.g. "G09").
        std::string constellation;                        ///< Constellation name (e.g. "GPS").
        FrequencyBand band{ FrequencyBand::Unknown };     ///< Physical band identity.
        char obs_type{ 0 };                               ///< 'C' (code) or 'L' (phase).
        std::string rov_code;                             ///< Rover RINEX-3 code.
        std::string ref_code;                             ///< Reference RINEX-3 code.
        bool brsd_aliased{ false };                       ///< @c rov_code != @c ref_code (cross-attribute pair).

        double residual{ 0.0 };                           ///< (obs_rov - mod_rov) - (obs_ref - mod_ref) [m].
        double variance{ 0.0 };                           ///< @c var_rov + @c var_ref [m²].
        double var_rov{ 0.0 };                            ///< Rover-side variance, retained for diagnostics.
        double var_ref{ 0.0 };                            ///< Reference-side variance, retained for diagnostics.

        Eigen::Vector3d los_rov{ Eigen::Vector3d::Zero() };///< Rover-side LOS unit vector.
        double elevation_rov{ 0.0 };                      ///< Rover-side elevation (degrees).

        GnssTime time{};
    };

    /**
     * @brief Stateless between-receiver SD transform.
     *
     * One static @c apply() call per (rov, ref, signal) tuple.  The
     * class has no instance state and no configuration, since the BRSD
     * layer is a pure operation.  The caller owns the aliasing policy
     * and the band/code selection; this class performs the arithmetic.
     */
    class BrsdTransform {
    public:
        /**
         * @brief Compute BRSD for one (rover, reference, signal) tuple.
         *
         * Looks up @c rov_code in @p rov.observed and @p rov.modeled,
         * @c ref_code in @p ref.observed and @p ref.modeled, then
         * returns the differenced residual and combined variance.
         *
         * @param rov            Rover-side modeled observation.
         * @param ref            Reference-side modeled observation (same satellite).
         * @param constellation  Constellation name string (e.g. "GPS").
         * @param rov_code       Rover RINEX-3 code, must be present in
         *                       @p rov.observed and @p rov.modeled.
         * @param ref_code       Reference RINEX-3 code; may differ from
         *                       @p rov_code in cross-attribute aliasing
         *                       scenarios.
         * @param band           Physical band identity.
         * @param obs_type       'C' (code) or 'L' (phase).
         * @return BrsdObservation with the BRSD residual / variance /
         *         metadata when all four (observed, modeled) lookups
         *         succeed and produce a finite residual; @c std::nullopt
         *         otherwise (missing data or NaN).
         */
        static std::optional<BrsdObservation> apply(
            const PppObservation& rov,
            const PppObservation& ref,
            const std::string& constellation,
            const std::string& rov_code,
            const std::string& ref_code,
            FrequencyBand band,
            char obs_type);
    };

} // namespace GnssProcessor
