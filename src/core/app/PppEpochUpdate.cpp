/**
 * @file PppEpochUpdate.cpp
 * @brief The measurement update of one PPP / PPP-RTK epoch and its ambiguity resolution.
 *
 * In the order Application::runPpp (Application.cpp) calls them: the iterative update
 * loop with its KF-reset fan-out (runFilterUpdateLoop), the optional geometry
 * relinearization (IEKF, or the naive pass on the tightly coupled IMU path), the merge of
 * the epoch's resets for the smoother, the GIM ionosphere pseudo-observation, and integer
 * ambiguity resolution with its ATX gates.  Moved or cut from the former single
 * Application::run unchanged (card UOM step U7b; scripts/uom_u7_verify.py proves the
 * move).
 */

#include "app/Application.hpp"
#include "app/EpochUpdateSupport.hpp"
#include "app/PppPipelineWork.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {


// ---------------------------------------------------------------------------
// Epoch helper: Iterative KF update / relinearization loop
// ---------------------------------------------------------------------------

void Application::runFilterUpdateLoop(const std::string& epoch_prefix,
                                      const std::vector<PppObservation>& modeled_obs,
                                      EpochMeasurement& applied_meas,
                                      bool& have_applied,
                                      std::set<std::string>& reset_satellites,
                                      bool& was_complete_reset) {
    reset_satellites.clear();
    was_complete_reset = false;

    // Snapshot the pre-update position for runGeometryRelinearization's |Δpos|
    // gate.  It is captured before any KF update so the outlier-rebuild loop below
    // does not hide the full epoch displacement.  The IEKF's full-state anchor
    // (x⁻, P⁻) is not captured here: it is the prior that the primary update()
    // stashes (KalmanFilter::getLastUpdatePriorX/P), whose dimension matches the
    // post-augmentation state.
    relinearize_pre_update_pos_ = state_manager_->getStateVector().head<3>();
    // IEKF relinearization is active when relinearize_geometry is on and the IMU
    // is not tightly coupled.
    const bool use_iekf_relinear =
        config_.relinearizeGeometry() && !config_.tightlyCoupledImu();
    // Choose a relinearization threshold based on whether phase measurements are present.
    auto has_phase = [&modeled_obs]() {
        for (const auto& obs : modeled_obs) {
            for (const auto& kv : obs.observed) {
                if (!kv.first.empty() && kv.first[0] == 'L') return true;
            }
        }
        return false;
    };
    // With IEKF relinearization active, the in-loop linear relinearization (the
    // pos_jump gate below) is disabled: runGeometryRelinearization then does all
    // relinearization, anchored on the prior with a single covariance commit.
    // An in-loop pass would update a posterior that the IEKF later discards.
    // With the IEKF path off (feature disabled, or a tightly coupled IMU using
    // the legacy naive pass), the thresholds are 100 m with phase and 1000 m
    // without.
    const double kPosRelinearThreshM =
        use_iekf_relinear ? std::numeric_limits<double>::infinity()
                          : (has_phase() ? 100.0 : 1000.0);

    // Pass budget, sized so that the complete-reset backstop, not the pass cap,
    // terminates a pathological epoch (see computeRebuildPassBudget, which the
    // baseline and real-time drivers share, so the sizing is the same in all three).
    const int max_passes =
        computeRebuildPassBudget(*state_manager_, config_, modeled_obs);

    int relinear_pass = 0;
    bool need_relinear = true;
    bool need_update = true;
    size_t carry_bad_residual_count = 0;
    std::set<std::string> carry_excluded_measurements;
    // Soft-LLI bridge: phase signals currently held across a receiver LLI gap
    // pending a TurboEdit integer repair are excluded from the KF update so
    // their still-slipped ambiguities cannot contaminate shared states across
    // the MW one-epoch confirmation delay.  Empty unless turbo_edit_bridge_lli.
    if (cycle_slip_manager_) {
        auto held = cycle_slip_manager_->getHeldPendingBridgeKeys();
        carry_excluded_measurements.insert(held.begin(), held.end());
        // The held ambiguity states must also survive the measurement model's
        // prune-on-first-miss for the duration of the outage, because the
        // bridge repair restores their pre-gap values.  Protection uses the
        // same set as the exclusion above, so the two cannot diverge; the set
        // is empty when nothing is held.
        meas_model_->setProtectedAmbiguityKeys(std::move(held));
    }

    // Concise-log gate: marks the start of a new epoch's update loop.  When
    // logging.kalman_filter_concise_log is on, pass-1 messages are written
    // immediately and passes ≥2 are buffered; endEpoch (below) replays the buffer
    // of the final pass and discards the rest.  No-op when the flag is off.
    if (kalman_filter_) kalman_filter_->beginEpoch();

    while ((need_update || need_relinear) && relinear_pass < max_passes) {
        ++relinear_pass;
        need_update = false;

        // In tightly-coupled mode, provide current INS attitude and specific
        // force to the measurement model so Doppler Jacobian rows include
        // attitude-error and accelerometer-bias partials (implemented in
        // ImuAidingSource).
        if (aiding_manager_) aiding_manager_->applyTcJacobianInputs(*meas_model_);

        EpochMeasurement meas_update = meas_model_->computeMeasurementUpdate(
            modeled_obs, state_manager_->getStateVector(), carry_excluded_measurements);
        if (meas_update.innovations.size() == 0 || !meas_model_->validateMeasurementUpdate(meas_update)) {
            log_manager_.getLogger("main_log")->warn("{}Invalid or empty measurement update (pass {})",
                epoch_prefix, relinear_pass);
            break;
        }

        // Carry forward the bad-residual count so the full-reset safety net accumulates across passes.
        meas_update.bad_residual_count = carry_bad_residual_count;

        // Append composable aiding measurement rows (ZUPT/baro/...) into the
        // finalized measurement before the KF consumes it.  Row sources are
        // restricted to undifferenced PPP for now; appendAidingRow hard-errors
        // if R_full is populated (BSSD/DD active).  The state vector is held in
        // a local so the context's reference outlives collectMeasurement.
        if (aiding_manager_ && !aiding_manager_->empty()) {
            const Eigen::VectorXd aiding_meas_x = state_manager_->getStateVector();
            const AidingEpochContext aiding_meas_ctx{
                aiding_meas_x, prev_epoch_time_, meas_update.time, false };
            for (const auto& row : aiding_manager_->collectMeasurement(aiding_meas_ctx))
                MeasurementModel::appendAidingRow(meas_update, row);
        }

        const Eigen::Vector3d old_pos = ppp_state_.position;

        const auto result = kalman_filter_->update(meas_update);

        try {
            state_manager_->updatePppStateFromVector(state_manager_->getStateVector(), ppp_state_, *state_manager_);
        } catch (const std::exception& e) {
            log_manager_.getLogger("main_log")->error("{}Failed to update ppp_state after Kalman update: {}",
                epoch_prefix, e.what());
            break;
        }

        if (result == KalmanFilter::UpdateResult::NeedsMeasurementRebuild) {
            carry_bad_residual_count = meas_update.bad_residual_count;
            carry_excluded_measurements = meas_update.excluded_measurements;

            log_manager_.getLogger("main_log")->debug(
                "{}KF requested measurement rebuild (pass {}): recalculating innovations after ambiguity reset/outlier handling",
                epoch_prefix, relinear_pass);

            // Forward the KF post-fit detector's forced pivot-handover requests
            // (drained take-and-clear) to the PivotManager, notify the MW pivot
            // picker, reset per-constellation residual history, and release the
            // handled constellations' carried exclusions so their SD rows re-form
            // against the new pivot.  The real-time driver calls the same
            // EpochUpdateSupport helper, so both paths perform the same handover.
            forwardKfPivotHandovers(
                *kalman_filter_, *meas_model_,
                cycle_slip_manager_ ? cycle_slip_manager_->mwDetector() : nullptr,
                log_manager_, epoch_prefix, carry_excluded_measurements);

            // Residual-history tracker, shared by the MW arc-history reset below
            // (the per-constellation handover reset is done inside the helper).
            auto& res_hist = kalman_filter_->getResidualHistoryTracker();

            // Reset MW slip detector history for satellites that had ambiguities reset.
            // Phase wind-up is not reset: it is a continuous geometric quantity,
            // unaffected by the integer-cycle jump of an in-arc cycle slip.
            if (kalman_filter_->wasCompleteReset()) {
                // Complete reset: clear all MW arc history so pre-reset
                // statistics do not enter post-reset arcs.
                was_complete_reset = true;
                for (const auto& obs : modeled_obs) {
                    cycle_slip_manager_->resetSatellite(obs.satellite);
                    reset_satellites.insert(obs.satellite);
                }
                if (res_hist.isEnabled()) res_hist.resetAll();
                log_manager_.getLogger("main_log")->info(
                    "{}Reset MW slip detector state (complete filter reset)", epoch_prefix);
            } else {
                // Per-(sat, signal) MW arc cleanup when the KF recorded which
                // signal was reset.  MW arcs on signal pairs that do not
                // involve the slipped signal are kept (an L1C slip leaves the
                // L2W/L5X arc intact).  Satellites in last_reset_satellites_
                // without a per-signal entry fall back to the per-sat clear.
                std::set<std::string> covered_sats;
                for (const auto& [sat_id, sig] : kalman_filter_->getLastResetSignals()) {
                    cycle_slip_manager_->resetSatelliteSignal(sat_id, sig);
                    reset_satellites.insert(sat_id);
                    covered_sats.insert(sat_id);
                    // Residual history is per satellite; the loop below resets it
                    // once for each satellite in getLastResetSatellites().
                    log_manager_.getLogger("main_log")->debug(
                        "{}Reset MW history for {} {} (KF ambiguity reset, per-signal)",
                        epoch_prefix, sat_id, sig);
                }
                for (const auto& sat_id : kalman_filter_->getLastResetSatellites()) {
                    if (covered_sats.count(sat_id)) {
                        // Already covered by a per-signal reset: reset the
                        // residual history once and skip the sat-wide MW clear.
                        if (res_hist.isEnabled()) res_hist.resetSatellite(sat_id);
                        continue;
                    }
                    cycle_slip_manager_->resetSatellite(sat_id);
                    reset_satellites.insert(sat_id);
                    if (res_hist.isEnabled()) res_hist.resetSatellite(sat_id);
                    log_manager_.getLogger("main_log")->debug(
                        "{}Reset MW history for {} (KF ambiguity reset, per-sat fallback)",
                        epoch_prefix, sat_id);
                }
            }

            // Satellite-wide (iono, sat-bias) re-inflation for satellites whose
            // phase ambiguities were fully reset this pass.  It mirrors the
            // CycleSlipManager full-reset path and lives in EpochUpdateSupport,
            // so the real-time engine applies the same partition-baking guard.
            // No-op on a complete reset (checked inside) and when
            // slip_state_reinflation is disabled.
            reinflateResetSatStates(*kalman_filter_, *state_manager_,
                                    *meas_model_, config_);

            need_update = true;
            continue;
        }

        if (result == KalmanFilter::UpdateResult::Applied) {
            applied_meas = meas_update;
            have_applied = true;
        }

        if (ppp_state_.position.norm() < 1e-6 || !ppp_state_.position.allFinite()) {
            log_manager_.getLogger("main_log")->warn("{}Invalid position after Kalman update, reverting to initial position", epoch_prefix);
            ppp_state_.position = initial_position_;
            Eigen::VectorXd sv = state_manager_->getStateVector();
            sv.segment<3>(state_manager_->getPositionIndex()) = initial_position_;
            state_manager_->setStateVector(sv);
        }

        const double pos_jump = (ppp_state_.position - old_pos).norm();
        log_manager_.getLogger("main_log")->trace(
            "{}Post-update (pass {}): pos_jump={:.4f} m",
            epoch_prefix, relinear_pass, pos_jump);

        need_relinear = (pos_jump > kPosRelinearThreshM);
    }

    // Concise-log gate: replay the final pass's buffer (the "last pass"
    // visible in the kalman_filter log) and restore passthrough mode.  No-op
    // when the gate is null.  Runs however the loop exited (max_passes hit, no
    // more rebuilds, or measurement failure), so the gate state is consistent
    // for the next epoch's beginEpoch.
    if (kalman_filter_) kalman_filter_->endEpoch();
}

