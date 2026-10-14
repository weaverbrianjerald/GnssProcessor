#pragma once

/**
 * @file KalmanPostFitQc.hpp
 * @brief @internal The working value the stages of KalmanFilter's post-fit outlier screen
 *        share.
 *
 * KalmanFilter::validatePostFitResiduals (src/core/filter/KalmanPostFitQc.cpp) is an ordered
 * driver over named private stages (card UOM step U2c):
 *
 *   S0   beginPostFitQc_            the pass's thresholds, row references and counts
 *   E1                              no rows: {false, false}
 *   S1   reportPostFitResiduals_    per-row table, worst row, the "Residual summary" line
 *   S2   reportStatisticalQc_       the variance-factor and data-snooping lines
 *   S3   detectPivotOffsets_        coherent pivot offsets, phase and code: the rows of
 *                                   kalman::kPivotDetectors (below) over the grouped rows
 *   S3e  detectNonPivotSlips_       one non-pivot satellite slipping on several bands
 *   E2   finishGroupRejection_      a group was rejected: rebuild; counted as a bad
 *                                   residual; returns before the backstop
 *   S4   queueHuberInflations_      sub-cycle phase rows get an R target
 *   E3   finishHuberRebuild_        R targets queued: rebuild; not counted; returns before
 *                                   the backstop
 *   S5   selectGateRow_             the sigma threshold, or the Baarda w-test when armed
 *   S6   dispatchWorstRow_          reset (phase) or exclude (code) the gated row, with
 *        chooseOutlierRow_          the MW tiebreak and
 *        respondToPhaseOutlier_     the phase response (floor, reset, correlated rows)
 *   S7   completeResetBackstop_     too many outliers this epoch: complete reset
 *   S8   logPostFitResult_          the pass's verdict line
 *   S9   recordCleanCommit_         residual history, the stuck-code-bias scan
 *        queueStuckCodeBiasResets_  and the history digest, on a clean pass only
 *   E4                              {outlier found, complete reset}
 *
 * The stages read this value through an alias block that binds the names the code had as
 * locals of the former single function (`auto& measurement = w.measurement;`), so their
 * bodies read unchanged.  A stage hands a result to later stages by writing its member
 * (`w.max_idx = max_idx;`).
 *
 * The four coherent-offset pivot detectors of S3 are declared here as one table,
 * kalman::kPivotDetectors, with the pure test they share (kalman::testCoherentOffset,
 * unit-tested in tests/test_kalman_pivot_detectors.cpp).
 *
 * Included by src/core/filter/KalmanPostFitQc.cpp and that test only.
 */

#include "filter/KalmanFilter.hpp"
#include "filter/KalmanQcCounters.hpp"
#include <spdlog/spdlog.h>
#include <Eigen/Dense>
#include <cstddef>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

namespace kalman {

    /**
     * @brief One coherent-offset pivot detector of the post-fit screen: a row of
     *        @ref kPivotDetectors.
     *
     * Under BSSD a disturbance of a pivot offsets every single-difference row that uses
     * it by the same amount.  A detector groups the non-pivot rows of one observable
     * against each pivot and tests each group for that signature (@ref
     * testCoherentOffset):
     *   1. at least @c min_rows rows (statistical power), and for a pool at least
     *      @c min_bands distinct bands;
     *   2. at least @c above_fraction_min of them over the per-row threshold (a
     *      fraction, so the trigger scales with visibility);
     *   3. |median| at least @c median_floor_m, so numerical noise cannot fire it;
     *   4. MAD/|median| at most @c mad_ratio_max: the median ignores one or two rows
     *      that also slipped and the MAD measures the spread without being inflated by
     *      them, so tight means a coherent pivot offset and loose means independent
     *      outliers.
     * The four rows are one algorithm with four parameter sets; the parts that are not
     * parameters are the named options below and the report line, which the
     * observable and the grouping choose.
     */
    struct PivotDetectorSpec {
        const char* label;               ///< Row name, as the documentation calls it.
        char obs_type;                   ///< First character of the rows' signal: 'L' phase,
                                         ///< 'C' code.
        bool pooled;                     ///< false: one group per (pivot, full signal);
                                         ///< true: one pool per pivot across bands, and the
                                         ///< row skips a constellation whose handover an
                                         ///< earlier row already requested.
        std::size_t min_rows;            ///< Fewest rows in a group.
        std::size_t min_bands;           ///< Fewest distinct bands in a pool (pooled only).
        double above_fraction_min;       ///< Least fraction of rows over the threshold.
        double median_floor_m;           ///< Least |median residual| (m).
        double mad_ratio_max;            ///< Largest MAD/|median|.
        bool wavelength_floor;           ///< Apply the wavelength-fraction floor
                                         ///< (estimation.pivot_slip_min_cycles) to the median.
        double tight_cluster_mad_ratio;  ///< At or below this MAD/|median| the wavelength
                                         ///< floor is bypassed (a tight cluster is a pivot
                                         ///< disturbance at any size).
        bool reinflate_iono;             ///< Record the firing group for the post-handover
                                         ///< ionosphere re-inflation.
        std::size_t KalmanQcCounters::* counter;  ///< The run-end counter a firing group
                                                  ///< increments.
    };

