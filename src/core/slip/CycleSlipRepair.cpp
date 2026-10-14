/**
 * @file CycleSlipRepair.cpp
 * @brief The two state primitives every slip response goes through: the
 *        BSSD-aware ambiguity reset and the covariance-preserving integer
 *        repair.
 *
 * Moved verbatim from CycleSlipManager.cpp (card UOM step U3).  Both write the
 * filter state directly and record the epoch's smoother actions in
 * @c epoch_reset_sats_ (reset) and @c epoch_repairs_ (repair).
 */

#include "slip/CycleSlipManager.hpp"
#include <cmath>

namespace GnssProcessor {

// ===========================================================================
// BSSD-aware ambiguity reset
// ===========================================================================

void CycleSlipManager::resetAmbiguityBssdAware(
    const std::string& amb_key,
    const std::string& sat_id,
    const std::string& signal,
    const std::vector<PppObservation>& modeled_obs,
    StateManager& state_manager,
    const MeasurementModel& meas_model,
    const std::string& epoch_prefix) {

    const double reset_std_m = config_.getAmbiguityStd();

    // Smoother segmentation: every branch below re-seeds the value with a large
    // variance, inflates the pivot variance, or defers the reseed.  None carries
    // information backward, so the satellite is recorded for the smoother to
    // exclude at this epoch's boundary (audit F1).
    epoch_reset_sats_.insert(sat_id);
    ++counters_.ambiguity_resets;

    if (!config_.enableBetweenSatelliteSd()) {
        state_manager.resetAmbiguity(amb_key, reset_std_m);
        return;
    }

    const auto& pivots = meas_model.getBssdPivotSatellites();
    const std::string cons = PppUtils::getSystemString(sat_id);
    auto piv_it = pivots.find(cons);

    if (piv_it == pivots.end()) {
        state_manager.resetAmbiguity(amb_key, reset_std_m);
        return;
    }

    const std::string& pivot_sat = piv_it->second;
    auto log = log_manager_.getLogger("cycle_slip_manager");

    // Pivot satellite: inflate the frozen variance so that the downstream
    // PivotManager::handlePivotHandover() detects the disturbance (via
    // P > 1e-12) and re-references to a clean satellite.  The state value is
    // left unchanged (zero for the pivot).  A non-zero value would propagate
    // through the T·P·T' re-referencing into every SD ambiguity of the
    // constellation, producing large innovations and repeated outlier-rebuild
    // passes.
    if (sat_id == pivot_sat) {
        const int amb_idx = state_manager.getAmbiguityIndex(amb_key);
        if (amb_idx >= 0) {
            Eigen::MatrixXd P = state_manager.getCovariance();
            const auto aidx = static_cast<Eigen::Index>(amb_idx);
            P(aidx, aidx) = reset_std_m * reset_std_m;
            state_manager.setCovariance(P);
        }
        log->info("{}BSSD: pivot {} slipped on {} -- variance inflated to trigger handover",
                  epoch_prefix, sat_id, signal);
        return;
    }

    // Non-pivot: compute SD seed.
    const int amb_idx = state_manager.getAmbiguityIndex(amb_key);
    if (amb_idx < 0) {
        state_manager.resetAmbiguity(amb_key, reset_std_m);
        return;
    }

    const Eigen::VectorXd x = state_manager.getStateVector();

    const PppObservation* obs_sat = nullptr;
    const PppObservation* obs_piv = nullptr;
    for (const auto& obs : modeled_obs) {
        if (obs.satellite == sat_id)    obs_sat = &obs;
        if (obs.satellite == pivot_sat) obs_piv = &obs;
        if (obs_sat && obs_piv) break;
    }

    // Reset the covariance unconditionally: the slip invalidates the float
    // value, and a large variance lets the filter take up new data quickly.
    Eigen::MatrixXd P = state_manager.getCovariance();
    const auto aidx = static_cast<Eigen::Index>(amb_idx);
    const double old_val = x(aidx);
    P.row(aidx).setZero();
    P.col(aidx).setZero();
    P(aidx, aidx) = reset_std_m * reset_std_m;

    // Re-seed the float value only when the satellite and the pivot are both
    // observed this epoch.  In BSSD mode the state stores N_sat - N_pivot;
    // seeding with the absolute N_sat (what computeAmbiguitySeed returns for a
    // single obs) is off by N_pivot, possibly many cycles, and the filter
    // needs many epochs to remove that offset, often rejecting good
    // observations as outliers meanwhile.  When the pivot obs is missing
    // (below the mask, or lost), the reseed is deferred to the next epoch via
    // markAmbiguityPendingReseed; ensureRequiredStates() performs it there
    // with both obs available.
    if (obs_sat && obs_piv) {
        // The pivot may use a different tracking attribute on the same
        // frequency band (e.g. L1W vs L1C).  Find the pivot's actual
        // signal on the same band for a correct absolute seed.
        std::string piv_signal = signal;  // default: same signal
        if (!obs_piv->observed.count(signal) && signal.size() >= 2) {
            const std::string band = signal.substr(0, 2);
            for (const auto& [sig, val] : obs_piv->observed) {
                if (sig.size() >= 2 && sig.substr(0, 2) == band) {
                    piv_signal = sig;
                    break;
                }
            }
        }
        const double sd_seed =
            state_manager.computeAmbiguitySeed(*obs_sat, signal, x)
          - state_manager.computeAmbiguitySeed(*obs_piv, piv_signal, x);

        Eigen::VectorXd x_mod = x;
        x_mod(aidx) = sd_seed;
        state_manager.setStateVector(x_mod);
        state_manager.setCovariance(P);

        log->info("{}BSSD SD-aware reset {}: {:.4f} -> {:.4f} (pivot={})",
                  epoch_prefix, amb_key, old_val, sd_seed, pivot_sat);
    } else {
        // Pivot obs (and possibly the satellite's own obs) missing: inflate
        // the variance, leave the state value, and let the next epoch reseed.
        state_manager.setCovariance(P);
        state_manager.markAmbiguityPendingReseed(amb_key);

        log->info("{}BSSD SD-aware reset {}: pivot {} obs missing this epoch, "
                  "variance inflated, reseed deferred to next epoch (old={:.4f})",
                  epoch_prefix, amb_key, pivot_sat, old_val);
    }
}

// ===========================================================================
// Cycle slip repair: additive integer adjustment, covariance preserved
// ===========================================================================

bool CycleSlipManager::repairAmbiguityBssdAware(
    const std::string& sat_id,
    const std::string& signal,
    int slip_cycles,
    const std::vector<PppObservation>& modeled_obs,
    StateManager& state_manager,
    const MeasurementModel& meas_model,
    const std::string& epoch_prefix) {

    auto log = log_manager_.getLogger("cycle_slip_manager");
    if (slip_cycles == 0) return false;

    // BSSD pivot: refuse repair.  The pivot's SD ambiguity is frozen at
    // zero by construction; integer adjustment makes no sense.  Pivot
    // slips are handled by the post-fit residual detector forcing a
    // handover, which losslessly re-references the entire SD frame.
    if (config_.enableBetweenSatelliteSd()) {
        const auto& pivots = meas_model.getBssdPivotSatellites();
        const std::string cons = PppUtils::getSystemString(sat_id);
        auto piv_it = pivots.find(cons);
        if (piv_it != pivots.end() && piv_it->second == sat_id) {
            log->info("{}MW slip repair refused: {} {} is the BSSD pivot; "
                      "pivot handover handles this case.",
                      epoch_prefix, sat_id, signal);
            return false;
        }
    }

    const std::string amb_key = sat_id + "_" + signal;
    const int amb_idx = state_manager.getAmbiguityIndex(amb_key);
    if (amb_idx < 0) {
        log->info("{}MW slip repair skipped: no ambiguity state for {}",
                  epoch_prefix, amb_key);
        return false;
    }

    // Find the current observation for this satellite to recover the
    // signal wavelength.  If the satellite is not observed this epoch,
    // λ_signal is unknown and the caller falls back to reset.
    const PppObservation* obs_for_sat = nullptr;
    for (const auto& obs : modeled_obs) {
        if (obs.satellite == sat_id) { obs_for_sat = &obs; break; }
    }
    if (!obs_for_sat) {
        log->info("{}MW slip repair skipped: no observation for {} this epoch",
                  epoch_prefix, sat_id);
        return false;
    }
    const double lambda_sig = ppp_utils_.getWavelength(
        obs_for_sat->system, signal, sat_id);
    if (!(std::isfinite(lambda_sig) && lambda_sig > 0.0)) {
        log->info("{}MW slip repair skipped: invalid wavelength for {} {}",
                  epoch_prefix, sat_id, signal);
        return false;
    }

    // Apply the additive integer adjustment.  The covariance is preserved on
    // purpose: repair, unlike reset, keeps the converged precision of the
    // ambiguity.
    Eigen::VectorXd x = state_manager.getStateVector();
    const double delta_m = static_cast<double>(slip_cycles) * lambda_sig;
    const double prev    = x(amb_idx);
    x(amb_idx) += delta_m;
    state_manager.setStateVector(x);

    // Smoother compensation: this is a deterministic, covariance-preserving
    // state jump applied in the pre-update frame in which the smoother
    // captured x_pred, so it can be folded into the a-priori as a control
    // input instead of being absorbed as an innovation (audit F1).  Recorded
    // only when the repair is applied; a refused or skipped repair returns
    // earlier.
    epoch_repairs_.emplace_back(amb_key, delta_m);

    log->info("{}MW slip REPAIR for {}: dN={:+d} cyc, lambda={:.4f} m, "
              "delta={:+.4f} m, x: {:.4f} -> {:.4f} (covariance preserved)",
              epoch_prefix, amb_key, slip_cycles, lambda_sig, delta_m,
              prev, x(amb_idx));

    return true;
}

} // namespace GnssProcessor