// ---------------------------------------------------------------------------
// Epoch helper: Geometry relinearization
// ---------------------------------------------------------------------------

void Application::runGeometryRelinearization(const std::string& epoch_prefix,
                                             const std::vector<RinexObservation>& epoch_obs,
                                             EpochMeasurement& applied_meas,
                                             bool have_applied) {
    if (!config_.relinearizeGeometry() || !have_applied) return;

    // ---- Gates -------------------------------------------------------
    // 1. |Δpos| threshold, chosen by whether the epoch has phase: skip
    //    relinearization when the primary update moved the position less
    //    than the threshold.  Phase data constrains position tightly, so the
    //    1 m default fires only in early convergence; code-only epochs have
    //    SPP-level jitter of about 5 m, so the 100 m default fires only on
    //    large jumps (cold start, post-gap recovery).  A threshold ≤ 0 disables
    //    this gate.
    // 2. Per-run cap on total relinearizations, bounding worst-case cost on
    //    pathological inputs (-1 = unlimited).
    const Eigen::Vector3d post_update_pos =
        state_manager_->getStateVector().head<3>();
    const double dpos = (post_update_pos - relinearize_pre_update_pos_).norm();

    bool has_phase = false;
    for (const auto& obs : epoch_obs) {
        for (const auto& [code, val] : obs.measurements) {
            if (!code.empty() && code[0] == 'L') { has_phase = true; break; }
        }
        if (has_phase) break;
    }
    const double thresh = has_phase
        ? config_.relinearizeDposThresholdPhaseM()
        : config_.relinearizeDposThresholdCodeOnlyM();
    if (thresh > 0.0 && dpos < thresh) {
        log_manager_.getLogger("main_log")->debug(
            "{}Geometry relinearization skipped: |Δpos|={:.4f} m < threshold {:.4f} m ({})",
            epoch_prefix, dpos, thresh, has_phase ? "phase" : "code-only");
        return;
    }
    const int cap = config_.relinearizeMaxCount();
    if (cap >= 0 && relinearize_count_ >= cap) {
        // No per-epoch log once the cap is hit; one info-level "cap reached"
        // line is written when the count first reaches it.
        if (relinearize_count_ == cap) {
            log_manager_.getLogger("main_log")->info(
                "{}Geometry relinearization cap ({}) reached; suppressing further passes",
                epoch_prefix, cap);
            ++relinearize_count_;  // bump past so the message fires once
        }
        return;
    }

    // ---- Tightly-coupled IMU: legacy single naive second pass ----------
    // Re-deriving the posterior from the epoch prior conflicts with the
    // error-state attitude/bias nominal feedback that the primary update already
    // folded into C_b_e_ (and zeroed).  An IEKF on this path needs deferred
    // nominal feedback, so tightly coupled configurations keep the single naive
    // second pass.
    if (aiding_manager_ && aiding_manager_->anySourceRequiresNaiveRelinearization()) {
        runGeometryRelinearizationNaive(epoch_prefix, epoch_obs, applied_meas);
        return;
    }

    // ---- IEKF: iterate computeModels → updateFromPrior ----------------
    // Every Gauss–Newton step is anchored on the captured epoch prior (x⁻, P⁻),
    // with the measurement set held fixed to the one the primary update
    // converged on (same outlier exclusions).  Each step is then a Gauss–Newton
    // iteration on the MAP estimate, and the covariance (I−KH)·P⁻ is committed
    // once, so no information is counted twice.  GNSS geometry is nearly
    // linear, so this converges in one pass except after large cold-start or
    // post-gap jumps.
    // The anchor is the prior that the primary update() stashed this epoch.
    // It already reflects this epoch's ambiguity augmentation and outlier
    // resets, so its dimension matches the post-update state and updateFromPrior
    // never hits a dimension mismatch.  have_applied (checked above) guarantees
    // it is fresh.  The references stay valid across the loop: updateFromPrior
    // does not touch the stash (only update() writes it).
    const Eigen::VectorXd& prior_x = kalman_filter_->getLastUpdatePriorX();
    const Eigen::MatrixXd& prior_P = kalman_filter_->getLastUpdatePriorP();
    if (prior_x.size() == 0 || prior_P.size() == 0) {
        log_manager_.getLogger("main_log")->debug(
            "{}Geometry relinearization (IEKF): no stashed prior; skipping", epoch_prefix);
        return;
    }

    // Copy (not reference) the converged exclusion set: applied_meas is
    // reassigned inside the loop, which would otherwise mutate the set out
    // from under the iteration.
    const std::set<std::string> fixed_excludes = applied_meas.excluded_measurements;
    constexpr int    kMaxIekfIters = 5;
    constexpr double kIekfPosTolM  = 1e-4;   // 0.1 mm: geometry change is sub-noise
    bool any_applied = false;

    for (int iter = 0; iter < kMaxIekfIters; ++iter) {
        // Sync ppp_state_ to the current iterate so computeModels recomputes
        // the geometry (sat positions, LOS, troposphere, corrections) about it.
        try {
            state_manager_->updatePppStateFromVector(
                state_manager_->getStateVector(), ppp_state_, *state_manager_);
        } catch (const std::exception& e) {
            log_manager_.getLogger("main_log")->error(
                "{}Relinearize(IEKF): failed to sync ppp_state: {}", epoch_prefix, e.what());
            break;
        }

        std::vector<PppObservation> relinear_obs;
        if (!ppp_model_->computeModels(epoch_obs, ppp_state_, relinear_obs) ||
            relinear_obs.empty()) {
            log_manager_.getLogger("main_log")->debug(
                "{}Geometry relinearization (IEKF): computeModels failed or empty (iter {})",
                epoch_prefix, iter);
            break;
        }

        EpochMeasurement relinear_meas = meas_model_->computeMeasurementUpdate(
            relinear_obs, state_manager_->getStateVector(), fixed_excludes);
        if (relinear_meas.innovations.size() == 0 ||
            !meas_model_->validateMeasurementUpdate(relinear_meas)) {
            log_manager_.getLogger("main_log")->debug(
                "{}Geometry relinearization (IEKF): empty/invalid measurement (iter {})",
                epoch_prefix, iter);
            break;
        }

        const Eigen::Vector3d pos_before = state_manager_->getStateVector().head<3>();
        const auto res = kalman_filter_->updateFromPrior(
            relinear_meas, prior_x, prior_P);
        if (res != KalmanFilter::UpdateResult::Applied) {
            log_manager_.getLogger("main_log")->debug(
                "{}Geometry relinearization (IEKF): updateFromPrior not applied (iter {}, result={})",
                epoch_prefix, iter, static_cast<int>(res));
            break;
        }
        any_applied = true;
        applied_meas = relinear_meas;

        const Eigen::Vector3d pos_after = state_manager_->getStateVector().head<3>();
        const double step = (pos_after - pos_before).norm();
        if (step < kIekfPosTolM) {
            log_manager_.getLogger("main_log")->debug(
                "{}Geometry relinearization (IEKF) converged at iter {} (step={:.2e} m)",
                epoch_prefix, iter, step);
            break;
        }
        if (iter + 1 == kMaxIekfIters) {
            log_manager_.getLogger("main_log")->debug(
                "{}Geometry relinearization (IEKF) hit iter cap {} (last step={:.4f} m)",
                epoch_prefix, kMaxIekfIters, step);
        }
    }

    if (any_applied) {
        ++relinearize_count_;
        try {
            state_manager_->updatePppStateFromVector(
                state_manager_->getStateVector(), ppp_state_, *state_manager_);
        } catch (const std::exception& e) {
            log_manager_.getLogger("main_log")->error(
                "{}Relinearize(IEKF): failed to sync after update: {}", epoch_prefix, e.what());
        }
        log_manager_.getLogger("main_log")->debug(
            "{}Geometry relinearization (IEKF) applied", epoch_prefix);
    }
}