    /**
     * @brief The four coherent-offset pivot detectors, in the order they run.
     *
     *   - phase per signal: groups by (pivot, full signal), so "L1W" and "L1C" stay
     *     apart, matching the per-signal states of uncombined PPP-RTK.  A real slip is
     *     an integer number of wavelengths, so a median under the wavelength floor is
     *     sub-cycle bias, unless the cluster is tight (MAD/|median| <= 0.2), where only
     *     a common pivot disturbance explains several rows.  The report carries the
     *     residual history's temporal confidence (diagnostic only).
     *   - phase cross-band: pools a pivot's rows across bands when sparse per-signal
     *     coverage (mixed BeiDou-2/3, partial signal sets) leaves every group short of
     *     four rows; three rows on two independent bands are stronger evidence than one
     *     band, since multipath and the atmosphere rarely move several frequencies
     *     coherently.
     *   - code per signal and code cross-band: the same for code rows, with a 0.30 m
     *     median floor (routine code multipath reaches 0.1-0.2 m without any pivot
     *     problem), no wavelength floor (cycles do not apply to code) and no temporal
     *     tag (the history is tuned for phase).  The firing group's satellites, whose
     *     ionosphere absorbed the pivot's code bias, are recorded for re-inflation after
     *     the handover.  Combined (ionosphere-free) code signals are not grouped.
     */
    inline constexpr PivotDetectorSpec kPivotDetectors[] = {
        // label               type  pooled rows bands frac  median_m mad   lambda tight reinflate
        //   counter
        { "phase per signal",  'L',  false, 4,   0,    0.5,  0.005,   0.4,  true,  0.2,  false,
          &KalmanQcCounters::pivot_group_slips },
        { "phase cross-band",  'L',  true,  3,   2,    0.5,  0.005,   0.4,  false, 0.0,  false,
          &KalmanQcCounters::pivot_group_slips },
        { "code per signal",   'C',  false, 4,   0,    0.5,  0.30,    0.4,  false, 0.0,  true,
          &KalmanQcCounters::pivot_code_disturbances },
        { "code cross-band",   'C',  true,  3,   2,    0.5,  0.30,    0.4,  false, 0.0,  true,
          &KalmanQcCounters::pivot_code_disturbances },
    };

    /**
     * @brief k of the robust band max(k * MAD, floor) around a firing group's median.
     *
     * Rows outside the band disagree with the coherent median: those satellites are
     * co-disturbed or slipping independently, so the handover must not elect them.
     * k = 3 sits just inside the observed co-offset separation (a second offset
     * satellite lands about 3.2 MAD off the median).  The band is permissive on purpose:
     * PivotManager's retired-pivot backstop guarantees termination regardless.
     */
    inline constexpr double kHandoverExcludeMadK = 3.0;

    /// Floor of the handover-exclusion band (m), guarding MAD -> 0.
    inline constexpr double kHandoverExcludeFloorM = 0.02;

    /**
     * @brief The rows of one group: one (pivot, signal) group, or one pivot's pool.
     */
    struct PivotRowGroup {
        std::vector<std::size_t> indices;  ///< Measurement row indices, in row order.
        std::vector<double> residuals;     ///< Their post-fit residuals (m), same order.
        std::size_t above = 0;             ///< Rows whose |standardized residual| exceeds
                                           ///< the per-row threshold.
        std::set<std::string> bands;       ///< Distinct bands ("L1", "C5") of a pool; empty
                                           ///< for a per-signal group.
    };

    /**
     * @brief The non-pivot single-difference rows of one observable, grouped for the
     *        detectors.
     */
    struct PivotRowGroups {
        /// (pivot, full signal) -> rows, in map order.
        std::map<std::pair<std::string, std::string>, PivotRowGroup> per_signal;
        /// pivot -> its rows across bands, in map order.
        std::map<std::string, PivotRowGroup> pooled;
    };

