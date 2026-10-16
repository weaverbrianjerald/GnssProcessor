/**
 * @file PppEpochRecords.cpp
 * @brief What one PPP / PPP-RTK epoch records: the smoother snapshot halves and the
 *        slip-repair fold, the NEQ accumulation, the soft-LLI bridge snapshot, the summary
 *        line and the CSV traces.
 *
 * None of these changes the filter state except through the smoother's own buffer.
 */

#include "app/Application.hpp"
#include "app/PppPipelineWork.hpp"
#include "math/TimeUtils.hpp"
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <fmt/format.h>

namespace GnssProcessor {


// ---------------------------------------------------------------------------
// Epoch helper: Smoother snapshot captures
// ---------------------------------------------------------------------------

void Application::capturePriorSnapshot(const GnssTime& current_time, double dt) {
    if (!config_.runSmoother()) return;

    SmootherEpochSnapshot snap;
    snap.t  = current_time;
    snap.dt = dt;
    snap.x_pred = state_manager_->getStateVector();
    snap.P_pred = state_manager_->getCovariance();
    // Transition matrix from the previous epoch to this one.  Evaluated
    // at the current (post-predict) state size, which equals the state
    // size at the end of the previous epoch (predict does not grow the
    // state).  At the first epoch there is no "previous posterior" to
    // propagate, so the smoother terminates at k=0 and the stored F is
    // not used.
    snap.F = state_manager_->getTransitionMatrix(dt);
    // Default x_post/P_post mirror the prior so that has_posterior=false
    // (skipped update) falls through to the forward solution.
    snap.x_post = snap.x_pred;
    snap.P_post = snap.P_pred;
    smoother_buffer_.push_back(std::move(snap));
}

// ---------------------------------------------------------------------------
// Epoch helper: slip repairs folded into the smoother a-priori
// ---------------------------------------------------------------------------

void Application::foldSlipRepairsIntoPrior_(const PppEpochWork& w) {
    const auto& slip_result = w.slip_result;

    // Smoother: fold covariance-preserving repairs into the just-captured
    // a-priori as a deterministic control input (audit F1).  A repair shifts
    // the ambiguity by a known dN·λ in the same (pre-update) frame in which
    // x_pred was captured; adding it to x_pred keeps the backward pass from
    // absorbing the jump as a spurious measurement innovation.  Indices
    // resolve against the live layout, which still equals x_pred's layout
    // here (predict and slip detection do not reindex existing keys; state
    // growth happens later in the update loop).  A key whose satellite was
    // also reset this epoch is segmented, not compensated (R6).
    if (config_.runSmoother() && !smoother_buffer_.empty()
        && !slip_result.ambiguity_repairs.empty()) {
        SmootherEpochSnapshot& snap = smoother_buffer_.back();
        for (const auto& [amb_key, delta_m] : slip_result.ambiguity_repairs) {
            const auto us = amb_key.find('_');
            const std::string sat = (us != std::string::npos)
                                        ? amb_key.substr(0, us) : amb_key;
            if (slip_result.reset_satellites.count(sat)) continue;  // segmented
            const int idx = state_manager_->getAmbiguityIndex(amb_key);
            if (idx >= 0 && static_cast<Eigen::Index>(idx) < snap.x_pred.size())
                snap.x_pred(idx) += delta_m;
        }
    }
}

void Application::capturePosteriorSnapshot(const std::set<std::string>& reset_satellites,
                                           bool was_complete_reset,
                                           bool update_applied) {
    if (!config_.runSmoother()) return;
    if (smoother_buffer_.empty()) return;   // guard: prior half wasn't captured

    SmootherEpochSnapshot& snap = smoother_buffer_.back();
    snap.x_post        = state_manager_->getStateVector();
    snap.P_post        = state_manager_->getCovariance();
    snap.has_posterior = update_applied;

    // Core index metadata, read after the update so the indices match x_post.
    snap.pos_idx         = state_manager_->getPositionIndex();
    snap.clock_idx       = state_manager_->getClockIndex();
    snap.tropo_idx       = state_manager_->getTropoIndex();
    snap.velocity_idx    = state_manager_->hasVelocity()
                            ? state_manager_->getVelocityIndex()
                            : std::numeric_limits<size_t>::max();
    snap.phase_clock_idx = state_manager_->hasPhaseClock()
                            ? state_manager_->getPhaseClockIndex()
                            : std::numeric_limits<size_t>::max();
    snap.gradient_n_idx  = state_manager_->hasTropoGradients()
                            ? state_manager_->getTropoGradientNorthIndex()
                            : std::numeric_limits<size_t>::max();
    snap.gradient_e_idx  = state_manager_->hasTropoGradients()
                            ? state_manager_->getTropoGradientEastIndex()
                            : std::numeric_limits<size_t>::max();

    // Dynamic-key maps are copied, not referenced, because the StateManager
    // changes them across epochs.
    snap.ambiguities = state_manager_->getAmbiguities();
    snap.ionospheres = state_manager_->getIonosphereStates();
    snap.iono_rates  = state_manager_->getIonosphereRateStates(); // CV model; empty otherwise
    snap.code_biases = state_manager_->getCodeBiasStates();
    snap.sat_biases  = state_manager_->getSatBiasStates();
    for (const auto& sys : state_manager_->getSystemsWithTimeOffsets()) {
        snap.sys_offsets[sys] = state_manager_->getSystemTimeOffsetIndex(sys);
    }

    snap.reset_satellites = reset_satellites;
    snap.is_discontinuity = was_complete_reset;

    // BSSD frame metadata for the backward pass (audit P2-1).
    // getBssdPivotSatellites is the per-epoch pivot map after all handovers
    // (routine, forced-rebuild and slip-phase); getCleanHandovers reports the
    // undisturbed, losslessly invertible legs.  Both are empty when BSSD is
    // inactive.
    if (meas_model_) {
        snap.pivot_satellites = meas_model_->getBssdPivotSatellites();
        snap.clean_handovers  = meas_model_->getPivotManager().getCleanHandovers();
    }
}

// ---------------------------------------------------------------------------
// Epoch helper: cross-session NEQ accumulation
// ---------------------------------------------------------------------------

void Application::accumulateNormalEquations_(const PppEpochWork& w) {
    const auto& epoch_prefix = w.epoch_prefix;
    const auto& applied_meas = w.applied_meas;
    const auto& have_applied = w.have_applied;
    const auto& epoch_reset_sats = w.epoch_reset_sats;

    // Cross-session NEQ accumulation (item #13B).  Mirrors the accepted
    // update's (H, R, innovation) into a label-keyed normal system; the
    // state labels are captured here so they match applied_meas.H columns
    // (no state resize occurs between the accepted update and this point).
    // Read-only consumer: it does not modify the filter state.
    if (neq_accumulator_ && have_applied) {
        const std::vector<std::string> state_labels = state_manager_->getStateLabels();
        if (static_cast<Eigen::Index>(state_labels.size()) == applied_meas.H.cols()) {
            neq_accumulator_->accumulate(applied_meas, state_labels, epoch_reset_sats);
        } else {
            // Not expected (StateManager only appends and prunes, so labels
            // stay aligned with the H columns).  Logged at warn, not debug,
            // so a broken invariant is visible without debug logging: an
            // unreported per-epoch skip would leave the NEQ incomplete.
            log_manager_.getLogger("main_log")->warn(
                "{}NEQ accumulation skipped: state labels ({}) != H columns ({})",
                epoch_prefix, state_labels.size(), applied_meas.H.cols());
        }
    }
}

// ---------------------------------------------------------------------------
// Epoch helper: the soft-LLI bridge's last-good ambiguity snapshot
// ---------------------------------------------------------------------------

void Application::recordBridgeSnapshot_(const PppEpochWork& w) {
    const auto& current_time = w.current_time;

    // Soft-LLI bridge: update the "last good" ambiguity snapshot from the
    // converged float posterior (before AR fix-and-hold tightens the
    // covariance).  Only converged keys update it, so it holds the pre-gap
    // anchor that a later LLI hold repairs against.  No-op unless
    // turbo_edit_bridge_lli is set.
    if (cycle_slip_manager_)
        cycle_slip_manager_->recordAmbiguitySnapshot(
            *state_manager_, TimeUtils::continuousGpsSeconds(current_time));
}

// ---------------------------------------------------------------------------
// Epoch helper: Log epoch summary
// ---------------------------------------------------------------------------

void Application::logEpochSummary(const std::string& epoch_prefix) {
    const bool is_ppp = (config_.getProcessingMode() == ProcessingMode::PPP);
    const bool phase_clk = config_.estimatePhaseClock();
    const bool has_vel = config_.estimateVelocity();
    const auto& pos = ppp_state_.position;
    const auto& vel = ppp_state_.velocity;

    // Build optional velocity suffix.
    std::string vel_str;
    if (has_vel)
        vel_str = fmt::format(", Vel ({:.3f}, {:.3f}, {:.3f}) m/s", vel.x(), vel.y(), vel.z());

    if (is_ppp) {
        if (phase_clk) {
            log_manager_.getLogger("main_log")->info(
                "{}Position ({:.3f}, {:.3f}, {:.3f}) m, Clock {:.3f} m, PhaseClk {:.3f} m, Tropo {:.3f} m{}, mode=PPP",
                epoch_prefix, pos.x(), pos.y(), pos.z(),
                ppp_state_.clock_bias, ppp_state_.phase_clock_offset, ppp_state_.troposphere_zwd, vel_str);
        } else {
            log_manager_.getLogger("main_log")->info(
                "{}Position ({:.3f}, {:.3f}, {:.3f}) m, Clock {:.3f} m, Tropo {:.3f} m{}, mode=PPP",
                epoch_prefix, pos.x(), pos.y(), pos.z(),
                ppp_state_.clock_bias, ppp_state_.troposphere_zwd, vel_str);
        }
    } else {
        if (phase_clk) {
            log_manager_.getLogger("main_log")->info(
                "{}Position ({:.3f}, {:.3f}, {:.3f}) m, Clock {:.3f} m, PhaseClk {:.3f} m, Tropo {:.3f} m{}, IonoStates {}, mode=PPP-RTK",
                epoch_prefix, pos.x(), pos.y(), pos.z(),
                ppp_state_.clock_bias, ppp_state_.phase_clock_offset, ppp_state_.troposphere_zwd,
                vel_str, ppp_state_.slant_ionosphere.size());
        } else {
            log_manager_.getLogger("main_log")->info(
                "{}Position ({:.3f}, {:.3f}, {:.3f}) m, Clock {:.3f} m, Tropo {:.3f} m{}, IonoStates {}, mode=PPP-RTK",
                epoch_prefix, pos.x(), pos.y(), pos.z(),
                ppp_state_.clock_bias, ppp_state_.troposphere_zwd,
                vel_str, ppp_state_.slant_ionosphere.size());
        }
    }
}

// ---------------------------------------------------------------------------
// Epoch helper: Write CSV trace records
// ---------------------------------------------------------------------------

void Application::writeEpochTraces(const GnssTime& current_time,
                                   const std::vector<RinexObservation>& epoch_obs,
                                   const std::vector<PppObservation>& modeled_obs,
                                   const std::map<std::string, DropReason>& drop_reasons,
                                   const EpochMeasurement& applied_meas,
                                   bool have_applied,
                                   const std::set<std::string>& reset_satellites) {
    // Trace files carry GPS time (week + tow_s in the GPS scale), so the
    // timestamps match the RINEX epoch records.  No leap-second adjustment is
    // applied (the traces are not converted to UTC), and consumers apply
    // none either: trace time = RINEX time = GPS time.
    const GnssTime gps_time = current_time;
    trace_logger_->logState(gps_time, *state_manager_);
    trace_logger_->logAmbiguities(gps_time, *state_manager_);
    trace_logger_->logIonosphere(gps_time, *state_manager_);
    trace_logger_->logSatBiases(gps_time, *state_manager_);
    trace_logger_->logReceiverCodeBiases(gps_time, *state_manager_);
    if (have_applied) {
        trace_logger_->logResiduals(gps_time, applied_meas);
    }

    // Per-satellite epoch status: gather slip set from the CSM's last MW
    // detector results (only sats that slipped are cached in the map) and
    // combine with the KF-reset set passed in by the caller.  Both feed
    // the "Slip" status category in TraceLogger::logSatelliteStatus.  The
    // raw epoch_obs vector enables the logger to surface "BelowMask" rows
    // for sats that were tracked in RINEX but dropped by ModelGeometry.
    std::set<std::string> slipped_sats;
    if (cycle_slip_manager_) {
        for (const auto& [sat, mw_res] : cycle_slip_manager_->getLastMwSlips()) {
            (void)mw_res;
            slipped_sats.insert(sat);
        }
    }
    trace_logger_->logSatelliteStatus(
        gps_time, epoch_obs, modeled_obs, drop_reasons, applied_meas, have_applied,
        *state_manager_, slipped_sats, reset_satellites,
        config_.getElevationMaskDeg());

    // Per-epoch DOPs (PDOP/HDOP/VDOP/TDOP/GDOP) and ambiguity DOP, plus
    // per-system + total used-sat counts.
    trace_logger_->logDops(
        gps_time, modeled_obs, applied_meas, have_applied, *state_manager_);
}

} // namespace GnssProcessor
