#pragma once

/**
 * @file BssdTransform.hpp
 * @brief Between-Satellite Single-Difference (BSSD) S-transformation.
 *
 * BssdTransform centralizes the BSSD math used by both PPP-RTK and the
 * baseline DD pipeline.  Each path consumes BSSD differently:
 *
 *  - PPP-RTK (single receiver, SD-enabled mode) calls @c apply() to
 *    transform a fully-built @c EpochMeasurement in place.  The S-
 *    transformation rewrites the measurement vector @c z, the design
 *    matrix @c H, and the noise covariance @c R from undifferenced
 *    form to per-(constellation, signal) single differences against a
 *    pivot satellite.  Receiver-shared columns (clock, ISB, code
 *    biases) and the pivot's ambiguity column are zeroed.
 *
 *  - Baseline DD (rover + reference) feeds @c BrsdObservation entries
 *    into @c differenceBrsd().  BRSD has already cancelled receiver-
 *    shared bias terms in residual space; @c differenceBrsd applies
 *    the BSSD operator (non-pivot minus pivot) per-(cons, band, obs_type)
 *    to produce DD rows ready for the @c MeasurementModel row builder.
 *    The two-step composition BRSD → BSSD is mathematically equivalent
 *    to the single-step DD = (rov_i - rov_p) - (ref_i - ref_p) but the
 *    intermediate BRSD layer is inspectable as a separate diagnostic.
 *
 * Renamed from @c DifferencingTransform (the old name was generic — it
 * didn't say between-satellite vs between-receiver).  PPP-RTK and
 * baseline now consume BSSD through this single class.
 */