// ---------------------------------------------------------------------------
// Epoch helper: Geometry relinearization, legacy naive second pass
// ---------------------------------------------------------------------------

void Application::runGeometryRelinearizationNaive(const std::string& epoch_prefix,
                                                  const std::vector<RinexObservation>& epoch_obs,
                                                  EpochMeasurement& applied_meas) {
    // A single extra full update() layered on top of the primary posterior.
    // It re-applies the same measurement information against an
    // already-tightened covariance (mild over-confidence), and is used only on
    // the tightly coupled IMU path, where the prior-anchored IEKF would
    // conflict with the error-state nominal feedback.  The caller evaluates the
    // |Δpos| and per-run-cap gates.
    try {
        state_manager_->updatePppStateFromVector(state_manager_->getStateVector(), ppp_state_, *state_manager_);
    } catch (const std::exception& e) {
        log_manager_.getLogger("main_log")->error("{}Relinearize: failed to sync ppp_state: {}", epoch_prefix, e.what());
        return;
    }

    std::vector<PppObservation> relinear_obs;
    if (!ppp_model_->computeModels(epoch_obs, ppp_state_, relinear_obs) || relinear_obs.empty()) {
        log_manager_.getLogger("main_log")->debug("{}Geometry relinearization: computeModels failed or empty", epoch_prefix);
        return;
    }

    // Set the IMU state for the tightly coupled Jacobian partials (implemented
    // in ImuAidingSource).
    if (aiding_manager_) aiding_manager_->applyTcJacobianInputs(*meas_model_);

    EpochMeasurement relinear_meas = meas_model_->computeMeasurementUpdate(
        relinear_obs, state_manager_->getStateVector());
    if (relinear_meas.innovations.size() == 0 || !meas_model_->validateMeasurementUpdate(relinear_meas)) return;

    auto relinear_result = kalman_filter_->update(relinear_meas);
    ++relinearize_count_;
    if (relinear_result == KalmanFilter::UpdateResult::Applied) {
        applied_meas = relinear_meas;
        try {
            state_manager_->updatePppStateFromVector(state_manager_->getStateVector(), ppp_state_, *state_manager_);
        } catch (const std::exception& e) {
            log_manager_.getLogger("main_log")->error("{}Relinearize: failed to sync after update: {}", epoch_prefix, e.what());
        }
        log_manager_.getLogger("main_log")->debug("{}Geometry relinearization applied", epoch_prefix);
    } else {
        log_manager_.getLogger("main_log")->debug("{}Geometry relinearization update not applied (result={})",
            epoch_prefix, static_cast<int>(relinear_result));
    }
}

