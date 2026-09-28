#pragma once

/**
 * @file EpochUpdateSupport.hpp
 * @brief Per-epoch measurement-rebuild helpers shared by the post-processing
 *        driver (@c Application::runFilterUpdateLoop) and the real-time engine
 *        (@c RealTimeEngine::processEpoch).
 *
 * The two drivers differ in their per-epoch envelope: post-processing
 * interleaves IEKF geometry relinearization and an MW arc-history reset, and
 * real-time adds SSR application and integer ambiguity resolution on top.  The
 * steps that are the same algorithm in both drivers live here as free
 * functions, so both drivers run one implementation of each and cannot diverge
 * on them.  A duplicated copy drifts: the real-time copy of this code lacked the
 * forced pivot-handover forwarding.
 */

#include "common/PppObservation.hpp"

#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

class StateManager;
class GnssConfig;
class KalmanFilter;
class MeasurementModel;
class MwSlipDetector;
class LogManager;

/**
 * @brief Compute the per-epoch measurement-rebuild pass budget.
 *
 * The rebuild loop peels one post-fit outlier (or applies one ambiguity reset)
 * per pass.  The budget has to be large enough that a pathological epoch ends at
 * the complete-reset backstop (@c bad_residual_count > @c bad_meas_percent ·
 * total_obs) and not at the pass cap.  A budget sized from the ambiguity count
 * alone is enough in phase modes, where the ambiguity count tracks the
 * measurement and outlier count, but falls to the floor of 10 in code-only mode.
 * There an outlier-heavy epoch reaches the cap mid-peel and commits nothing: the
 * state stays at the predicted prior while P inflates without bound.
 *
 * The returned budget is @c max(10, n_ambiguities + 3, ⌊bad_meas_percent·n_meas⌋
 * + 3), clamped to a constant hard ceiling of 256 so the loop always terminates on
 * a pathological observation set.  @p modeled_obs supplies the measurement count
 * (the sum of observed signals, a safe over-estimate of the differenced row count).
 *
 * @param state_manager  Filter state (read for the ambiguity count).
 * @param config         Configuration (read for @c bad_meas_percent).
 * @param modeled_obs    This epoch's modeled observations.
 * @return Maximum rebuild passes for the epoch (10 to 256).
 */
int computeRebuildPassBudget(const StateManager& state_manager,
                             const GnssConfig& config,
                             const std::vector<PppObservation>& modeled_obs);

/**
 * @brief Forward the KF post-fit detector's forced pivot-handover requests to
 *        the @c PivotManager during a measurement-rebuild pass.
 *
 * Drains the three lockstep request maps that the KF populated when it returned
 * @c NeedsMeasurementRebuild (the flagged constellations, their replacement-pivot
 * exclusion sets, and their ionosphere/sat-bias re-inflation sets) and, for each
 * constellation:
 *   - calls @c PivotManager::requestForcedHandover, so that the next rebuild
 *     pass's @c computeMeasurementUpdate re-references the SD frame to a
 *     replacement pivot and re-inflates the contaminated non-pivot states;
 *   - notifies the MW pivot picker (when @p mw_detector is non-null) so that it
 *     does not re-elect the slipped satellite;
 *   - clears the constellation's residual-history EWMA when the tracker is
 *     enabled (post-handover BSSD residuals are not comparable to the
 *     pre-handover statistics);
 *   - releases that constellation's carried exclusions from @p carry_excluded so
 *     that its SD rows re-form against the new pivot on the rebuild pass.
 *
 * @param kalman_filter  KF whose request maps are drained (take-and-clear).
 * @param meas_model     Owns the @c PivotManager that receives the requests.
 * @param mw_detector    MW slip detector to notify; may be null (notify skipped).
 *                       Post-processing passes @c CycleSlipManager::mwDetector();
 *                       real-time passes its own @c MwSlipDetector.
 * @param log_manager    For the "main_log" forwarding/diagnostic lines.
 * @param epoch_prefix   Log prefix string.
 * @param[in,out] carry_excluded  Running excluded-measurement set; handled
 *                       constellations' rows are erased so they re-form.
 */
void forwardKfPivotHandovers(KalmanFilter& kalman_filter,
                             MeasurementModel& meas_model,
                             MwSlipDetector* mw_detector,
                             LogManager& log_manager,
                             const std::string& epoch_prefix,
                             std::set<std::string>& carry_excluded);

/**
 * @brief Re-inflate per-satellite ionosphere and satellite-bias variances for
 *        every satellite whose phase ambiguities the KF reset in full on the
 *        update that triggered the current measurement-rebuild pass.
 *
 * When the KF reseeds every phase ambiguity of a satellite (an outlier or
 * cycle-slip full reset, as opposed to a single-signal reset), the slant-iono
 * and sat-bias states keep the values of the pre-reset partition, and the
 * reseeded ambiguities then lock that partition, which may be wrong, into the
 * rest of the arc.  Inflating the covariance of the affected diagonals lets the
 * filter re-partition over the next few epochs.  This is the
 * @c estimation.slip_state_reinflation guard; both drivers call this helper, so
 * the post-processing and real-time paths apply it identically.
 *
 * Semantics (the same for both callers; call once per rebuild pass):
 *   - No-op when @c KalmanFilter::wasCompleteReset (the reset already inflated
 *     the whole covariance), when @c slip_state_reinflation.enabled is false, or
 *     when both configured standard deviations are ≤ 0.
 *   - Acts only on satellites in @c KalmanFilter::getLastResetSatellites that have
 *     no @c getLastResetSignals entry (a per-signal reset does not justify
 *     satellite-wide reinflation).
 *   - Skips the current BSSD pivots when between-satellite SD is enabled (the
 *     StateManager layer guards their frozen (0,0) rows anyway; skipping avoids a
 *     per-pivot warning line).
 *
 * @param kalman_filter  KF supplying the reset lists / complete-reset flag.
 * @param state_manager  Filter state whose iono / sat-bias diagonals are inflated.
 * @param meas_model     Owns the BSSD pivot map (pivots are skipped).
 * @param config         Read for @c slip_state_reinflation and the BSSD toggle.
 */
void reinflateResetSatStates(const KalmanFilter& kalman_filter,
                             StateManager& state_manager,
                             const MeasurementModel& meas_model,
                             const GnssConfig& config);

} // namespace GnssProcessor
