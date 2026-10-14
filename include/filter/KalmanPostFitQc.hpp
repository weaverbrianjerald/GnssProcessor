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
 *   S3   detectPivotOffsets_        coherent pivot offsets, phase and code
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
 * Included only by src/core/filter/KalmanPostFitQc.cpp.
 */

#include "filter/KalmanFilter.hpp"
#include <spdlog/spdlog.h>
#include <Eigen/Dense>
#include <cstddef>
#include <memory>
#include <string>

namespace GnssProcessor {

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

        // --- S6 and S7: the pass's verdict ---
        bool is_outlier_found = false;   ///< A row was reset or excluded, or the filter reset.
        bool is_complete_reset = false;  ///< The backstop reset the covariance.
    };

} // namespace GnssProcessor