// ---------------------------------------------------------------------------
// Epoch helper: the epoch's resets, merged for the smoother
// ---------------------------------------------------------------------------

void Application::mergeEpochResets_(PppEpochWork& w) {
    const auto& slip_result = w.slip_result;
    auto& epoch_reset_sats = w.epoch_reset_sats;
    auto& epoch_complete_reset = w.epoch_complete_reset;

    // Relinearization can also trigger ambiguity or complete resets through
    // its internal update() call.  Such resets are merged into the epoch's
    // accumulators before the smoother captures the posterior.
    if (kalman_filter_->wasCompleteReset()) epoch_complete_reset = true;
    for (const auto& sat_id : kalman_filter_->getLastResetSatellites()) {
        epoch_reset_sats.insert(sat_id);
    }
    // Merge the slip-phase resets (SD-aware resets, pivot-disturbance
    // inflations, soft-LLI bridge restores, post-handover deferred repairs)
    // so the smoother segments them as well as the KF-internal resets
    // (audit F1).
    epoch_reset_sats.insert(slip_result.reset_satellites.begin(),
                            slip_result.reset_satellites.end());
}

// ---------------------------------------------------------------------------
// Epoch helper: GIM ionosphere-weighting pseudo-observation update
// ---------------------------------------------------------------------------