#include "config/GnssConfig.hpp"
#include "filter/StateManager.hpp"
#include "filter/PivotManager.hpp"
#include "logging/LogManager.hpp"
#include "common/PppTypes.hpp"
#include "common/SignalBand.hpp"
#include "measurement/BrsdTransform.hpp"
#include <Eigen/Dense>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

    /**
     * @brief Enumeration of differencing operator modes.
     *
     * Selects which kind of linear transform is applied to the raw
     * observation vector, design matrix H, and noise covariance R.
     *
     * - @c None: no differencing; raw (undifferenced) observables pass through.
     *   Used by PPP and by PPP-RTK when @c enable_between_satellite_sd is false.
     * - @c BetweenSatellite: per-constellation single difference against a
     *   pivot satellite within one receiver.  Cancels receiver-shared
     *   states (receiver clock, ISB, receiver DCB, ...).  Used by PPP-RTK
     *   when the SD transform is enabled.  Realised by @c BssdTransform::apply().
     * - @c DoubleDifference: between-receiver AND between-satellite
     *   differencing.  Used by baseline modes; realised by composing
     *   @c BrsdTransform::apply (per sat) with @c BssdTransform::differenceBrsd
     *   (per cons / band / obs_type).
     */
    enum class DifferencingMode {
        None,              ///< Raw observables (PPP / PPP-RTK without SD).
        BetweenSatellite,  ///< Per-constellation single difference vs. pivot (PPP-RTK SD).
        DoubleDifference   ///< Between-receiver + between-satellite (baseline processing).
    };

    /**
     * @brief Output of a BSSD step applied to a BRSD vector.
     *
     * Represents one DD row: the BSSD-differenced residual / variance
     * pair against the constellation's pivot, plus the metadata the
     * downstream @c MeasurementModel row builder needs to seed
     * ambiguity states, build the H_pos partial, and emit trace entries.
     *
     * Geometry partials w.r.t. the rover position state are recoverable
     * from @c los_rov_i and @c los_rov_p as @c h_pos = -(los_rov_i - los_rov_p).
     * The fields are deliberately kept narrow — MeasurementModel can
     * layer on its own bookkeeping fields (group_key, amb_idx, etc.)
     * around the BssdDdRow without growing this struct.
     */
    struct BssdDdRow {
        char obs_type{ 'C' };                                ///< 'C' (code) or 'L' (phase).
        std::string sat_i;                                   ///< Non-pivot satellite ID.
        std::string sat_p;                                   ///< Pivot satellite ID.
        std::string cons;                                    ///< Constellation name.
        std::string rov_code;                                ///< Rover RINEX-3 code.
        std::string ref_code;                                ///< Reference RINEX-3 code.
        FrequencyBand band{ FrequencyBand::Unknown };        ///< Physical band identity.

        bool integer_capable{ true };                        ///< All four DD observations use the same RINEX code.

        double innov_pre{ 0.0 };                             ///< BRSD_i.residual - BRSD_p.residual [m].
        double var_brsd_i{ 0.0 };                            ///< BRSD variance on the non-pivot side [m²].
        double var_brsd_p{ 0.0 };                            ///< BRSD variance on the pivot side [m²].

        Eigen::Vector3d los_rov_i{ Eigen::Vector3d::Zero() };///< Rover-side LOS to non-pivot.
        Eigen::Vector3d los_rov_p{ Eigen::Vector3d::Zero() };///< Rover-side LOS to pivot.
    };

    /**
     * @class BssdTransform
     * @brief Between-Satellite Single-Difference S-transformation.
     *
     * One class, two methods:
     *
     *  - @c apply() is the in-place S-transform on an @c EpochMeasurement,
     *    used by PPP-RTK with BSSD enabled.  Constructs the differencing
     *    operator D from the current pivot map (provided by @c PivotManager
     *    in uncombined mode, or chosen per signal in combined IF mode)
     *    and applies @c (D z, D H, D R Dᵀ) plus the column-zeroing of
     *    receiver-shared states.
     *
     *  - @c differenceBrsd() is the static helper that consumes BRSD
     *    outputs (from @c BrsdTransform::apply) plus a pivot map, and
     *    emits a vector of @c BssdDdRow.  No state required — pure
     *    function of its inputs.  The baseline DD path runs this for
     *    each epoch's BRSD set.
     *
     * Both methods are conceptually the same operation (subtract the
     * pivot's value from each non-pivot in the same group); they differ
     * in what they're subtracting from and what bookkeeping the result
     * needs.
     */
    class BssdTransform {
    public:
        BssdTransform(const GnssConfig& config,
                      const StateManager& state_manager,
                      const PivotManager& pivot_manager,
                      LogManager& log_manager);

        /**
         * @brief Apply between-satellite single-difference transformation in place.
         *
         * Combined (IF) mode:
         *   Pivot: one per (constellation, signal_type), highest elevation.
         *   Columns zeroed: receiver clock, phase clock, ISB.
         *
         * Uncombined PPP-RTK mode:
         *   Pivot: reads the sticky pivot from PivotManager.
         *   Columns zeroed: receiver clock, phase clock, ISB, receiver DCB/IFB,
         *   reference satellite ionosphere.
         *
         * In both modes reference ambiguity columns are zeroed and the
         * differencing operator D is applied to z, H, and R.
         *
         * @param update         EpochMeasurement to transform (modified in place).
         * @param epoch_prefix   Log prefix string.
         */
        void apply(EpochMeasurement& update, const std::string& epoch_prefix);

        /**
         * @brief Apply BSSD to a vector of BRSD entries to produce DD rows.
         *
         * Iterates the @p brsd_by_cons_sat structure, finds each
         * constellation's pivot in @p pivots, and for every non-pivot
         * satellite in that constellation emits one @c BssdDdRow per
         * (band, obs_type) pair where the pivot also has a BRSD entry.
         * The DD residual is the BSSD difference @c BRSD_i - @c BRSD_p
         * in residual space; the DD variance components are simply the
         * two BRSD variances (BSSD's pivot column dropping carries no
         * additional noise).  Cross-attribute aliasing flags are
         * propagated into @c integer_capable so downstream AR knows
         * which rows can round to integer.
         *
         * Stateless / pure: the static method depends only on its
         * inputs.  Pivot selection happens upstream (@c MeasurementModel
         * owns the sticky pivot logic for baseline mode).
         *
         * @param brsd_by_cons_sat  BRSD entries indexed by constellation
         *                          name then satellite ID.
         * @param pivots            Pivot satellite per constellation.
         * @param exclude_keys      Row signatures to skip
         *                          (format @c "<sat_i>_<signal_token>",
         *                          where signal_token is the rover code
         *                          or @c "<rov>/<ref>" for aliased rows).
         *                          Used by the per-epoch rebuild loop to
         *                          drop rows the KF outlier handler flagged.
         * @return Vector of @c BssdDdRow ready for downstream row-builder
         *         bookkeeping (group_key, amb_idx, h_pos derivation).
         */
        static std::vector<BssdDdRow> differenceBrsd(
            const std::map<std::string, std::map<std::string, std::vector<BrsdObservation>>>& brsd_by_cons_sat,
            const std::map<std::string, std::string>& pivots,
            const std::set<std::string>& exclude_keys = {});

    private:
        const GnssConfig& config_;
        const StateManager& state_manager_;
        const PivotManager& pivot_manager_;
        LogManager& log_manager_;
    };

} // namespace GnssProcessor
