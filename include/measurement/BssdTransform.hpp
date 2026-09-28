#pragma once

/**
 * @file BssdTransform.hpp
 * @brief Between-Satellite Single-Difference (BSSD) S-transformation.
 *
 * BssdTransform holds the BSSD math used by both PPP-RTK and the baseline DD
 * pipeline.  The two paths consume it differently:
 *
 *  - PPP-RTK (single receiver, SD-enabled mode) calls @c apply() to
 *    transform a fully built @c EpochMeasurement in place.  The S-
 *    transformation rewrites the measurement vector @c z, the design
 *    matrix @c H, and the noise covariance @c R from undifferenced
 *    form to per-(constellation, signal) single differences against a
 *    pivot satellite.  Receiver-shared columns (clock, ISB, code
 *    biases) and the pivot's ambiguity column are zeroed.
 *
 *  - Baseline DD (rover + reference) feeds @c BrsdObservation entries
 *    into @c differenceBrsd().  BRSD has already cancelled receiver-
 *    shared bias terms in residual space; @c differenceBrsd applies
 *    the BSSD operator (non-pivot minus pivot) per (cons, band, obs_type)
 *    to produce DD rows ready for the @c MeasurementModel row builder.
 *    The two-step composition BRSD → BSSD is mathematically equivalent
 *    to the single-step DD = (rov_i - rov_p) - (ref_i - ref_p), and the
 *    intermediate BRSD layer remains inspectable as a separate diagnostic.
 *
 * Formerly named @c DifferencingTransform; the current name states that the
 * difference is between satellites rather than between receivers.
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
     *   Used by PPP, by PPP-RTK when @c enable_between_satellite_sd is false,
     *   and always by the PPP-RTK network producer.
     * - @c BetweenSatellite: per-constellation single difference against a
     *   pivot satellite within one receiver.  Cancels receiver-shared
     *   states (receiver clock, ISB, receiver DCB, ...).  Used by PPP-RTK
     *   when the SD transform is enabled.  Realised by @c BssdTransform::apply().
     * - @c DoubleDifference: between-receiver and between-satellite
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
     * The struct carries only these fields; MeasurementModel keeps its own
     * bookkeeping (group_key, amb_idx, etc.) in its own row type around the
     * BssdDdRow.
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
     *    used by PPP-RTK with BSSD enabled.  It builds the differencing
     *    operator D from the per-constellation pivot map held by
     *    @c PivotManager (in both combined IF and uncombined mode), applies
     *    @c (D z, D H, D R Dᵀ), and zeroes the columns of receiver-shared
     *    states.
     *
     *  - @c differenceBrsd() is the static helper that consumes BRSD
     *    outputs (from @c BrsdTransform::apply) plus a pivot map, and
     *    emits a vector of @c BssdDdRow.  It holds no state and is a pure
     *    function of its inputs.  The baseline DD path runs it on each
     *    epoch's BRSD set.
     *
     * Both methods perform the same operation (subtract the pivot's value
     * from each non-pivot in the same group); they differ in what they
     * subtract from and in the bookkeeping the result needs.
     */
    class BssdTransform {
    public:
        BssdTransform(const GnssConfig& config,
                      const StateManager& state_manager,
                      const PivotManager& pivot_manager,
                      LogManager& log_manager);

        /**
         * @brief Applies the between-satellite single-difference transformation in place.
         *
         * Pivot: in both combined (IF) and uncombined mode, the sticky
         * per-constellation pivot satellite from PivotManager.  Rows are grouped
         * by (constellation, full signal type); in each group the pivot
         * satellite's row is the reference.  A row with no pivot in its group,
         * or in a group of one, is dropped rather than passed through
         * undifferenced.  If no SD pair forms, the update is emptied.  An update
         * with fewer than two rows is returned unchanged.
         *
         * Columns zeroed in both modes: receiver clock, phase clock, ISB, clock
         * rate and ISB rate (when allocated), and receiver code-bias (DCB/IFB)
         * states except the per-FCN-slot GLONASS IFB states, which do not cancel
         * between satellites on different slots.  Uncombined mode also zeroes the
         * pivot satellites' ionosphere and satellite-bias columns.
         *
         * In both modes the pivot's ambiguity columns are zeroed (under the NlWl
         * basis, its NL column and, for a non-reference band, the WL column), and
         * the differencing operator D is applied to z, H, and R.  R is formed from
         * the diagonal @c R_v: each SD row's variance is the sum of its two input
         * variances, and rows sharing a pivot are correlated by the pivot's
         * variance.  Output satellite IDs read "<sat>-<pivot>"; output signal
         * types are the non-pivot row's.
         *
         * @param update         EpochMeasurement to transform (modified in place).
         * @param epoch_prefix   Log prefix string.
         */
        void apply(EpochMeasurement& update, const std::string& epoch_prefix);

        /**
         * @brief Applies BSSD to a vector of BRSD entries to produce DD rows.
         *
         * For each constellation in @p pivots, finds that constellation's
         * entries in @p brsd_by_cons_sat, and for every non-pivot satellite
         * emits one @c BssdDdRow per code or phase (band, obs_type) pair on
         * which the pivot also has a BRSD entry.  A constellation absent from
         * @p pivots, or whose pivot has no BRSD entries, yields no rows.
         * The DD residual is the BSSD difference @c BRSD_i - @c BRSD_p
         * in residual space; the DD variance components are the two BRSD
         * variances (removing the pivot column adds no noise).
         * Cross-attribute aliasing flags are propagated into
         * @c integer_capable so that downstream AR knows which rows can round
         * to an integer.
         *
         * Stateless: the static method depends only on its inputs.  Pivot
         * selection happens upstream (@c MeasurementModel owns the sticky
         * pivot logic for baseline mode, through its DdPivotManager).
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