void Application::applyGimIonoUpdate(const GnssTime& current_time,
                                     const std::vector<PppObservation>& modeled_obs) {
    const auto& iw = config_.getIonosphereWeightingConfig();
    if (!iw.enabled || !gim_ || !gim_->isLoaded() || !kalman_filter_ || !state_manager_) {
        return;
    }

    const Eigen::VectorXd x = state_manager_->getStateVector();
    if (x.size() < 3) return;
    const Eigen::Vector3d rx = x.head<3>();

    // One pseudo-observation row per satellite that owns a slant-iono
    // state and has a valid GIM value above the elevation cutoff.
    std::vector<int>    idx;    // iono-state column index
    std::vector<double> innov;  // GIM slant delay - current iono state (m)
    std::vector<double> var;    // per-row pseudo-obs variance (m^2)
    std::vector<System> cons;   // constellation, for the common-mode block structure

    // BSSD pivot satellites hold their slant-iono state frozen (value 0,
    // variance 0) as the rank-fixing datum; they must be excluded from the
    // pseudo-observation entirely (see the per-row guard below).
    const std::set<std::string>& frozen_pivots =
        state_manager_->getFrozenPivotSatellites();

    for (const auto& obs : modeled_obs) {
        if (obs.elevation < iw.elevation_cutoff_deg) continue;
        const int iono_idx = state_manager_->getIonosphereIndex(obs.satellite);
        if (iono_idx < 0 || iono_idx >= static_cast<int>(x.size())) continue;
        // Skip BSSD pivot / any frozen-datum iono row.  A zero-variance state
        // receives zero Kalman gain, so it cannot benefit from the GIM; but
        // because it shares the per-constellation common-mode block in R, its
        // spurious innovation (GIM_STEC - 0 ~ 5-30 m) would otherwise leak into
        // every other same-constellation iono state through the off-diagonal
        // S^-1.  The pivot's datum offset is exactly what the common-mode block
        // is meant to absorb from the *remaining* rows, so it must not appear
        // as a row itself.
        if (frozen_pivots.count(obs.satellite) ||
            state_manager_->getStateVariance(iono_idx) <= 0.0) {
            continue;
        }
        // The slant-iono state is referenced to GPS L1 for every
        // constellation (see the MeasurementModel iono Jacobian).
        const GimQuery q = gim_->slantIonoDelay(current_time, rx, obs.sat_pos,
                                                Constants::GPS_L1_FREQ);
        if (!q.valid) continue;
        const double sigma = std::max(iw.floor_m, iw.std_scale * q.std_m);
        idx.push_back(iono_idx);
        innov.push_back(q.delay_m - x(iono_idx));
        var.push_back(sigma * sigma);
        cons.push_back(obs.system);
    }

    const int m = static_cast<int>(idx.size());
    if (m == 0) return;
    const int n = static_cast<int>(x.size());

    Eigen::MatrixXd H = Eigen::MatrixXd::Zero(m, n);
    Eigen::VectorXd z(m);
    for (int k = 0; k < m; ++k) {
        H(k, idx[k]) = 1.0;
        z(k) = innov[k];
    }

    // R = block-diagonal over constellations of  diag(var) + sigma_c^2 * 11^T.
    // The all-ones block leaves the per-constellation common mode (the
    // un-modellable receiver iono-datum bias) unconstrained, so only the
    // between-satellite structure of the GIM is imposed.
    const double common_var = iw.common_mode_sigma_m * iw.common_mode_sigma_m;
    Eigen::MatrixXd R = Eigen::MatrixXd::Zero(m, m);
    for (int r = 0; r < m; ++r) {
        for (int c = 0; c < m; ++c) {
            if (cons[r] == cons[c]) R(r, c) = common_var;
        }
        R(r, r) += var[r];
    }

    kalman_filter_->applyPseudoObservation(H, z, R);

    try {
        state_manager_->updatePppStateFromVector(state_manager_->getStateVector(),
                                                 ppp_state_, *state_manager_);
    } catch (const std::exception& e) {
        log_manager_.getLogger("main_log")->error(
            "GIM iono update: failed to sync ppp_state: {}", e.what());
    }

    double sum_abs = 0.0;
    for (double e : innov) sum_abs += std::abs(e);
    log_manager_.getLogger("ionosphere")->debug(
        "GIM iono update: {} sats constrained, mean|innov|={:.3f} m",
        m, sum_abs / static_cast<double>(m));
}

