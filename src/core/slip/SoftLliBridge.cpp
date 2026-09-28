/**
 * @file SoftLliBridge.cpp
 * @brief Implementation of the shared soft-LLI gap-bridge orchestration.
 */

#include "slip/SoftLliBridge.hpp"
#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <limits>

namespace GnssProcessor {

SoftLliBridge::SoftLliBridge(const GnssConfig& config, LogManager& log_manager)
    : config_(config), log_manager_(log_manager), ppp_utils_(log_manager) {}

std::string SoftLliBridge::signalOf(const std::string& amb_key) {
    const auto p = amb_key.find('_');
    return (p != std::string::npos) ? amb_key.substr(p + 1) : std::string{};
}

bool SoftLliBridge::enabled() const {
    const auto& cs = config_.getCycleSlipConfig();
    return cs.turbo_edit && cs.turbo_edit_bridge_lli && cs.turbo_edit_max_gap_s > 0.0;
}

void SoftLliBridge::recordSnapshot(const StateManager& state_manager,
                                   double epoch_now_s) {
    if (!enabled()) return;
    const Eigen::VectorXd x = state_manager.getStateVector();
    for (const auto& [key, idx] : state_manager.getAmbiguities()) {
        const auto i = static_cast<Eigen::Index>(idx);
        if (i < 0 || i >= x.size()) continue;
        const double var = state_manager.getStateVariance(static_cast<int>(idx));
        // Skip mid-reset and not-yet-converged keys, so a fresh reset (var ~1e6)
        // does not overwrite the last converged snapshot, which is the pre-gap
        // anchor a later soft-LLI hold needs.
        if (!(std::isfinite(var) && var >= 0.0) || var > kSnapshotMaxVariance)
            continue;
        last_good_amb_[key] = AmbSnapshot{ x(i), var, epoch_now_s };
    }
}

std::set<std::string> SoftLliBridge::heldKeys() const {
    std::set<std::string> keys;
    for (const auto& [k, h] : held_pending_bridge_) { (void)h; keys.insert(k); }
    return keys;
}

bool SoftLliBridge::qualifies(const std::string& sat_id, double epoch_now_s) const {
    if (!enabled()) return false;
    // High rate only: the bridge's across-gap GF jump is iono-coherent only at
    // high rate (mirrors TurboEditResolver::kHighRateMaxIntervalS = 1 s).  Use
    // the declared interval when known, else the measured spacing to the
    // previous epoch (SBF/RTCM streams carry no interval header, so declared 0).
    const double measured_dt =
        (prev_epoch_s_ > 0.0 && epoch_now_s > prev_epoch_s_)
            ? (epoch_now_s - prev_epoch_s_) : 0.0;
    const double dt = (sample_interval_s_ > 0.0) ? sample_interval_s_ : measured_dt;
    if (!(dt > 0.0 && dt <= 1.0 + 1e-3)) return false;
    // AR-capable constellation (exclude GLONASS FDMA).
    if (sat_id.empty() || sat_id[0] == 'R') return false;
    return true;
}

void SoftLliBridge::registerHold(const std::string& sat_id,
                                 const std::set<std::string>& amb_keys,
                                 TurboEditResolver& resolver,
                                 double epoch_now_s,
                                 const std::string& epoch_prefix) {
    auto log = log_manager_.getLogger("cycle_slip_manager");
    const double cap = config_.getCycleSlipConfig().turbo_edit_max_gap_s;

    // Hold every phase signal of this satellite that has a recent pre-gap
    // snapshot, in addition to the signals still in the LLI reset set
    // (@p amb_keys).  The signal that slipped is often the one that dropped
    // first (its carrier vanished an epoch before the LLI fired) and is already
    // pruned out of @p amb_keys; unless it is held from the snapshot, the bridge
    // moots it and its integer repair is lost.  The recency gate (snapshot
    // within the bridge cap) excludes long-untracked signals.
    std::set<std::string> candidate_keys = amb_keys;
    const std::string prefix = sat_id + "_";
    for (const auto& [key, snap] : last_good_amb_) {
        if (key.rfind(prefix, 0) != 0) continue;
        if (epoch_now_s > 0.0 && snap.epoch_s > 0.0 &&
            (epoch_now_s - snap.epoch_s) > cap)
            continue;  // stale: sat is not actively tracking this signal
        candidate_keys.insert(key);
    }

    int n_held = 0;
    for (const auto& amb_key : candidate_keys) {
        const std::string signal = signalOf(amb_key);
        if (signal.empty() || signal[0] != 'L') continue;        // phase only
        if (signal.find('_') != std::string::npos) continue;     // raw signal only
        const auto snap = last_good_amb_.find(amb_key);
        if (snap == last_good_amb_.end()) continue;              // no anchor → normal reset
        if (held_pending_bridge_.count(amb_key)) { ++n_held; continue; }  // multi-epoch LLI
        held_pending_bridge_[amb_key] =
            HeldBridge{ sat_id, signal, snap->second.value,
                        snap->second.variance, epoch_now_s };
        resolver.markHeldAcrossLli(sat_id, signal);
        ++n_held;
    }
    if (n_held > 0) {
        log->info("{}Soft-LLI hold for {}: {} phase ambiguit(ies) held across the "
                  "LLI gap (GF arc preserved, measurements excluded; awaiting "
                  "bridge repair, cap {:.1f} s)",
                  epoch_prefix, sat_id, n_held, cap);
    }
}

void SoftLliBridge::registerGapHolds(const std::vector<PppObservation>& modeled_obs,
                                     const StateManager& state_manager,
                                     const IsBssdPivotFn& is_bssd_pivot,
                                     TurboEditResolver& resolver,
                                     double epoch_now_s,
                                     const std::string& epoch_prefix) {
    if (!enabled()) return;
    auto log = log_manager_.getLogger("cycle_slip_manager");
    const double cap = config_.getCycleSlipConfig().turbo_edit_max_gap_s;

    // Raw phase keys ("G03_L2W") that produced an observation this epoch.
    std::set<std::string> observed;
    for (const auto& obs : modeled_obs) {
        for (const auto& [ref_type, observed_val] : obs.observed) {
            (void)observed_val;
            if (ref_type.empty() || ref_type[0] != 'L') continue;
            if (ref_type.find('_') != std::string::npos) continue;
            observed.insert(obs.satellite + "_" + ref_type);
        }
    }

    for (const auto& [amb_key, idx] : state_manager.getAmbiguities()) {
        (void)idx;
        const std::string signal = signalOf(amb_key);
        if (signal.empty() || signal[0] != 'L') continue;        // phase only
        if (signal.find('_') != std::string::npos) continue;     // raw signal only
        if (observed.count(amb_key)) continue;                   // still tracked
        if (held_pending_bridge_.count(amb_key)) continue;       // already held
        const auto snap = last_good_amb_.find(amb_key);
        if (snap == last_good_amb_.end()) continue;              // never converged
        if (epoch_now_s > 0.0 && snap->second.epoch_s > 0.0 &&
            (epoch_now_s - snap->second.epoch_s) > cap)
            continue;  // stale (long-untracked / unconverged): normal prune
        const std::string sat = amb_key.substr(0, amb_key.find('_'));
        if (!qualifies(sat, epoch_now_s)) continue;
        if (is_bssd_pivot && is_bssd_pivot(sat)) continue;
        held_pending_bridge_[amb_key] =
            HeldBridge{ sat, signal, snap->second.value,
                        snap->second.variance, epoch_now_s };
        resolver.markHeldAcrossLli(sat, signal);
        log->info("{}Gap hold for {} {}: phase signal dropped with a recent "
                  "converged snapshot — ambiguity held across the outage "
                  "(prune-protected, measurements excluded) awaiting bridge "
                  "adjudication (cap {:.1f} s)",
                  epoch_prefix, sat, signal, cap);
    }
}

void SoftLliBridge::resolveHolds(const TurboEditResolver::EpochResolution& te,
                                 TurboEditResolver& resolver,
                                 const std::vector<PppObservation>& modeled_obs,
                                 StateManager& state_manager,
                                 double epoch_now_s,
                                 const std::string& epoch_prefix,
                                 std::set<std::string>& repaired_sats,
                                 int& n_repaired,
                                 int& n_reset,
                                 const ResetFn& reset_fn) {
    if (held_pending_bridge_.empty()) return;
    auto log = log_manager_.getLogger("cycle_slip_manager");
    const double cap = config_.getCycleSlipConfig().turbo_edit_max_gap_s;
    const double reset_var = config_.getAmbiguityStd() * config_.getAmbiguityStd();

    // This epoch's bridge repairs grouped by satellite.
    std::map<std::string, std::vector<const TurboEditResolver::SignalRepair*>> reps_by_sat;
    for (const auto& r : te.repairs) reps_by_sat[r.satellite].push_back(&r);

    // Held keys grouped by satellite (adjudication is per satellite; the
    // timeout below is per key, from each hold's own start).
    std::map<std::string, std::vector<std::string>> keys_by_sat;
    for (const auto& [k, h] : held_pending_bridge_) {
        keys_by_sat[h.sat].push_back(k);
    }

    // Overwrite an ambiguity with (anchor + dN·λ) at the snapshot variance,
    // cross-covariances zeroed; the variance falls back to the reset variance
    // when the snapshot's is unusable and is floored at 1e-6.  The anchor is
    // the held snapshot when present; for a signal that dropped before the LLI
    // reset captured it (never formally held) the rolling last_good_amb_
    // snapshot is used.  This reconstructs the satellite's current integer
    // ambiguity from its pre-gap value plus the bridge-resolved carrier change
    // (valid for a slip with lock held and for a re-acquisition, since dN
    // absorbs either) at the pre-gap precision instead of a loose reset.
    auto applyRestore = [&](const std::string& amb_key, const std::string& sat,
                            const std::string& sig, int dN, double lambda) -> bool {
        const int idx = state_manager.getAmbiguityIndex(amb_key);
        if (idx < 0) return false;
        double anchor = std::numeric_limits<double>::quiet_NaN();
        double anchor_var = std::numeric_limits<double>::quiet_NaN();
        if (auto hit = held_pending_bridge_.find(amb_key); hit != held_pending_bridge_.end()) {
            anchor = hit->second.snap_value; anchor_var = hit->second.snap_variance;
        } else if (auto sit = last_good_amb_.find(amb_key); sit != last_good_amb_.end()) {
            anchor = sit->second.value; anchor_var = sit->second.variance;
        }
        if (!std::isfinite(anchor)) return false;
        if (!(lambda > 0.0) && dN != 0) {
            for (const auto& o : modeled_obs)
                if (o.satellite == sat) {
                    lambda = ppp_utils_.getWavelength(o.system, sig, sat);
                    break;
                }
            if (!(lambda > 0.0)) return false;  // cannot apply a nonzero integer
        }
        const double val = anchor + static_cast<double>(dN) * (lambda > 0.0 ? lambda : 0.0);
        const auto i = static_cast<Eigen::Index>(idx);
        Eigen::VectorXd x = state_manager.getStateVector();
        const double prev = x(i);
        x(i) = val;
        state_manager.setStateVector(x);
        Eigen::MatrixXd P = state_manager.getCovariance();
        P.row(i).setZero();
        P.col(i).setZero();
        double var = (std::isfinite(anchor_var) && anchor_var > 0.0) ? anchor_var : reset_var;
        if (var < 1e-6) var = 1e-6;  // numeric floor
        P(i, i) = var;
        state_manager.setCovariance(P);
        state_manager.noteAmbiguityDiscontinuity(amb_key);
        log->info("{}Soft-LLI bridge REPAIR {}: anchor {:.4f} + dN={:+d}·λ({:.4f}) "
                  "= {:.4f} m (was {:.4f}; var={:.3e})",
                  epoch_prefix, amb_key, anchor, dN, lambda, val, prev, var);
        return true;
    };

    std::vector<std::string> erase_keys;
    for (const auto& [sat, keys] : keys_by_sat) {
        const bool sat_reset = te.reset_sats.count(sat) > 0;
        auto rit = reps_by_sat.find(sat);
        const bool sat_repaired = (rit != reps_by_sat.end() && !rit->second.empty());

        if (sat_reset) {
            log->info("{}Soft-LLI hold released for {} (bridge resolved to reset)",
                      epoch_prefix, sat);
            erase_keys.insert(erase_keys.end(), keys.begin(), keys.end());
            continue;
        }
        if (sat_repaired) {
            // A restore fails only abnormally: the held state vanished (a
            // prune-protection gap), both anchors are gone, or a nonzero dN has
            // no wavelength.  Erasing the hold alone would leave the ambiguity
            // at its loose or freshly seeded value, neither repaired nor reset,
            // so a failure logs a warning and falls back to the normal reset.
            auto restoreOrReset = [&](const std::string& amb_key,
                                      const std::string& sig,
                                      int dN, double lambda) -> bool {
                if (applyRestore(amb_key, sat, sig, dN, lambda)) return true;
                log->warn("{}Soft-LLI bridge restore FAILED for {} (state "
                          "index or anchor missing) — falling back to a fresh "
                          "reset",
                          epoch_prefix, amb_key);
                if (reset_fn) reset_fn(amb_key, sat, sig);
                ++n_reset;
                return false;
            };
            // Signals the repair loop already handled (restored or reset by
            // the fallback).  The dN = 0 partner loop skips them, so a failed
            // restore is not reset twice.
            std::set<std::string> actioned_sigs;
            bool any = false;
            for (const auto* r : rit->second) {
                const std::string rk = r->satellite + "_" + r->signal;
                actioned_sigs.insert(r->signal);
                if (restoreOrReset(rk, r->signal, r->dN, r->lambda))
                    any = true;
            }
            // A held signal the bridge did not flag this epoch did not slip
            // (no GF/MW exceedance): restore its converged pre-gap value
            // (dN = 0) instead of leaving it at the loose LLI reset.
            for (const auto& k : keys) {
                const std::string sig = signalOf(k);
                if (actioned_sigs.count(sig)) continue;
                restoreOrReset(k, sig, /*dN=*/0, /*lambda=*/0.0);
            }
            if (any) {
                repaired_sats.insert(sat);
                ++n_repaired;
            }
            erase_keys.insert(erase_keys.end(), keys.begin(), keys.end());
            continue;
        }
        // A WL-settle deferral extends the hold past the base cap: the
        // resolver is waiting for the post-restart MW mean to settle and
        // owns the timing (it emits repairs or reset_sats).  The backstop
        // of cap plus settle window bounds a hold whose pending flag ends
        // without an adjudication (e.g. an external arc erase), so no hold
        // stays open indefinitely.
        const double settle_max =
            config_.getCycleSlipConfig().turbo_edit_wl_settle_max_s;
        const double cap_eff =
            te.pending_sats.count(sat) ? cap + settle_max : cap;
        // Per-key timeout: each held signal gets the full cap from its own
        // hold start, so a signal that drops late into an existing outage
        // does not inherit the group's nearly spent timer.  An expired key
        // takes the path-specific reset and has its resolver state voided
        // (the held-continuous assertion and the GF arcs touching the
        // signal): its integer continuity is broken, so a later bridged
        // adjudication must not treat the signal as continuous.  Keys within
        // their window stay held (measurements stay excluded).
        std::string expired_sigs;
        double max_elapsed = 0.0;
        std::size_t n_expired = 0;
        for (const auto& k : keys) {
            const double start = held_pending_bridge_.at(k).hold_start_s;
            const double elapsed =
                (epoch_now_s > start && start > 0.0) ? (epoch_now_s - start) : 0.0;
            if (elapsed <= cap_eff) continue;
            const std::string sig = signalOf(k);
            if (reset_fn) reset_fn(k, sat, sig);
            resolver.resetSatelliteSignal(sat, sig);
            ++n_reset;
            ++n_expired;
            max_elapsed = std::max(max_elapsed, elapsed);
            if (!expired_sigs.empty()) expired_sigs += ",";
            expired_sigs += sig;
            erase_keys.push_back(k);
        }
        if (n_expired > 0) {
            log->info("{}Soft-LLI hold TIMED OUT for {} {} after {:.1f} s "
                      "(> {:.1f} s cap) — fresh reset, hold released "
                      "({} key(s) still held)",
                      epoch_prefix, sat, expired_sigs, max_elapsed, cap_eff,
                      keys.size() - n_expired);
        }
    }
    for (const auto& k : erase_keys) held_pending_bridge_.erase(k);
}

void SoftLliBridge::reset() {
    last_good_amb_.clear();
    held_pending_bridge_.clear();
    prev_epoch_s_ = 0.0;
}

} // namespace GnssProcessor