    /**
     * @brief The statistics of one group, as far as @ref testCoherentOffset computed them.
     *
     * The test stops at the first failed criterion, so the members after it keep their
     * defaults.
     */
    struct CoherentOffset {
        bool coherent = false;   ///< All four criteria passed.
        std::size_t n = 0;       ///< Rows in the group.
        double frac = 0.0;       ///< Fraction of the rows over the per-row threshold.
        double med = 0.0;        ///< Median residual (m).
        double mad = 0.0;        ///< Median absolute deviation about the median (m).
        double mad_ratio = 0.0;  ///< mad / |med|.
    };

    /**
     * @brief Median of @p v, reordering it (nth_element); an even count averages the two
     *        middle values.
     *
     * @param v Values; reordered in place.  Must not be empty.
     * @return The median.
     */
    double medianInPlace(std::vector<double>& v);

    /**
     * @brief Test one group for the coherent-offset signature of @p spec (criteria 1 to 4
     *        of @ref PivotDetectorSpec, in that order).
     *
     * Pure: no state, no logging.  The wavelength floor, the report and the response are
     * the caller's (KalmanFilter::testPivotGroup_).
     *
     * @param spec The detector row.
     * @param g    The group.
     * @return The statistics, with @c coherent true when every criterion passed.
     */
    CoherentOffset testCoherentOffset(const PivotDetectorSpec& spec, const PivotRowGroup& g);

} // namespace kalman

    /**
     * @brief The values the post-fit stages share for one pass of the screen.
     *
     * Built by @c beginPostFitQc_ (S0); the later members are written by the stage named
     * beside them.  The references point into the pass's measurement and the caller's
     * pre-update state, which the stages edit in place.
     */
    struct KalmanFilter::PostFitQcWork {
        // --- S0: the pass's inputs, set once ---
        EpochMeasurement& measurement;   ///< The pass's rows; stages add exclusions, and a code
                                         ///< response trims a row.
        Eigen::VectorXd& x;              ///< Pre-update state; the outlier responses edit it.
        Eigen::MatrixXd& P;              ///< Pre-update covariance; the responses and the
                                         ///< complete reset edit it.
        std::shared_ptr<spdlog::logger> klogger;  ///< The kalman_filter logger.
        std::string epoch_prefix;        ///< Log prefix; always empty (the log pattern carries
                                         ///< the epoch).
        double threshold;                ///< Per-row |standardized residual| threshold
                                         ///< (GnssConfig::getResidualThreshold; no JSON key).
        double bad_meas_percent;         ///< Complete-reset fraction of the epoch's rows
                                         ///< (GnssConfig::getBadMeasPercent; no JSON key).
        const Eigen::VectorXd& res;      ///< measurement.residuals (post-fit, m).
        const Eigen::MatrixXd& vres;     ///< measurement.residuals_covariance (Sr).
        const Eigen::VectorXd& sres;     ///< measurement.residuals_standardized.
        size_t num_obs;                  ///< Rows in this pass.
        size_t total_obs_count;          ///< Rows the epoch started with: this pass's rows plus
                                         ///< the earlier passes' exclusions (the backstop's
                                         ///< pass-invariant denominator).
        size_t meta_n;                   ///< Rows with satellite and signal metadata.

        // --- S1: reportPostFitResiduals_ ---
        size_t max_idx = 0;              ///< Row of the largest |standardized residual|.
        double max_sres = 0.0;           ///< That largest |standardized residual|.
        spdlog::level::level_enum residual_summary_level = spdlog::level::info;
                                         ///< info on a clean pass, debug when outliers follow.
        double sx = 0.0;                 ///< Formal position sigma, x (m), from P.
        double sy = 0.0;                 ///< Formal position sigma, y (m).
        double sz = 0.0;                 ///< Formal position sigma, z (m).

        // --- S5: selectGateRow_ ---
        double gate_stat = 0.0;          ///< The gated statistic: max_sres, or max |w|.
        size_t gate_idx = 0;             ///< Its row.
        double gate_crit = 0.0;          ///< Its critical value.
        bool gate_is_wtest = false;      ///< The gate is the Baarda w-test (data snooping
                                         ///< armed and a checkable row); for the counters.

        // --- S6 and S7: the pass's verdict ---
        bool is_outlier_found = false;   ///< A row was reset or excluded, or the filter reset.
        bool is_complete_reset = false;  ///< The backstop reset the covariance.
    };

} // namespace GnssProcessor