// ---------------------------------------------------------------------------
// Epoch stage 4i2: integer ambiguity resolution
// ---------------------------------------------------------------------------

void Application::resolveEpochAmbiguities_(const PppEpochWork& w) {
    const auto& epoch_prefix = w.epoch_prefix;
    const auto& modeled_obs = w.modeled_obs;
    const auto& have_applied = w.have_applied;

    // 4i2. Integer ambiguity resolution (MLAMBDA).
    if (!ambiguity_resolver_ || !have_applied) return;
    auto wl_ambs  = cycle_slip_manager_->getResolvedWlAmbiguities();
    auto wl_fracs = cycle_slip_manager_->getResolvedWlFractions();

    // The AR-side ATX gates withhold integer fixing on the WL entries whose
    // signals' antenna corrections are missing or frequency-substituted.
    dropAtxGatedWlEntries_(epoch_prefix, wl_ambs, wl_fracs);

    // WL-only FFR gate input (see AmbiguityResolverConfig::wl_only_ffr).
    // Built from the unfiltered detector map on purpose: entries whose
    // integer the ATX gate above dropped simply never get looked up,
    // because no constraint is formed for them.
    std::map<std::string, WlArcQuality> wl_quality;
    for (const auto& [k, q] : cycle_slip_manager_->getResolvedWlQuality())
        wl_quality[k] = WlArcQuality{ q.sigma_mean_cycles, q.count, q.exact };
    auto ar_result = ambiguity_resolver_->resolve(*state_manager_, modeled_obs, wl_ambs, wl_fracs, wl_quality);
    if (ar_result.success) {
        // Apply fixed state to the filter
        state_manager_->setStateVector(ar_result.fixed_state);
        state_manager_->setCovariance(ar_result.fixed_covariance);

        // Fix-and-hold: constrain ambiguities to integer values
        ambiguity_resolver_->holdAmbiguities(*state_manager_, ar_result);

        log_manager_.getLogger("main_log")->info(
            "{}AR: fixed {}/{} ambs, ratio={:.2f}",
            epoch_prefix, ar_result.num_fixed, ar_result.num_candidates,
            ar_result.ratio);
    }
}

