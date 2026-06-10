#pragma once

/**
 * @file EpochUpdateSupport.hpp
 * @brief Shared per-epoch measurement-rebuild helpers used by BOTH the
 *        post-processing driver (@c Application::runFilterUpdateLoop) and the
 *        real-time engine (@c RealTimeEngine::processEpoch).
 *
 * The two drivers necessarily differ in their per-epoch envelope —
 * post-processing interleaves IEKF geometry relinearization and an elaborate
 * MW arc-history reset; real-time layers SSR application and integer ambiguity
 * resolution on top.  But the parts that are genuinely the *same algorithm*
 * must not be duplicated, or they drift: the real-time path silently lacked the
 * forced-pivot-handover forwarding for a long time precisely because it was a
 * copy that fell behind.  These free functions are the single source of truth
 * for those shared pieces so the two drivers can never diverge on them again.
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
 * per pass, so the budget must be large enough that the complete-reset backstop
 * (@c bad_residual_count > @c bad_meas_percent · total_obs) — NOT the pass cap —
 * terminates a pathological epoch.  Sizing it only from the ambiguity count did
 * that for phase modes (there the ambiguity count tracks the measurement/outlier
 * count) but collapsed to the floor of 10 in code-only mode, where an
 * outlier-heavy epoch then hit the cap mid-peel and committed nothing — freezing
 * the state at the predicted prior while P inflated unbounded.
 *
 * The returned budget is @c max(10, n_ambiguities + 3, ⌈bad_meas_percent·n_meas⌉
 * + 3), clamped to a constant hard ceiling so the loop can never run unbounded on
 * a pathological observation set.  @p modeled_obs supplies the measurement count
 * (sum of observed signals — a safe over-estimate of the differenced row count).
 *
 * @param state_manager  Filter state (read for the ambiguity count).
 * @param config         Configuration (read for @c bad_meas_percent).
 * @param modeled_obs    This epoch's modeled observations.
 * @return Maximum rebuild passes for the epoch (≥ 10).
 */
int computeRebuildPassBudget(const StateManager& state_manager,
                             const GnssConfig& config,
                             const std::vector<PppObservation>& modeled_obs);

/**
 * @brief Forward the KF post-fit detector's forced pivot-handover requests to
 *        the @c PivotManager during a measurement-rebuild pass.
 *
 * Drains the three lockstep request maps the KF populated when it returned
 * @c NeedsMeasurementRebuild — the flagged constellations, their replacement-
 * pivot exclusion sets, and their ionosphere/sat-bias re-inflation sets — and
 * for each constellation:
 *   - calls @c PivotManager::requestForcedHandover (so the next rebuild pass's
 *     @c computeMeasurementUpdate re-references the SD frame to a healthy pivot
 *     and re-inflates the contaminated non-pivot states),
 *   - notifies the MW pivot picker (when @p mw_detector is non-null) so it does
 *     not re-elect the slipped satellite,
 *   - clears the constellation's residual-history EWMA (post-handover BSSD
 *     residuals are not comparable to the pre-handover statistics),
 *   - releases that constellation's carried exclusions from @p carry_excluded so
 *     its SD rows re-form against the new pivot on the rebuild pass.
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
 *        every satellite whose phase ambiguities the KF FULLY reset on the
 *        update that triggered the current measurement-rebuild pass.
 *
 * When the KF reseeds every phase ambiguity for a satellite (an outlier /
 * cycle-slip full reset, as opposed to a single-signal reset) the slant-iono
 * and sat-bias states keep whatever value the pre-reset partition had, and the
 * reseeded ambiguities then bake that (possibly wrong) partition into the rest
 * of the arc.  Breaking the affected diagonals' covariance lets the filter
 * re-partition cleanly over the next few epochs.  This is the
 * @c estimation.slip_state_reinflation guard; it was previously inlined in the
 * post-processing driver only, leaving the real-time engine without it — the
 * asymmetry this helper removes.
 *
 * Semantics (identical for both callers — call once per rebuild pass):
 *   - No-op when @c KalmanFilter::wasCompleteReset (the reset already inflated
 *     the whole covariance) or when @c slip_state_reinflation.enabled is false.
 *   - Acts only on satellites in @c KalmanFilter::getLastResetSatellites that do
 *     NOT also carry a @c getLastResetSignals entry (a per-signal reset does not
 *     justify sat-wide reinflation).
 *   - Skips the current BSSD pivots (their frozen (0,0) rows are guarded at the
 *     StateManager layer anyway; skipping avoids a noisy per-pivot warn line).
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