// ---------------------------------------------------------------------------
// Epoch helper: the AR-side ATX gates on the WL entries
// ---------------------------------------------------------------------------

void Application::dropAtxGatedWlEntries_(const std::string& epoch_prefix,
                                         std::map<std::string, int>& wl_ambs,
                                         std::map<std::string, double>& wl_fracs) {
    // Optional AR-side ATX gates: drop WL entries by the ATX coverage
    // status of either signal in the pair.  The observations stay in
    // the float Kalman update; only integer fixing is withheld for
    // these signals, because missing or approximate antenna PCV
    // introduces sub-cycle errors that bias the integer determination.
    //
    // Two complementary gates, usable singly, together, or not at all:
    //   require_atx_coverage_for_ar     drops pairs without strict
    //                                   coverage on either signal
    //                                   (substituted or missing).
    //   restrict_ar_for_substituted_atx drops pairs where either
    //                                   signal's ATX correction came
    //                                   from the frequency-fallback
    //                                   chain (substituted only; pairs
    //                                   with no ATX at all pass).
    const auto& mc = config_.getModelCorrections();
    const bool gate_strict      = mc.require_atx_coverage_for_ar;
    const bool gate_substituted = mc.restrict_ar_for_substituted_atx;
    if ((gate_strict || gate_substituted) && ppp_model_) {
        int n_dropped_strict      = 0;
        int n_dropped_substituted = 0;
        for (auto it = wl_ambs.begin(); it != wl_ambs.end(); ) {
            // Key format: "<sat>_<sig1>_<sig2>".
            const auto& key = it->first;
            const auto u1 = key.find('_');
            const auto u2 = (u1 == std::string::npos) ? std::string::npos
                                                        : key.find('_', u1 + 1);
            if (u1 == std::string::npos || u2 == std::string::npos) {
                ++it; continue;
            }
            const std::string sat  = key.substr(0, u1);
            const std::string sig1 = key.substr(u1 + 1, u2 - u1 - 1);
            const std::string sig2 = key.substr(u2 + 1);
            const System sys = sat.empty() ? System::Unknown :
                sat[0] == 'G' ? System::GPS :
                sat[0] == 'E' ? System::Galileo :
                sat[0] == 'R' ? System::GLONASS :
                sat[0] == 'C' ? System::BeiDou :
                sat[0] == 'J' ? System::QZSS :
                sat[0] == 'S' ? System::SBAS : System::Unknown;
            bool drop = false;
            if (gate_strict) {
                const bool a1 = ppp_model_->hasFullAtxCoverage(sat, sig1, sys);
                const bool a2 = ppp_model_->hasFullAtxCoverage(sat, sig2, sys);
                if (!a1 || !a2) { drop = true; ++n_dropped_strict; }
            }
            if (!drop && gate_substituted) {
                const bool s1 = ppp_model_->wasAtxSubstituted(sat, sig1, sys);
                const bool s2 = ppp_model_->wasAtxSubstituted(sat, sig2, sys);
                if (s1 || s2) { drop = true; ++n_dropped_substituted; }
            }
            if (drop) {
                wl_fracs.erase(key);
                it = wl_ambs.erase(it);
            } else {
                ++it;
            }
        }
        if (n_dropped_strict > 0 || n_dropped_substituted > 0) {
            log_manager_.getLogger("main_log")->debug(
                "{}AR ATX gate: dropped {} (strict-coverage), {} (substituted) WL entries",
                epoch_prefix, n_dropped_strict, n_dropped_substituted);
        }
    }
}

} // namespace GnssProcessor
