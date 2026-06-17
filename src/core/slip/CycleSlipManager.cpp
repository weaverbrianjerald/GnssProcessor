/**
/**
* @file CycleSlipManager.cpp
* @brief Unified MW-only cycle slip detection and reset coordinator.
*
* Coordinates MW slip detection with multi-frequency cross-consistency
* analysis and targeted ambiguity resets.
*/

#include "slip/CycleSlipManager.hpp"
#include "slip/SlipDetectorRegistry.hpp"
#include "math/TimeUtils.hpp"
#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace GnssProcessor {

// Helper: extract satellite ID from an ambiguity key "G01_L1C" -> "G01"
static std::string satIdFromAmbiguityKey(const std::string& key) {
    auto pos = key.find('_');
    return (pos != std::string::npos) ? key.substr(0, pos) : key;
}

// Helper: extract signal from an ambiguity key "G01_L1C" -> "L1C"
static std::string signalFromAmbiguityKey(const std::string& key) {
    auto pos = key.find('_');
    return (pos != std::string::npos) ? key.substr(pos + 1) : "";
}

// ===========================================================================
// Construction
// ===========================================================================

CycleSlipManager::CycleSlipManager(const GnssConfig& config, LogManager& log_manager)
    : config_(config),
      log_manager_(log_manager),
      ppp_utils_(log_manager),
      soft_lli_(config, log_manager) {
    // Build the config-selected detector set (MW-first, then auxiliaries).
    detectors_ = SlipDetectorRegistry::createEnabled(config, log_manager);
    // Recover a typed handle to the MW detector for its rich API (WL locks,
    // quality scoring, pivot hints) — nullptr if MW is disabled in config.
    for (const auto& d : detectors_) {
        if (auto* mw = d->asMelbourneWubbena()) { mw_ = mw; break; }
    }

    // TurboEdit MW+GF joint repair: requires the MW detector (its source of
    // wide-lane integers).  Build the resolver only when both the toggle and
    // the MW detector are active; otherwise warn and stay in the MW-only path.
    const auto& cs = config.getCycleSlipConfig();
    if (cs.turbo_edit) {
        if (mw_) {
            turbo_ = std::make_unique<TurboEditResolver>(config, log_manager);
            log_manager.getLogger("cycle_slip_manager")->info(
                "TurboEdit MW+GF joint cycle-slip repair ENABLED (high-rate only).");
        } else {
            log_manager.getLogger("cycle_slip_manager")->warn(
                "turbo_edit requested but the MW detector (estimation.cycle_slip.mw) "
                "is disabled — TurboEdit needs the MW wide-lane integers; staying "
                "MW-only (TurboEdit OFF).");
        }
    }
}

// ===========================================================================
// Public API
// ===========================================================================

std::map<std::string, int> CycleSlipManager::getResolvedWlAmbiguities() const {
    return mw_ ? mw_->getResolvedWlAmbiguities() : std::map<std::string, int>{};
}

std::map<std::string, double> CycleSlipManager::getResolvedWlFractions() const {
    return mw_ ? mw_->getResolvedWlFractions() : std::map<std::string, double>{};
}

void CycleSlipManager::resetSatellite(const std::string& satellite) {
    // Reset the arc state in every detector so an externally-driven reset
    // (e.g. the KF outlier handler) clears all of them, not just MW.
    for (const auto& d : detectors_) d->resetSatellite(satellite);
    // The TurboEdit resolver holds its own GF jump arcs (it is not an
    // ISlipDetector in detectors_), so clear them too — otherwise a stale
    // pre-reset GF value would corrupt the next time-difference.
    if (turbo_) turbo_->resetSatellite(satellite);
}

void CycleSlipManager::resetSatelliteSignal(const std::string& satellite,
                                              const std::string& signal) {
    for (const auto& d : detectors_) d->resetSatelliteSignal(satellite, signal);
    if (turbo_) turbo_->resetSatelliteSignal(satellite, signal);
}

void CycleSlipManager::notifyPivotForcedHandover(const std::string& satellite,
                                                  const std::string& reason) {
    if (mw_) mw_->notifyPivotForcedHandover(satellite, reason);
}

void CycleSlipManager::setCorrections(const GnssCorrections* corrections) {
    // MW and EWL consume biases; the others ignore it via the default no-op.
    for (const auto& d : detectors_) d->setCorrections(corrections);
    if (turbo_) turbo_->setCorrections(corrections);
}

bool CycleSlipManager::enableMwTracing(const std::filesystem::path& log_dir) {
    return mw_ ? mw_->enableTracing(log_dir) : false;
}

bool CycleSlipManager::enableTurboEditTracing(const std::filesystem::path& log_dir) {
    return turbo_ ? turbo_->enableTracing(log_dir) : false;
}

void CycleSlipManager::setSampleInterval(double interval_s) {
    sample_interval_s_ = interval_s;
    soft_lli_.setSampleInterval(interval_s);
    // Tighten the TurboEdit GF-arc gap tolerance to ~1.5× the cadence so a
    // brief outage restarts the arc rather than absorbing a slip-during-gap
    // into a stale GF reference (mirrors the MW detector's gap handling).
    if (turbo_ && interval_s > 0.0)
        turbo_->setMaxGapSeconds(interval_s * 1.5);
}

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

    // Smoother segmentation: every branch below either re-seeds the value with
    // a large variance, inflates the pivot variance, or defers the reseed — none
    // carry information backward, so record the satellite for the smoother to
    // exclude this epoch's boundary (audit F1).
    epoch_reset_sats_.insert(sat_id);

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
    // BssdPivotManager::handlePivotHandover() detects the disturbance
    // (via P > 1e-12) and performs a re-referencing to a clean satellite.
    // The state value is left at 0.0  setting it to a non-zero poison
    // value would propagate through the T·P·T' re-referencing and
    // corrupt all SD ambiguities in the constellation, causing a cascade
    // of large innovations and dozens of outlier-rebuild passes.
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

    // Reset the covariance unconditionally -- the float value is corrupted
    // by the slip and we want the filter to absorb new info quickly.
    Eigen::MatrixXd P = state_manager.getCovariance();
    const auto aidx = static_cast<Eigen::Index>(amb_idx);
    const double old_val = x(aidx);
    P.row(aidx).setZero();
    P.col(aidx).setZero();
    P(aidx, aidx) = reset_std_m * reset_std_m;

    // Only re-seed the float value when BOTH observations are available
    // this epoch.  In BSSD mode the state stores N_sat - N_pivot; seeding
    // with the absolute N_sat (which is what computeAmbiguitySeed returns
    // for a single obs) is off by N_pivot -- potentially many cycles -- and
    // the filter takes many epochs to drag it back, often triggering
    // spurious outlier rejections en route.  When the pivot obs is missing
    // (dropped below mask, or just lost), defer the reseed to the next
    // epoch via markAmbiguityPendingReseed; ensureRequiredStates() picks
    // it up there with both obs in hand.
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
        // Pivot obs (and possibly self obs) missing -- inflate variance,
        // leave the state value alone, and let the next epoch reseed.
        state_manager.setCovariance(P);
        state_manager.markAmbiguityPendingReseed(amb_key);

        log->info("{}BSSD SD-aware reset {}: pivot {} obs missing this epoch, "
                  "variance inflated, reseed deferred to next epoch (old={:.4f})",
                  epoch_prefix, amb_key, pivot_sat, old_val);
    }
}

// ===========================================================================
// Cycle slip REPAIR — additive integer adjustment, covariance preserved
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
    // signal wavelength.  If the sat isn't observed this epoch we can't
    // resolve λ_signal — fall back to reset.
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

    // Apply the additive integer adjustment.  Covariance is intentionally
    // preserved: the entire point of repair (vs reset) is to keep the
    // converged precision of the ambiguity.
    Eigen::VectorXd x = state_manager.getStateVector();
    const double delta_m = static_cast<double>(slip_cycles) * lambda_sig;
    const double prev    = x(amb_idx);
    x(amb_idx) += delta_m;
    state_manager.setStateVector(x);

    // Smoother compensation: this is a deterministic, covariance-preserving
    // state jump applied in the SAME (pre-update) frame the smoother captured
    // x_pred in, so it can be folded into the a-priori as a control input
    // rather than absorbed as an innovation (audit F1).  Recorded only on the
    // confirmed-applied path — a refused / skipped repair returns earlier.
    epoch_repairs_.emplace_back(amb_key, delta_m);

    log->info("{}MW slip REPAIR for {}: dN={:+d} cyc, lambda={:.4f} m, "
              "delta={:+.4f} m, x: {:.4f} -> {:.4f} (covariance preserved)",
              epoch_prefix, amb_key, slip_cycles, lambda_sig, delta_m,
              prev, x(amb_idx));

    return true;
}

std::map<std::string, int> CycleSlipManager::computeCulpritSlipIntegers(
    const std::vector<MwSlipResult>& slipped_pairs,
    const std::set<std::string>& culprits) const {

    auto log = log_manager_.getLogger("cycle_slip_manager");

    // For each culprit signal, gather slip integer estimates from the
    // slipped pairs in which the culprit is the SOLE culprit signal.
    // Sign convention: ΔN_WL = ΔN_sig1 − ΔN_sig2.  If sig1 is the lone
    // culprit, ΔN_sig1 = ΔN_WL (since ΔN_sig2 = 0).  If sig2 is the
    // lone culprit, ΔN_sig2 = −ΔN_WL.
    std::map<std::string, std::vector<int>> per_culprit_candidates;

    for (const auto& sr : slipped_pairs) {
        if (!sr.slip_wl_known || sr.slip_wl_cycles == 0) continue;
        if (sr.sig1.empty() || sr.sig2.empty()) continue;

        const bool sig1_culprit = culprits.count(sr.sig1) > 0;
        const bool sig2_culprit = culprits.count(sr.sig2) > 0;

        if (sig1_culprit && !sig2_culprit) {
            per_culprit_candidates[sr.sig1].push_back(sr.slip_wl_cycles);
        } else if (sig2_culprit && !sig1_culprit) {
            per_culprit_candidates[sr.sig2].push_back(-sr.slip_wl_cycles);
        }
        // Both or neither culprit in this pair → cannot attribute the
        // WL slip to a single signal; skip this pair as a contributor.
    }

    // A culprit's slip integer must be consistent across every pair that
    // contributes evidence.  Inconsistency means the cross-consistency
    // analysis or the slip-integer rounding is wrong somewhere — safer
    // to disqualify and fall back to reset for that signal.
    std::map<std::string, int> result;
    for (const auto& [signal, slips] : per_culprit_candidates) {
        if (slips.empty()) continue;
        const int first = slips.front();
        const bool consistent = std::all_of(
            slips.begin(), slips.end(),
            [first](int s){ return s == first; });
        if (consistent) {
            result[signal] = first;
        } else {
            std::string sample;
            for (int s : slips) {
                if (!sample.empty()) sample += ",";
                sample += std::to_string(s);
            }
            log->info("Per-culprit slip integer inconsistent for signal {}: "
                      "candidates [{}]; disqualifying repair, will reset.",
                      signal, sample);
        }
    }

    return result;
}

// ===========================================================================
// Multi-frequency MW cross-consistency analysis
// ===========================================================================

std::set<std::string> CycleSlipManager::identifySlippedSignals(
    const std::vector<MwSlipResult>& sat_slips,
    const std::vector<MwSlipResult>& all_results) const {

    // Collect signals that appear in slipped pairs and in clean pairs.
    std::set<std::string> slipped_pair_signals;
    std::set<std::string> clean_pair_signals;

    // Build set of slipped pair keys for matching at the pair level
    // (not amb_key level, since uncombined pairs emit two amb_keys per pair).
    std::set<std::string> slipped_pair_keys;
    for (const auto& sr : sat_slips) {
        if (!sr.sig1.empty() && !sr.sig2.empty())
            slipped_pair_keys.insert(sr.sig1 + "_" + sr.sig2);
    }

    for (const auto& sr : all_results) {
        if (sr.sig1.empty() || sr.sig2.empty()) continue;
        const std::string pk = sr.sig1 + "_" + sr.sig2;
        if (slipped_pair_keys.count(pk) > 0) {
            // This pair slipped
            slipped_pair_signals.insert(sr.sig1);
            slipped_pair_signals.insert(sr.sig2);
        } else {
            // This pair is clean
            clean_pair_signals.insert(sr.sig1);
            clean_pair_signals.insert(sr.sig2);
        }
    }

    if (clean_pair_signals.empty()) {
        // All pairs slipped or we have no clean reference  inconclusive
        return {};
    }

    // Signals in slipped pairs but NOT in any clean pair are the culprits.
    // Example: L1W_L2W slipped, L1W_L5Q clean.
    //   slipped_pair_signals = {L1W, L2W}
    //   clean_pair_signals = {L1W, L5Q}
    //   result = {L2W}  (L1W appears in a clean pair, so it's OK)
    std::set<std::string> culprits;
    for (const auto& sig : slipped_pair_signals) {
        if (clean_pair_signals.count(sig) == 0)
            culprits.insert(sig);
    }

    return culprits;
}

// ===========================================================================
// Pre-estimation: MW detection and reset
// ===========================================================================

CycleSlipEpochResult CycleSlipManager::detectAndRepair(
    const std::string& epoch_prefix,
    const std::vector<PppObservation>& modeled_obs,
    StateManager& state_manager,
    const MeasurementModel& meas_model) {

    auto log = log_manager_.getLogger("cycle_slip_manager");
    CycleSlipEpochResult result;
    last_mw_slips_.clear();
    // Per-epoch smoother-action accumulators (see CycleSlipEpochResult docs).
    epoch_reset_sats_.clear();
    epoch_repairs_.clear();

    // Continuous GPS seconds of this epoch (for soft-LLI hold bookkeeping).
    double epoch_now_s = 0.0;
    for (const auto& obs : modeled_obs)
        if (!obs.satellite.empty()) {
            epoch_now_s = TimeUtils::continuousGpsSeconds(obs.time);
            break;
        }

    // BSSD pivot predicate, shared by the gap-hold pre-pass and the
    // WL-settle pending-hold registration below (a pivot's SD ambiguity is
    // frozen at 0 — holds and in-frame integer repairs are meaningless).
    const SoftLliBridge::IsBssdPivotFn is_bssd_pivot =
        [&](const std::string& sat) -> bool {
            if (!config_.enableBetweenSatelliteSd()) return false;
            const auto& pivots = meas_model.getBssdPivotSatellites();
            const auto pit = pivots.find(PppUtils::getSystemString(sat));
            return pit != pivots.end() && pit->second == sat;
        };

    // --- Gap-hold registration (pre-pass) ----------------------------------
    // A phase signal that DROPPED this epoch (no observation, no LLI) with a
    // recent converged snapshot is held rather than left to the measurement
    // model's prune-on-first-miss: the held key is prune-protected and
    // KF-excluded (via getHeldPendingBridgeKeys) until the TurboEdit gap
    // bridge adjudicates the outage on the signal's return — repair
    // (snapshot+dN·λ in resolveHolds below), reset, or timeout.  Must run
    // before turbo_->resolve() so the resolver sees the signal as
    // held-continuous from the first missing epoch.  No-op unless
    // turbo_edit_bridge_lli.
    if (turbo_ && soft_lli_.enabled()) {
        soft_lli_.registerGapHolds(modeled_obs, state_manager, is_bssd_pivot,
                                   *turbo_, epoch_now_s, epoch_prefix);
    }

    // Run the auxiliary detectors through the polymorphic contract and gather
    // their unified SlipEvents (applied as BSSD-aware resets after the MW
    // pass).  The MW detector is consumed below via its concrete
    // detectLargeMwResets: its specialized BSSD-aware pipeline needs the full
    // MwSlipResult WL-pair semantics that the unified SlipEvent projection
    // does not preserve, so calling detect() on it would only build a
    // projection the manager discards.
    SlipEpochContext slip_ctx(modeled_obs);
    std::vector<SlipEvent> aux_events;
    for (const auto& d : detectors_) {
        if (d->kind() == SlipDetectorKind::MelbourneWubbena) continue;
        std::vector<SlipEvent> ev = d->detect(slip_ctx);
        aux_events.insert(aux_events.end(),
                          std::make_move_iterator(ev.begin()),
                          std::make_move_iterator(ev.end()));
    }

    // --- MW / LLI detection (all dual-frequency pairs including EWL) ---
    std::set<std::string> reset_sats;
    // Satellites whose soft-LLI-held ambiguities were integer-repaired by the
    // bridge this epoch (snapshot+dN·λ restore in the pre-pass below).  The MW
    // per-sat reset loop skips these — their slip is already handled.
    std::set<std::string> soft_lli_repaired;

    // TurboEdit (MW+GF joint repair) resolution for this epoch.  Declared at
    // function scope because it is consumed in two places: the MW per-satellite
    // loop (where it supplies per-signal integers for the dual-frequency case
    // cross-consistency cannot split) and the GF-only pass after the loop (the
    // MW dN1==dN2 blind spot).  resolve() also rolls the GF jump arcs forward,
    // so it must run every epoch — even when there are no MW slips.
    TurboEditResolver::EpochResolution te;
    std::map<std::string, std::set<std::string>>  te_culprits_by_sat; // sat -> culprit signals
    std::map<std::string, std::map<std::string, int>> te_slips_by_sat; // sat -> signal -> dN
    // sat -> signals adjudicated by the resolver as REQUIRING NO STATE
    // ACTION, from three sources sharing one caller semantic (suppress the
    // MW mandate, touch nothing):
    //  - code-event signals (MW flag with affirmatively stable GF — the
    //    event lives in the code observable);
    //  - gap-bridge MOOT signals (across-gap slip already absorbed by the
    //    prune-and-reseed on the signal's return);
    //  - clean_signals (dN == 0 by a confident joint solve — the
    //    documented "must not destroy converged precision" contract; for
    //    repair satellites the culprit branch consumes them first, so
    //    their presence here is only reachable for clean-without-repair
    //    adjudications such as a bridged moot pair's clean partner).
    // Disjoint from te.reset_sats by resolver contract (conflicting
    // evidence withdraws the adjudications).
    std::map<std::string, std::set<std::string>> te_suppress_by_sat;
    {
        const std::vector<MwSlipResult> slip_results =
            mw_ ? mw_->detectLargeMwResets(modeled_obs)
                : std::vector<MwSlipResult>{};

        if (turbo_) {
            // mw_ is guaranteed non-null when turbo_ exists (constructor
            // refuses TurboEdit without the MW detector).  The clean-pair
            // set gates the resolver's GF-only equal-slip split on
            // affirmative MW evidence rather than mere MW silence; the
            // across-gap WL evidence feeds the bridged solve's WL-settle
            // deferral (the post-restart MW mean re-estimating the gap step).
            te = turbo_->resolve(slip_results, mw_->getEpochCleanPairs(),
                                 modeled_obs, sample_interval_s_,
                                 mw_->getAcrossGapWlEvidence(epoch_now_s));
            for (const auto& r : te.repairs) {
                te_culprits_by_sat[r.satellite].insert(r.signal);
                te_slips_by_sat[r.satellite][r.signal] = r.dN;
            }
            for (const auto& ce : te.code_events) {
                te_suppress_by_sat[ce.satellite].insert(ce.sig1);
                te_suppress_by_sat[ce.satellite].insert(ce.sig2);
            }
            for (const auto& [msat, sigs] : te.moot_signals)
                te_suppress_by_sat[msat].insert(sigs.begin(), sigs.end());
            for (const auto& [csat, sigs] : te.clean_signals)
                te_suppress_by_sat[csat].insert(sigs.begin(), sigs.end());

            // WL-settle deferral: a pending satellite's still-slipped phase
            // must stay out of the filter while the resolver waits for the
            // post-restart MW mean — register holds for its recently
            // converged phase signals (idempotent across the deferral; the
            // registered keys feed both the KF exclusion and the prune
            // protection).  Pivots are excluded as everywhere else; a
            // pivot's deferral runs to the resolver's timeout-reset.
            for (const auto& psat : te.pending_sats) {
                if (!soft_lli_.enabled()) break;
                if (is_bssd_pivot(psat)) {
                    log->warn("{}TurboEdit WL-settle pending for BSSD pivot {} "
                              "— holds unavailable for a pivot; deferral will "
                              "run to its timeout-reset",
                              epoch_prefix, psat);
                    continue;
                }
                soft_lli_.registerHold(psat, {}, *turbo_, epoch_now_s,
                                       epoch_prefix);
            }
        }

        // --- Soft-LLI bridge resolution (pre-pass) -------------------------
        // Resolve any outstanding holds against THIS epoch's bridge output
        // BEFORE the MW reset loop runs: a satellite whose held ambiguities
        // were integer-repaired here is added to @c soft_lli_repaired so the
        // loop below skips its (wl_known=false → full-reset) MW mandate — the
        // covariance-restoring snapshot+dN·λ repair already handled the slip.
        // No-op unless turbo_edit_bridge_lli holds something.
        if (turbo_ && !soft_lli_.empty()) {
            const SoftLliBridge::ResetFn reset_fn =
                [&](const std::string& key, const std::string& sat,
                    const std::string& sig) {
                    resetAmbiguityBssdAware(key, sat, sig, modeled_obs, state_manager,
                                            meas_model, epoch_prefix);
                };
            soft_lli_.resolveHolds(te, *turbo_, modeled_obs, state_manager,
                                   epoch_now_s, epoch_prefix, soft_lli_repaired,
                                   result.n_repaired, result.n_reset, reset_fn);
            if (!soft_lli_repaired.empty()) result.state_modified = true;
            reset_sats.insert(soft_lli_repaired.begin(), soft_lli_repaired.end());
            // Soft-LLI bridge "repair" is an ABSOLUTE value overwrite with the
            // covariance rebuilt from the pre-gap anchor (SoftLliBridge::
            // applyRestore) — not an additive, covariance-preserving repair.  The
            // smoother must SEGMENT these, not compensate them (audit F1 / C2).
            epoch_reset_sats_.insert(soft_lli_repaired.begin(), soft_lli_repaired.end());
        }

        // Group ALL MW results (slipped and non-slipped) by satellite for
        // cross-consistency analysis.
        // The MW detector returns results only for slipped pairs, so we also
        // need to know which pairs did NOT slip.

        // Helper: unique pair key from sig1 + sig2 for deduplication.
        // The MW detector emits two result entries per uncombined pair (one
        // per signal's amb_key), but they represent the same MW constraint.
        auto pairKey = [](const MwSlipResult& sr) -> std::string {
            return sr.sig1 + "_" + sr.sig2;
        };

        // Build per-satellite groups of slipped results (deduplicated by pair)
        std::map<std::string, std::vector<MwSlipResult>> slips_by_sat;
        {
            std::map<std::string, std::set<std::string>> seen_slip_pairs;
            for (const auto& sr : slip_results) {
                if (sr.sig1.empty() || sr.sig2.empty()) continue;
                const std::string sat_id = sr.satellite.empty()
                    ? satIdFromAmbiguityKey(sr.amb_key) : sr.satellite;
                const std::string pk = pairKey(sr);
                if (seen_slip_pairs[sat_id].insert(pk).second)
                    slips_by_sat[sat_id].push_back(sr);
            }
        }

        // Build per-satellite list of all MW signal pairs (slipped + clean),
        // deduplicated by (sig1, sig2).
        // Use getActiveArcKeys() which returns all mature arcs (those with
        // enough history to have detected a slip if one occurred), providing
        // clean-pair references for cross-consistency analysis.
        std::map<std::string, std::vector<MwSlipResult>> all_pairs_by_sat;
        {
            std::map<std::string, std::set<std::string>> seen_pairs;
            // First, add deduplicated slipped pairs
            for (const auto& [sat_id, slips] : slips_by_sat) {
                for (const auto& sr : slips) {
                    seen_pairs[sat_id].insert(pairKey(sr));
                    all_pairs_by_sat[sat_id].push_back(sr);
                }
            }
            // Then identify clean (non-slipped) signal pairs from all active
            // MW arcs.  An active arc that didn't produce a slip result means
            // dN_i - dN_j = 0 for that pair  a crucial reference for
            // cross-consistency analysis.
            for (const auto& stats_key :
                 (mw_ ? mw_->getActiveArcKeys() : std::vector<std::string>{})) {
                // stats_key = "G01_L1C_L2W" -> sat="G01", sig1="L1C", sig2="L2W"
                const auto first_us = stats_key.find('_');
                if (first_us == std::string::npos) continue;
                const std::string sat_id = stats_key.substr(0, first_us);
                const std::string rest = stats_key.substr(first_us + 1);
                const auto second_us = rest.find('_');
                if (second_us == std::string::npos) continue;
                const std::string s1 = rest.substr(0, second_us);
                const std::string s2 = rest.substr(second_us + 1);

                const std::string pk = s1 + "_" + s2;
                if (!seen_pairs[sat_id].insert(pk).second)
                    continue; // already present (slipped or duplicate)

                MwSlipResult clean_entry;
                clean_entry.amb_key = sat_id + "_" + s1;
                clean_entry.sig1 = s1;
                clean_entry.sig2 = s2;
                clean_entry.satellite = sat_id;
                clean_entry.slip_wl_known = true;
                clean_entry.slip_wl_cycles = 0;
                all_pairs_by_sat[sat_id].push_back(clean_entry);
            }
        }

        if (!slip_results.empty()) {
            // Collect all distinct amb_keys that need reset, grouped by satellite.
            // This includes LLI resets (empty sig1/sig2) and MW pair resets.
            std::map<std::string, std::set<std::string>> reset_keys_by_sat;
            // Satellites carrying a receiver-declared loss of lock this
            // epoch (signal-less reset entries).  LLI is authoritative:
            // integer continuity is broken arbitrarily, so culprit
            // repairs — in particular TurboEdit's GF-only resolution,
            // whose equal-slip premise is unverifiable when the LLI
            // branch suppressed all MW pair evidence — must not override
            // the mandated resets.  Mirrors the real-time engine, which
            // already treats LLI entries as unconditional resets.
            std::set<std::string> lli_sats;
            for (const auto& sr : slip_results) {
                const std::string sat_id = sr.satellite.empty()
                    ? satIdFromAmbiguityKey(sr.amb_key) : sr.satellite;
                reset_keys_by_sat[sat_id].insert(sr.amb_key);
                if (sr.sig1.empty() || sr.sig2.empty()) lli_sats.insert(sat_id);
                // Cache one representative slip per sat for the satellite-
                // status trace (Application::writeEpochTraces "Slip" rows).
                // Deliberately includes code-event-vetoed sats: the MW flag
                // is factual even when the phase reset gets suppressed.
                last_mw_slips_[sat_id] = sr;
            }

            // Ambiguity keys actually RESET during this epoch's slip phase
            // (full resets, targeted culprit resets, residual resets).  A
            // reset reseeds from CURRENT observations, i.e. in the
            // post-slip frame — so the in-place pivot machinery's
            // clean-peer pass below must never apply an additional
            // -dN_pivot shift on top of such a key (the fresh seed is
            // already consistent with the slipped pivot's carrier).
            std::set<std::string> epoch_reset_amb_keys;

            // --- Pivot-aware repair guardrail ---------------------------
            // Pre-scan: identify (constellation, signal) pairs where the
            // BSSD pivot itself is among the culprits.  When the pivot
            // slipped on signal S, every non-pivot sat's SD ambiguity for
            // S is referenced against the pivot's S ambiguity, which
            // just shifted by dN_pivot · λ.  Applying each non-pivot's
            // own dN_non-pivot · λ as an integer repair in the *current*
            // (pre-handover) SD frame would correct only the non-pivot's
            // own carrier slip and leave the frame off by dN_pivot · λ.
            //
            // Approach: defer the integer repair for every pivot-affected
            // (sat, signal), force a pivot handover synchronously here in
            // the cycle-slip phase (before the KF measurement update),
            // then apply each queued dN · λ to the post-handover SD
            // ambiguity.  In the new frame the new pivot is by
            // construction a sat that did NOT slip on S, so its
            // dN_newpivot = 0 and each remaining slipped sat's SD
            // adjustment is just its own dN · λ (including the *old*
            // pivot, which becomes a regular non-pivot with a non-zero
            // SD value and its own dN_oldpivot to apply).  Covariance
            // is preserved through both the handover transform
            // (which is a clean S-transform) and the integer step.
            //
            // Granularity is per-(cons, signal), so a sat with two
            // culprit signals — one pivot-affected, one not — gets the
            // pre-handover integer repair applied to the unaffected
            // signal and the post-handover deferred repair for the
            // pivot-affected one.
            std::set<std::pair<std::string, std::string>> pivot_affected_cons_sig;
            if (config_.enableBetweenSatelliteSd()) {
                const auto& pivots = meas_model.getBssdPivotSatellites();
                for (const auto& [sat_id, slips] : slips_by_sat) {
                    if (slips.empty()) continue;
                    const std::string cons = PppUtils::getSystemString(sat_id);
                    auto piv_it = pivots.find(cons);
                    if (piv_it == pivots.end()) continue;
                    if (piv_it->second != sat_id) continue;  // not the pivot

                    // This sat IS the pivot for its constellation and has
                    // slipped MW pairs.  Identify which signal(s) of the
                    // pivot are culprits — those are the (cons, signal)
                    // pairs that get the deferred-repair-after-handover
                    // treatment.  Cross-consistency handles 3+ frequencies;
                    // for the dual-frequency case it returns nothing, so fall
                    // back to TurboEdit's MW+GF resolution so a common-mode
                    // dual-frequency pivot slip still drives the in-place /
                    // handover machinery (rather than a blind full reset).
                    std::set<std::string> pivot_culprits =
                        identifySlippedSignals(slips, all_pairs_by_sat[sat_id]);
                    if (pivot_culprits.empty()) {
                        auto tcit = te_culprits_by_sat.find(sat_id);
                        if (tcit != te_culprits_by_sat.end())
                            pivot_culprits = tcit->second;
                    }
                    // A code-event-adjudicated signal cannot be a pivot
                    // culprit: the GF evidence says its phase (and hence
                    // the SD frame it anchors) is still, so no deferral /
                    // handover is owed on its account.  The per-sat
                    // contradiction guard below independently demotes such
                    // a satellite to a conservative full reset.
                    if (const auto tce_p = te_suppress_by_sat.find(sat_id);
                        tce_p != te_suppress_by_sat.end()) {
                        for (auto it = pivot_culprits.begin();
                             it != pivot_culprits.end(); ) {
                            it = tce_p->second.count(*it) ? pivot_culprits.erase(it)
                                                          : std::next(it);
                        }
                    }
                    for (const auto& sig : pivot_culprits) {
                        pivot_affected_cons_sig.emplace(cons, sig);
                        log->info("{}Pivot slip detected on {} {}: deferring integer repair "
                                   "for every sat's {} culprit (resolved below: in-place if the pivot "
                                   "integer is known, else via forced handover)",
                                   epoch_prefix, sat_id, sig, sig);
                    }
                }
            }

            // Deferred state for pivot-affected (sat, signal) culprits.
            // Populated by the per-sat loop below; drained after the
            // loop by the force-handover + post-handover repair block.
            struct DeferredIntegerRepair {
                std::string sat_id;
                std::string signal;
                int         dN_carrier_cycles;
            };
            std::vector<DeferredIntegerRepair> deferred_integer_repairs;
            // Pivot-affected sats whose slip integer was inconsistent
            // across pairs — no clean integer to apply, must reset
            // after handover.
            std::vector<std::pair<std::string, std::string>> deferred_resets;

            // Process each satellite once.
            for (auto& [sat_id, amb_keys] : reset_keys_by_sat) {
                // Soft-LLI bridge already integer-repaired this satellite's
                // held ambiguities in the pre-pass (snapshot+dN·λ) — its MW
                // mandate (typically wl_known=false → full reset) must NOT
                // override that covariance-restoring repair.
                if (soft_lli_repaired.count(sat_id)) continue;
                // WL-settle deferral: the resolver is adjudicating this
                // satellite's across-gap event against a settling MW
                // estimate.  Its mandate is suppressed — the holds keep the
                // affected phase out of the filter — and its GF arcs must
                // survive untouched until the deferral resolves to a
                // repair, reset, or timeout in a later epoch.
                if (te.pending_sats.count(sat_id)) {
                    log->info("{}MW mandate for {} suppressed: TurboEdit "
                              "WL-settle deferral pending (holds active)",
                              epoch_prefix, sat_id);
                    continue;
                }
                const auto& sat_all_pairs = all_pairs_by_sat[sat_id];
                const auto slips_it = slips_by_sat.find(sat_id);
                const bool has_pair_slips = slips_it != slips_by_sat.end()
                                         && !slips_it->second.empty();

                // --- Per-signal culprit resolution ---
                // (1) Multi-frequency MW cross-consistency: when a satellite
                //     has 3+ MW pairs (slipped + clean), overlapping pairs
                //     isolate which individual signal slipped.  When the slipped
                //     pair was wl_locked with a known integer, the lone-culprit
                //     interpretation gives an unambiguous signal-level shift.
                // (2) Dual-frequency TurboEdit fallback: when cross-consistency
                //     cannot split (only a single MW pair exists), pair the MW
                //     wide-lane integer with the geometry-free phase jump to
                //     recover the per-signal integers (MW+GF joint solve).
                // Both paths yield (culprit_signals, per_signal_slips) and feed
                // the identical repair + BSSD pivot-deferral machinery below;
                // culprits with a cross-consistent integer are repaired
                // (additive, covariance preserved), the rest reset.
                // LLI authority: a satellite with a receiver-declared loss
                // of lock takes the full-reset fall-through unconditionally.
                // No MW pair evidence exists on an LLI epoch (the detector
                // skips pair processing), so cross-consistency cannot fire,
                // and TurboEdit's GF-only resolution must not be adopted —
                // its ΔN_WL = 0 premise is unverifiable across a lock loss.
                const bool lli_sat = lli_sats.count(sat_id) > 0;

                std::set<std::string> culprit_signals;
                std::map<std::string, int> per_signal_slips;
                bool culprits_from_te = false;
                if (!lli_sat && has_pair_slips && sat_all_pairs.size() >= 2
                    && slips_it->second.size() < sat_all_pairs.size()) {
                    culprit_signals = identifySlippedSignals(
                        slips_it->second, sat_all_pairs);
                    if (!culprit_signals.empty())
                        per_signal_slips = computeCulpritSlipIntegers(
                            slips_it->second, culprit_signals);
                }
                if (culprit_signals.empty() && !lli_sat) {
                    auto tcit = te_culprits_by_sat.find(sat_id);
                    if (tcit != te_culprits_by_sat.end() && !tcit->second.empty()) {
                        culprit_signals  = tcit->second;
                        per_signal_slips = te_slips_by_sat[sat_id];
                        culprits_from_te = true;
                        log->info("{}TurboEdit dual-freq resolution for {}: {} "
                                  "culprit signal(s) from MW+GF joint solve",
                                  epoch_prefix, sat_id, culprit_signals.size());
                    }
                }

                // --- Contradiction guard: culprits vs code-event adjudication
                // A multi-pair code artifact (a shared-code NL excursion
                // flags two MW pairs at once on a 3-freq sat) can hand
                // cross-consistency a "culprit" whose phase the resolver's
                // GF evidence adjudicated affirmatively STILL — worst case a
                // covariance-preserved integer repair onto a phase that
                // never moved (the second pair's unusable integer leaves a
                // single "consistent" candidate).  Both readings cannot be
                // right, and the cost asymmetry is reversed for repairs, so
                // contradictory evidence demotes the satellite to the
                // conservative full-reset fall-through, atomically: the
                // culprits AND the code-event adjudication are both
                // withdrawn.  (TE-sourced culprits cannot contradict — the
                // resolver's staging already demotes repair-vs-zero
                // conflicts to reset_sats and withdraws the events — but
                // the guard is cheap and general.)
                if (!culprit_signals.empty()) {
                    const auto tce_c = te_suppress_by_sat.find(sat_id);
                    if (tce_c != te_suppress_by_sat.end()) {
                        bool contradicted = false;
                        for (const auto& sig : culprit_signals)
                            if (tce_c->second.count(sig)) { contradicted = true; break; }
                        if (contradicted) {
                            log->warn("{}MW culprit analysis for {} contradicts the "
                                      "TurboEdit code-event adjudication (a culprit "
                                      "signal was adjudicated GF-still); demoting to "
                                      "conservative full reset",
                                      epoch_prefix, sat_id);
                            culprit_signals.clear();
                            per_signal_slips.clear();
                            culprits_from_te = false;
                            te_suppress_by_sat.erase(tce_c);
                        }
                    }
                }
                if (!culprit_signals.empty()) {
                    {
                        for (const auto& culprit : culprit_signals) {
                            const std::string targeted_key = sat_id + "_" + culprit;
                            const std::string cons = PppUtils::getSystemString(sat_id);
                            const bool pivot_affected =
                                pivot_affected_cons_sig.count({cons, culprit}) > 0;

                            if (pivot_affected) {
                                // Defer: integer adjustment will be applied
                                // post-handover in the new SD frame.  Don't
                                // touch the state vector now — the handover
                                // transform expects the pre-slip SD values
                                // (the OLD pivot's frozen 0 is the implicit
                                // reference); applying dN here in the OLD
                                // frame would be off by dN_pivot · λ.
                                auto slip_it = per_signal_slips.find(culprit);
                                if (slip_it != per_signal_slips.end()) {
                                    deferred_integer_repairs.push_back(
                                        { sat_id, culprit, slip_it->second });
                                    log->info("{}MW deferred: {} {} dN={} cyc "
                                              "(integer repair queued; applied in-place if the pivot integer is "
                                              "known, else after forced handover)",
                                              epoch_prefix, sat_id, culprit, slip_it->second);
                                } else {
                                    deferred_resets.emplace_back(sat_id, culprit);
                                    log->info("{}MW deferred: {} {} (reset queued; resolved in-place or "
                                              "post-handover — no clean per-pair integer)",
                                              epoch_prefix, sat_id, culprit);
                                }
                                ++result.n_repaired;
                                result.state_modified = true;
                                continue;
                            }

                            // Non-pivot-affected: existing repair/reset path.
                            bool repaired = false;
                            auto slip_it = per_signal_slips.find(culprit);
                            if (slip_it != per_signal_slips.end()) {
                                repaired = repairAmbiguityBssdAware(
                                    sat_id, culprit, slip_it->second,
                                    modeled_obs, state_manager, meas_model,
                                    epoch_prefix);
                            }
                            if (!repaired) {
                                resetAmbiguityBssdAware(targeted_key, sat_id, culprit,
                                                        modeled_obs, state_manager, meas_model,
                                                        epoch_prefix);
                                epoch_reset_amb_keys.insert(targeted_key);
                                log->info("{}MW cross-consistency: targeted reset {} (culprit signal {})",
                                          epoch_prefix, targeted_key, culprit);
                            }
                            ++result.n_repaired;
                            result.state_modified = true;
                        }

                        // --- Residual mandated resets ---
                        // amb_keys collects every reset mandate the detector
                        // issued for this satellite, but the culprit handling
                        // above adjudicated only part of it.  Cross-consistency
                        // adjudicates every signal of every analyzed pair (a
                        // non-culprit appears in a clean pair by construction).
                        // The TurboEdit fallback adjudicates only the pairs its
                        // GF arcs could actually solve: culprits (dN ≠ 0) and
                        // their confidently-clean partners (dN == 0, reported
                        // via clean_signals).  A flagged pair outside the solve
                        // (GF arc in warm-up / per-signal dropout) still
                        // carries an un-adjudicated mandate that must take its
                        // reset rather than being dropped by the `continue`
                        // below.  This also closes the combined-IF hole:
                        // per-raw-signal culprit keys don't exist in the IF
                        // state (the repair above no-ops on them), but the
                        // real IF amb_key arrives here un-adjudicated and is
                        // reset — restoring the pre-TurboEdit behavior in
                        // that mode instead of silently swallowing the slip.
                        std::set<std::string> adjudicated = culprit_signals;
                        if (culprits_from_te) {
                            auto tcl = te.clean_signals.find(sat_id);
                            if (tcl != te.clean_signals.end())
                                adjudicated.insert(tcl->second.begin(),
                                                   tcl->second.end());
                        } else {
                            for (const auto& pr : sat_all_pairs) {
                                adjudicated.insert(pr.sig1);
                                adjudicated.insert(pr.sig2);
                            }
                        }
                        for (const auto& key : amb_keys) {
                            const std::string signal = signalFromAmbiguityKey(key);
                            if (adjudicated.count(signal)) continue;
                            resetAmbiguityBssdAware(key, sat_id, signal,
                                                    modeled_obs, state_manager,
                                                    meas_model, epoch_prefix);
                            epoch_reset_amb_keys.insert(key);
                            ++result.n_reset;
                            result.state_modified = true;
                            log->info("{}Residual reset {} (mandate not adjudicated "
                                      "by the culprit analysis)",
                                      epoch_prefix, key);
                        }

                        reset_sats.insert(sat_id);
                        continue;
                    }
                }

                // --- TurboEdit no-state-action adjudications ---------------
                // No culprit resolution exists for this satellite, but the
                // resolver adjudicated its MW pair flag(s) as requiring NO
                // state action: a code-domain artifact (GF affirmatively
                // stable — the event lives in the code observable), a
                // gap-bridge MOOT attribution (the across-gap slip was
                // already absorbed by the prune-and-reseed on the signal's
                // return), or a confidently clean partner (dN == 0).
                // Suppress the mandated phase resets for the adjudicated
                // signals instead of taking the full-reset fall-through.
                // The cost asymmetry contracts the backstop to the KF: a
                // wrongly suppressed real slip lands ≥ |4·λ1| ≈ 0.76 m in
                // that signal's phase innovation at this very epoch's update
                // and takes the residual-gate reset one epoch late; a
                // wrongly honored mandate destroys converged ambiguities
                // (and AR continuity) immediately.  Mandates on signals
                // OUTSIDE the adjudication (a pair whose GF arc was in
                // warm-up / per-signal dropout, combined-IF amb_keys) still
                // take their reset, mirroring the residual-mandate rule
                // above.  LLI sats never reach this block (gated above) — a
                // receiver-declared loss of lock outranks the GF evidence.
                if (!lli_sat) {
                    const auto tce = te_suppress_by_sat.find(sat_id);
                    if (tce != te_suppress_by_sat.end() && !tce->second.empty()) {
                        std::size_t n_suppressed = 0;
                        bool any_reset = false;
                        for (const auto& key : amb_keys) {
                            const std::string signal = signalFromAmbiguityKey(key);
                            if (tce->second.count(signal)) {
                                ++n_suppressed;
                                continue;
                            }
                            resetAmbiguityBssdAware(key, sat_id, signal, modeled_obs,
                                                    state_manager, meas_model,
                                                    epoch_prefix);
                            epoch_reset_amb_keys.insert(key);
                            ++result.n_reset;
                            result.state_modified = true;
                            any_reset = true;
                            log->info("{}Residual reset {} (mandate outside the "
                                      "TurboEdit adjudication)", epoch_prefix, key);
                        }
                        ++result.n_code_events;
                        log->info("{}TurboEdit adjudication for {}: {} phase reset "
                                  "mandate(s) suppressed (code event / bridged moot "
                                  "/ adjudicated clean), ambiguities preserved "
                                  "(KF residual gate is the backstop)",
                                  epoch_prefix, sat_id, n_suppressed);
                        // Mark the sat as handled only when state actually
                        // changed: a fully vetoed sat stays eligible for the
                        // GF-only pass and the auxiliary detectors (their
                        // independent evidence must not be muted by a veto
                        // that touched nothing).
                        if (any_reset) reset_sats.insert(sat_id);
                        continue;
                    }
                }

                // --- Fall through to full reset ---
                log->debug("{}MW full reset for {} ({} slipped pairs, {} total mature pairs, {} amb_keys)",
                           epoch_prefix, sat_id,
                           has_pair_slips ? slips_it->second.size() : 0,
                           sat_all_pairs.size(), amb_keys.size());
                for (const auto& key : amb_keys) {
                    const std::string signal = signalFromAmbiguityKey(key);
                    resetAmbiguityBssdAware(key, sat_id, signal, modeled_obs,
                                            state_manager, meas_model, epoch_prefix);
                    epoch_reset_amb_keys.insert(key);
                    ++result.n_reset;
                    result.state_modified = true;
                }
                reset_sats.insert(sat_id);

                // Receiver-declared loss of lock also invalidates the
                // TurboEdit GF arc levels for this satellite.  The MW EWMA
                // is deliberately preserved across LLI (observation-domain
                // history; see the detector's LLI branch), but a GF arc is
                // level-bearing: a slip baked into prev_gf would echo into
                // the next epoch's TD2 with flipped sign exactly when MW
                // flags the pair with the true integer, and the joint solve
                // would then emit a confident wrong repair onto the
                // just-reseeded ambiguity.
                //
                // EXCEPTION — soft-LLI bridge (turbo_edit_bridge_lli): for an
                // AR-capable, non-pivot satellite at high rate we instead HOLD
                // the ambiguities across the (short) LLI gap and let the
                // bridge integer-repair them at gap return.  That requires the
                // GF arc's pre-gap level to SURVIVE, so we deliberately do NOT
                // reset it here; the hold registration snapshots the pre-gap
                // values and marks the signals held-continuous in the resolver.
                // The BSSD pivot is excluded (its SD ambiguity is frozen at 0;
                // a pivot LLI needs a handover, not an in-frame integer shift).
                if (lli_sat && turbo_) {
                    bool is_bssd_pivot = false;
                    if (config_.enableBetweenSatelliteSd()) {
                        const auto& pivots = meas_model.getBssdPivotSatellites();
                        auto pit = pivots.find(PppUtils::getSystemString(sat_id));
                        is_bssd_pivot = (pit != pivots.end() && pit->second == sat_id);
                    }
                    if (!is_bssd_pivot && soft_lli_.qualifies(sat_id, epoch_now_s)) {
                        soft_lli_.registerHold(sat_id, amb_keys, *turbo_,
                                               epoch_now_s, epoch_prefix);
                        // GF arc intentionally PRESERVED (no resetSatellite).
                    } else {
                        turbo_->resetSatellite(sat_id);
                    }
                }

                // --- Sat-wide state re-inflation ---
                // When EVERY phase ambiguity for a satellite has been
                // reset, the new ambiguities will reseed against the
                // current (iono, sat-bias) state — baking in whatever
                // partition existed pre-reset.  In uncombined PPP-RTK
                // that partition can absorb slant iono in a non-unique
                // way (iono state holds I_true − δ/α, sat-bias holds
                // b_true + δ); without re-inflating iono and bias
                // variances now, the wrong partition stays locked in
                // for the rest of the arc and the L5 / E5* / B2a code
                // residual shows the unmodelable component for
                // hundreds of epochs.
                //
                // Skip BSSD pivot sats — their iono and bias states
                // are frozen at (0, 0) by PivotManager; reinflate
                // would either no-op (caught by the diag==0 guard in
                // StateManager) or break the BSSD frame.
                const auto& sr_cfg = config_.getSlipStateReinflationConfig();
                if (sr_cfg.enabled) {
                    bool is_pivot = false;
                    if (config_.enableBetweenSatelliteSd()) {
                        const auto& pivots_now = meas_model.getBssdPivotSatellites();
                        const std::string cons = PppUtils::getSystemString(sat_id);
                        auto pit = pivots_now.find(cons);
                        is_pivot = (pit != pivots_now.end() && pit->second == sat_id);
                    }
                    if (!is_pivot) {
                        const double io_std = sr_cfg.iono_std_m;
                        const double sb_std = sr_cfg.sat_bias_std_m;
                        if (io_std > 0.0) {
                            state_manager.reinflateIonosphereVariance(
                                sat_id, io_std * io_std);
                        }
                        if (sb_std > 0.0) {
                            state_manager.reinflateSatBiasVariancesForSat(
                                sat_id, sb_std * sb_std);
                        }
                    } else {
                        log->debug("{}Slip-state reinflation skipped for BSSD pivot {}",
                                    epoch_prefix, sat_id);
                    }
                }
            }

            // --- Repair-in-place across a slipped pivot (relative integers) ---
            // The handover block below re-anchors the SD frame onto a *clean*
            // satellite before applying each peer's integer.  That needs a
            // clean satellite to exist — which it does not in a common-mode
            // event (e.g. a receiver E6-band re-acquisition that slips every
            // Galileo L6C at once), so the handover refuses and every peer
            // takes a convergence-destroying reset.
            //
            // A clean anchor is not actually required: the pivot does not need
            // to be *unslipped*, only to have a *known* slip integer.  The SD
            // ambiguity state is N_sd(peer) = N(peer) − N(pivot), so a
            // simultaneous slip moves the true SD by (dN_peer − dN_pivot).
            // Shifting each peer's SD ambiguity by that *relative* integer
            // repairs the frame in place: pivot retained, no handover, no
            // clean-anchor requirement, covariance preserved.  This is exactly
            // the identity-handover case of the block below, with the integer
            // step taken relative to the (slipped but known) pivot.
            //
            // Eligibility is integer confidence: the pivot's own per-signal
            // integer must be present in deferred_integer_repairs — i.e. it
            // survived computeCulpritSlipIntegers' cross-pair consistency
            // check — for EVERY pivot-affected signal of the constellation.
            // The decision is all-or-nothing per constellation because the
            // BSSD pivot is per-constellation: handling some signals in place
            // while letting a same-constellation handover fire for the rest
            // would re-anchor (and re-seed) the very frame we just repaired.
            // Constellations that don't qualify fall through to the existing
            // handover-or-reset path unchanged.
            //
            // A wrong pivot integer would surface next epoch as a coherent
            // per-constellation residual offset, which the KF post-fit
            // pivot-slip detector already catches and turns into a forced
            // handover (Application.cpp) — the existing backstop, no separate
            // revocation needed here.  MW arc EWMA stats are observation-space
            // and untouched by the state-level integer step.
            //
            // Confidence test for the non-slipped-peer frame shift inside the
            // block: a sat is "confidently clean" on @p sig only if it has a
            // mature MW arc involving @p sig (so a slip WOULD have been seen)
            // and @p sig is not among its identified culprits.  Distinguishes
            // a genuinely-unslipped peer (take the lossless -dN_pivot shift)
            // from one we simply couldn't evaluate this epoch (reset instead).
            auto confidentlyCleanOnSignal =
                [&](const std::string& sat, const std::string& sig) -> bool {
                // A receiver-declared loss of lock is NEVER affirmative
                // cleanliness: the LLI sat has no pair slips (the detector
                // skips pair processing) yet its preserved MW arcs still
                // appear mature, so without this guard the
                // zero-slips-anywhere branch below would classify a sat
                // whose integer continuity just broke arbitrarily as
                // "confidently clean" and hand it a covariance-preserving
                // shift.
                if (lli_sats.count(sat)) return false;
                auto pit = all_pairs_by_sat.find(sat);
                if (pit == all_pairs_by_sat.end()) return false;
                bool has_arc_on_sig = false;
                for (const auto& pr : pit->second)
                    if (pr.sig1 == sig || pr.sig2 == sig) { has_arc_on_sig = true; break; }
                if (!has_arc_on_sig) return false;  // no MW evidence on this signal
                auto sit = slips_by_sat.find(sat);
                if (sit == slips_by_sat.end() || sit->second.empty())
                    return true;  // mature arc on sig and zero slips anywhere
                return identifySlippedSignals(sit->second, pit->second).count(sig) == 0;
            };

            if (config_.enableBetweenSatelliteSd() && !deferred_integer_repairs.empty()) {
                const auto& pivots_now = meas_model.getBssdPivotSatellites();

                std::set<std::string> cons_set;
                for (const auto& cs : pivot_affected_cons_sig)
                    cons_set.insert(cs.first);

                std::set<std::string> handled_cons;

                for (const auto& cons : cons_set) {
                    auto pit = pivots_now.find(cons);
                    if (pit == pivots_now.end()) continue;
                    const std::string pivot = pit->second;

                    // Pivot-affected signals for this constellation.
                    std::set<std::string> cons_signals;
                    for (const auto& cs : pivot_affected_cons_sig)
                        if (cs.first == cons) cons_signals.insert(cs.second);

                    // Require the pivot's own confident integer for every
                    // pivot-affected signal (all-or-nothing per constellation).
                    std::map<std::string, int> dN_pivot_by_sig;
                    bool all_known = true;
                    for (const auto& sig : cons_signals) {
                        bool found = false;
                        for (const auto& dr : deferred_integer_repairs) {
                            if (dr.sat_id == pivot && dr.signal == sig) {
                                dN_pivot_by_sig[sig] = dr.dN_carrier_cycles;
                                found = true;
                                break;
                            }
                        }
                        if (!found) { all_known = false; break; }
                    }
                    if (!all_known) continue;  // fall through to handover

                    int n_handled = 0;
                    for (const auto& sig : cons_signals) {
                        const int dN_pivot = dN_pivot_by_sig[sig];
                        for (const auto& dr : deferred_integer_repairs) {
                            if (PppUtils::getSystemString(dr.sat_id) != cons
                                || dr.signal != sig) continue;
                            if (dr.sat_id == pivot) continue;  // SD frozen at 0

                            const int rel = dr.dN_carrier_cycles - dN_pivot;
                            if (rel == 0) {
                                // Peer slipped by the same integer as the
                                // pivot: SD difference unchanged, nothing to do.
                                log->info("{}MW in-place: {} {} frame-consistent "
                                          "(dN_peer==dN_pivot={:+d}), no shift",
                                          epoch_prefix, dr.sat_id, sig, dN_pivot);
                                ++n_handled;
                                continue;
                            }

                            const bool ok = repairAmbiguityBssdAware(
                                dr.sat_id, sig, rel, modeled_obs,
                                state_manager, meas_model, epoch_prefix);
                            if (ok) {
                                log->info("{}MW in-place REPAIR {} {}: "
                                          "dN_peer={:+d} - dN_pivot={:+d} = {:+d} cyc "
                                          "(pivot {} retained, no handover, cov preserved)",
                                          epoch_prefix, dr.sat_id, sig,
                                          dr.dN_carrier_cycles, dN_pivot, rel, pivot);
                            } else {
                                // Sat unobserved this epoch / bad lambda: the
                                // relative shift can't be applied, so reset in
                                // the current (pivot-retained) frame.
                                resetAmbiguityBssdAware(
                                    dr.sat_id + "_" + sig, dr.sat_id, sig,
                                    modeled_obs, state_manager, meas_model, epoch_prefix);
                                log->info("{}MW in-place repair unavailable for {} {}; "
                                          "reset in current frame (pivot {} retained)",
                                          epoch_prefix, dr.sat_id, sig, pivot);
                            }
                            ++n_handled;
                        }

                        // --- Non-slipped peers: track the pivot's jump ---
                        // The flagged-peer loop above only shifts sats the MW
                        // detector queued.  A non-pivot SV that did NOT slip
                        // still has its SD ambiguity (N_peer − N_pivot) thrown
                        // off by exactly −dN_pivot when the pivot slipped — its
                        // frozen reference moved under it.  Such sats never
                        // enter the deferred queues, so without this pass they
                        // are left corrupt and surface as a lone giant post-fit
                        // residual (the E23−E13 L6C case).  Shift each
                        // confidently-clean observed peer by −dN_pivot
                        // (lossless integer step, covariance preserved); reset
                        // peers whose slip status can't be confirmed this epoch
                        // rather than risk applying a wrong integer.
                        if (dN_pivot != 0) {
                            std::set<std::string> handled_this_sig;
                            for (const auto& dr : deferred_integer_repairs)
                                if (PppUtils::getSystemString(dr.sat_id) == cons
                                    && dr.signal == sig)
                                    handled_this_sig.insert(dr.sat_id);
                            for (const auto& pr : deferred_resets)
                                if (PppUtils::getSystemString(pr.first) == cons
                                    && pr.second == sig)
                                    handled_this_sig.insert(pr.first);

                            for (const auto& obs : modeled_obs) {
                                const std::string& sat = obs.satellite;
                                if (sat == pivot) continue;
                                if (PppUtils::getSystemString(sat) != cons) continue;
                                if (!obs.observed.count(sig)) continue;
                                if (handled_this_sig.count(sat)) continue;
                                if (state_manager.getAmbiguityIndex(sat + "_" + sig) < 0)
                                    continue;
                                // Already reset this epoch (full / targeted /
                                // residual): the fresh observation-based seed
                                // is in the post-slip frame and consistent
                                // with the slipped pivot's carrier — an
                                // additional -dN_pivot shift would corrupt it
                                // by exactly dN_pivot · λ.
                                if (epoch_reset_amb_keys.count(sat + "_" + sig))
                                    continue;

                                if (confidentlyCleanOnSignal(sat, sig)) {
                                    const bool ok = repairAmbiguityBssdAware(
                                        sat, sig, -dN_pivot, modeled_obs,
                                        state_manager, meas_model, epoch_prefix);
                                    if (ok) {
                                        log->info("{}MW in-place REPAIR (clean peer) {} {}: "
                                                  "dN_peer=0 - dN_pivot={:+d} = {:+d} cyc "
                                                  "(pivot {} slipped under a non-slipped peer; "
                                                  "cov preserved)",
                                                  epoch_prefix, sat, sig, dN_pivot,
                                                  -dN_pivot, pivot);
                                    } else {
                                        resetAmbiguityBssdAware(
                                            sat + "_" + sig, sat, sig, modeled_obs,
                                            state_manager, meas_model, epoch_prefix);
                                        log->info("{}MW in-place repair unavailable for clean "
                                                  "peer {} {}; reset in current frame "
                                                  "(pivot {} retained)",
                                                  epoch_prefix, sat, sig, pivot);
                                    }
                                    ++n_handled;
                                } else {
                                    resetAmbiguityBssdAware(
                                        sat + "_" + sig, sat, sig, modeled_obs,
                                        state_manager, meas_model, epoch_prefix);
                                    log->info("{}MW in-place reset (unverifiable peer) {} {} "
                                              "(pivot {} slipped, peer slip status "
                                              "unconfirmed this epoch)",
                                              epoch_prefix, sat, sig, pivot);
                                    ++n_handled;
                                }
                            }
                        }
                    }

                    // Peers with no clean per-pair integer can't be repaired;
                    // reset them in the current (pivot-retained) SD frame
                    // rather than dragging the whole constellation to handover.
                    for (const auto& pr : deferred_resets) {
                        if (PppUtils::getSystemString(pr.first) != cons) continue;
                        if (pr.first == pivot) continue;
                        resetAmbiguityBssdAware(
                            pr.first + "_" + pr.second, pr.first, pr.second,
                            modeled_obs, state_manager, meas_model, epoch_prefix);
                        log->info("{}MW in-place reset {} {} (no clean per-pair "
                                  "integer; pivot {} retained)",
                                  epoch_prefix, pr.first, pr.second, pivot);
                        ++n_handled;
                    }

                    log->warn("{}MW in-place pivot-frame repair for {}: pivot {} "
                              "retained, {} peer(s) handled across {} signal(s), "
                              "NO handover",
                              epoch_prefix, cons, pivot, n_handled,
                              cons_signals.size());
                    handled_cons.insert(cons);
                    result.state_modified = true;
                }

                // Drain handled constellations from the deferred queues and the
                // pivot-affected set so the handover block only processes what
                // still genuinely needs a handover (or reset).
                if (!handled_cons.empty()) {
                    auto consHandled = [&](const std::string& sat) {
                        return handled_cons.count(
                            PppUtils::getSystemString(sat)) > 0;
                    };
                    deferred_integer_repairs.erase(
                        std::remove_if(deferred_integer_repairs.begin(),
                                       deferred_integer_repairs.end(),
                                       [&](const DeferredIntegerRepair& d) {
                                           return consHandled(d.sat_id);
                                       }),
                        deferred_integer_repairs.end());
                    deferred_resets.erase(
                        std::remove_if(deferred_resets.begin(),
                                       deferred_resets.end(),
                                       [&](const std::pair<std::string,
                                                           std::string>& p) {
                                           return consHandled(p.first);
                                       }),
                        deferred_resets.end());
                    for (auto it = pivot_affected_cons_sig.begin();
                         it != pivot_affected_cons_sig.end(); ) {
                        if (handled_cons.count(it->first))
                            it = pivot_affected_cons_sig.erase(it);
                        else
                            ++it;
                    }
                }
            }

            // --- Force pivot handover + apply deferred integer repairs ---
            // Triggered when the pre-scan above queued any (sat, signal)
            // whose constellation pivot is itself slipping on the same
            // signal.  We:
            //   1. Snapshot the current pivot per constellation.
            //   2. requestForcedHandover on each affected constellation.
            //   3. handlePivotHandover(forced_only=true) synchronously
            //      here in the pre-update path — re-references every SD
            //      ambiguity / iono / sat-bias for the affected cons
            //      from the old pivot to a clean replacement.
            //   4. For each deferred (sat, signal, dN), apply
            //      x(amb_idx) += dN · λ to the now-post-handover SD
            //      ambiguity, covariance preserved.  This is the
            //      *exactly* the integer carrier slip on the sat itself
            //      because the new pivot did not slip on this signal
            //      (it was picked by the pivot selector from the pool
            //      of clean candidates).
            //   5. For deferred sats with no clean per-pair integer,
            //      fall back to resetAmbiguityBssdAware in the new
            //      frame.
            // MW arc EWMA stats are intentionally NOT touched — the
            // arc data is in observation space and survives both the
            // handover and the integer step unchanged.
            if (!deferred_integer_repairs.empty() || !deferred_resets.empty()) {
                std::set<std::string> affected_cons;
                for (const auto& [cons, sig] : pivot_affected_cons_sig)
                    affected_cons.insert(cons);

                // PivotManager mutation requires non-const access.
                // detectAndRepair takes meas_model by const& because the
                // historical pivot-info read path is const; this is the
                // only write site and is bounded to the pivot-affected
                // failure mode.  Caller (Application::detectCycleSlips)
                // owns meas_model as a non-const unique_ptr, so the cast
                // is well-defined.
                auto& pivot_mgr =
                    const_cast<MeasurementModel&>(meas_model).getPivotManager();

                const auto pivots_before = pivot_mgr.getPivotSatellites();  // copy

                // Build the exclude set: every sat with slip evidence
                // this epoch, from ANY detector level.  Passed to
                // handlePivotHandover so the forced-handover replacement
                // search can never pick another sat that is itself
                // mid-slip — that would simply move the broken SD-frame
                // anchor from one slipping sat to another, leaving the
                // deferred integer repairs off by the new pivot's own
                // dN · λ and forcing us back into the same problem next
                // epoch.  MW pair slips alone are NOT the full picture:
                // LLI sats carry only signal-less entries (filtered out
                // of slips_by_sat) yet were just obs-reseeded at reset
                // covariance, and TurboEdit GF-only flagged sats (the MW
                // blind spot — MW silent by definition) are mid-slip or
                // about to be reset by the GF-only pass; electing either
                // as the new SD anchor re-references the whole
                // constellation against a corrupt/fresh-reset state.
                std::set<std::string> exclude_sats;
                for (const auto& [sat_id, slips] : slips_by_sat) {
                    if (!slips.empty()) exclude_sats.insert(sat_id);
                }
                exclude_sats.insert(lli_sats.begin(), lli_sats.end());
                for (const auto& [te_sat, sigs] : te_culprits_by_sat) {
                    if (!sigs.empty()) exclude_sats.insert(te_sat);
                }
                exclude_sats.insert(te.reset_sats.begin(), te.reset_sats.end());

                for (const auto& cons : affected_cons) {
                    pivot_mgr.requestForcedHandover(
                        cons, "MW slip on BSSD pivot signal");
                }
                pivot_mgr.handlePivotHandover(
                    modeled_obs, state_manager, epoch_prefix,
                    /*forced_only=*/true, exclude_sats);

                const auto& pivots_after = pivot_mgr.getPivotSatellites();

                // Helper: did the pivot for this constellation actually
                // change?  If the handover refused (e.g., no viable
                // replacement above the elevation threshold) the old
                // pivot is retained and applying the integer in the
                // unchanged frame would still be off by dN_oldpivot · λ.
                // Fall back to reset in that case.
                auto handoverFired = [&](const std::string& cons) {
                    auto bit = pivots_before.find(cons);
                    auto ait = pivots_after.find(cons);
                    if (bit == pivots_before.end() || ait == pivots_after.end())
                        return false;
                    return bit->second != ait->second;
                };

                for (const auto& dr : deferred_integer_repairs) {
                    const std::string cons = PppUtils::getSystemString(dr.sat_id);
                    if (!handoverFired(cons)) {
                        log->warn("{}Pivot handover refused for {} (still {}); "
                                   "falling back to reset for {} {}",
                                   epoch_prefix, cons,
                                   pivots_after.count(cons) ? pivots_after.at(cons) : "<none>",
                                   dr.sat_id, dr.signal);
                        resetAmbiguityBssdAware(
                            dr.sat_id + "_" + dr.signal,
                            dr.sat_id, dr.signal,
                            modeled_obs, state_manager, meas_model, epoch_prefix);
                        continue;
                    }

                    const int amb_idx =
                        state_manager.getAmbiguityIndex(dr.sat_id + "_" + dr.signal);
                    if (amb_idx < 0) {
                        log->info("{}Deferred MW repair skipped: no ambiguity state for {} {}",
                                   epoch_prefix, dr.sat_id, dr.signal);
                        continue;
                    }

                    const PppObservation* obs_for_sat = nullptr;
                    for (const auto& obs : modeled_obs) {
                        if (obs.satellite == dr.sat_id) { obs_for_sat = &obs; break; }
                    }
                    if (!obs_for_sat) {
                        log->info("{}Deferred MW repair skipped: no observation for {} this epoch",
                                   epoch_prefix, dr.sat_id);
                        continue;
                    }
                    const double lambda_sig = ppp_utils_.getWavelength(
                        obs_for_sat->system, dr.signal, dr.sat_id);
                    if (!(std::isfinite(lambda_sig) && lambda_sig > 0.0)) {
                        log->info("{}Deferred MW repair skipped: invalid lambda for {} {}",
                                   epoch_prefix, dr.sat_id, dr.signal);
                        continue;
                    }

                    Eigen::VectorXd x = state_manager.getStateVector();
                    const double delta_m =
                        static_cast<double>(dr.dN_carrier_cycles) * lambda_sig;
                    const double prev = x(amb_idx);
                    x(amb_idx) += delta_m;
                    state_manager.setStateVector(x);

                    // This +delta lands in the NEW (post-forced-handover) SD
                    // frame, while the smoother's x_pred is the OLD frame; folding
                    // it into x_pred would leave a residual pivot offset.  The
                    // forced handover itself is disturbance-triggered (the pivot
                    // slipped) so the smoother segments this constellation anyway;
                    // record the sat as a reset so it is segmented even if the
                    // handover classification missed it (audit F1 / R4).
                    epoch_reset_sats_.insert(dr.sat_id);

                    log->info("{}Post-handover MW REPAIR for {}_{}: dN={} cyc, "
                              "lambda={:.4f} m, delta={:+.4f} m, x: {:.4f} -> {:.4f} "
                              "(new pivot={}, covariance preserved)",
                              epoch_prefix, dr.sat_id, dr.signal,
                              dr.dN_carrier_cycles, lambda_sig, delta_m,
                              prev, x(amb_idx),
                              pivots_after.count(cons) ? pivots_after.at(cons) : "<none>");
                }

                for (const auto& [sat_id, signal] : deferred_resets) {
                    resetAmbiguityBssdAware(
                        sat_id + "_" + signal,
                        sat_id, signal,
                        modeled_obs, state_manager, meas_model, epoch_prefix);
                    log->info("{}Post-handover targeted reset {}_{} (no clean per-pair integer)",
                               epoch_prefix, sat_id, signal);
                }
            }

            if (result.n_repaired > 0 || result.n_reset > 0 ||
                result.n_code_events > 0)
                log->warn("{}MW: {} targeted, {} full reset, {} code-event "
                          "veto(es)",
                          epoch_prefix, result.n_repaired,
                          result.n_reset, result.n_code_events);
        }
    }

    // --- TurboEdit GF-only detections (MW dN1==dN2 blind spot) ---
    // The dual-frequency MW-flagged case was already resolved inside the MW
    // per-satellite loop above (routed through the BSSD pivot-deferral
    // machinery).  This pass applies TurboEdit's remaining resolutions for
    // satellites the MW detector reported nothing on — slips where the
    // wide-lane is unchanged and only the geometry-free jump moved.  Satellites
    // already handled this epoch (in reset_sats) are skipped.
    if (turbo_) {
        applyTurboEditGfOnlyResolutions(te, reset_sats, modeled_obs,
                                        state_manager, meas_model, epoch_prefix, result);
    }

    // --- Auxiliary detectors (GRAPHIC / EWL / ...): reset on detection ---
    if (!aux_events.empty()) {
        applyAuxiliarySlipEvents(aux_events, reset_sats, modeled_obs,
                                 state_manager, meas_model, epoch_prefix, result);
    }

    // Advance the previous-epoch marker for the soft-LLI measured-spacing gate.
    soft_lli_.advanceEpoch(epoch_now_s);

    // Hand the slip-phase frame actions to the smoother via the result: reset
    // sats are segmented, covariance-preserving repairs are compensated into
    // the stored a-priori (audit F1).  Accumulated across the MW loop, the
    // GF-only pass, the aux pass, and the helper functions.
    result.reset_satellites = epoch_reset_sats_;
    result.ambiguity_repairs = epoch_repairs_;

    return result;
}

// ===========================================================================
// Auxiliary (non-MW) detector slip handling
// ===========================================================================

void CycleSlipManager::applyAuxiliarySlipEvents(
    const std::vector<SlipEvent>& events,
    std::set<std::string>& handled_sats,
    const std::vector<PppObservation>& modeled_obs,
    StateManager& state_manager,
    const MeasurementModel& meas_model,
    const std::string& epoch_prefix,
    CycleSlipEpochResult& result) {

    auto log = log_manager_.getLogger("cycle_slip_manager");

    for (const auto& ev : events) {
        const std::string sat = ev.satellite.empty()
            ? satIdFromAmbiguityKey(ev.amb_key) : ev.satellite;
        if (sat.empty()) continue;

        // Skip satellites the MW pipeline already handled this epoch — its
        // targeted, covariance-preserving per-signal repairs are authoritative;
        // stacking an auxiliary reset on top would throw away that precision.
        if (handled_sats.count(sat)) continue;

        auto resetSignal = [&](const std::string& signal) {
            if (signal.empty()) return;
            const std::string key = sat + "_" + signal;
            if (state_manager.getAmbiguityIndex(key) < 0) return;
            resetAmbiguityBssdAware(key, sat, signal, modeled_obs,
                                    state_manager, meas_model, epoch_prefix);
            ++result.n_reset;
            result.state_modified = true;
            log->info("{}{} slip: BSSD-aware reset {} (auxiliary detector)",
                      epoch_prefix, toString(ev.source), key);
        };

        if (ev.granularity == SlipGranularity::BandDifference) {
            // A band-difference detector (EWL) cannot, on its own, attribute
            // the jump to a single signal — reset both members of the pair.
            resetSignal(ev.sig1);
            resetSignal(ev.sig2);
        } else {
            resetSignal(ev.sig1.empty()
                            ? signalFromAmbiguityKey(ev.amb_key) : ev.sig1);
        }

        handled_sats.insert(sat);  // de-dupe across multiple auxiliary detectors
    }
}

// ===========================================================================
// TurboEdit GF-only (MW blind-spot) slip handling
// ===========================================================================

void CycleSlipManager::applyTurboEditGfOnlyResolutions(
    const TurboEditResolver::EpochResolution& te,
    std::set<std::string>& handled_sats,
    const std::vector<PppObservation>& modeled_obs,
    StateManager& state_manager,
    const MeasurementModel& meas_model,
    const std::string& epoch_prefix,
    CycleSlipEpochResult& result) {

    auto log = log_manager_.getLogger("cycle_slip_manager");

    // Group confident per-signal repairs by satellite so all of a satellite's
    // signals are applied before it is marked handled.
    std::map<std::string, std::vector<const TurboEditResolver::SignalRepair*>> by_sat;
    for (const auto& r : te.repairs) by_sat[r.satellite].push_back(&r);

    for (const auto& [sat, reps] : by_sat) {
        if (handled_sats.count(sat)) continue;  // MW pipeline already acted on this sat
        bool any = false;
        for (const auto* r : reps) {
            const std::string key = sat + "_" + r->signal;
            if (state_manager.getAmbiguityIndex(key) < 0) continue;
            // Covariance-preserving additive integer repair; repairAmbiguityBssdAware
            // refuses the BSSD pivot (and bails when the sat is unobserved / λ is
            // bad), in which case we reset that ambiguity instead.
            const bool ok = repairAmbiguityBssdAware(
                sat, r->signal, r->dN, modeled_obs, state_manager, meas_model, epoch_prefix);
            if (ok) {
                ++result.n_repaired;
                log->info("{}TurboEdit GF-only REPAIR {} {}: dN={:+d} cyc (MW blind spot; "
                          "covariance preserved)", epoch_prefix, sat, r->signal, r->dN);
            } else {
                resetAmbiguityBssdAware(key, sat, r->signal, modeled_obs,
                                        state_manager, meas_model, epoch_prefix);
                ++result.n_reset;
                log->info("{}TurboEdit GF-only repair refused for {} {} (pivot / unobserved); "
                          "reset instead", epoch_prefix, sat, r->signal);
            }
            any = true;
        }
        if (any) { result.state_modified = true; handled_sats.insert(sat); }
    }

    // Satellites the GF jump flagged but could not resolve confidently (iono
    // disturbed, non-integer split): reset every observed phase ambiguity.
    for (const auto& sat : te.reset_sats) {
        if (handled_sats.count(sat)) continue;
        const PppObservation* obs = nullptr;
        for (const auto& o : modeled_obs)
            if (o.satellite == sat) { obs = &o; break; }
        if (!obs) continue;
        bool any = false;
        for (const auto& [sig, val] : obs->observed) {
            (void)val;
            if (sig.empty() || sig[0] != 'L') continue;          // phase only
            if (sig.find('_') != std::string::npos) continue;     // raw signal only
            const std::string key = sat + "_" + sig;
            if (state_manager.getAmbiguityIndex(key) < 0) continue;
            resetAmbiguityBssdAware(key, sat, sig, modeled_obs,
                                    state_manager, meas_model, epoch_prefix);
            ++result.n_reset;
            any = true;
        }
        if (any) {
            result.state_modified = true;
            handled_sats.insert(sat);
            log->info("{}TurboEdit GF-only reset for {} (GF jump unresolved — "
                      "iono disturbed or non-integer split)", epoch_prefix, sat);
        }
    }
}

// ===========================================================================
// Soft-LLI bridge (estimation.cycle_slip.turbo_edit_bridge_lli) — delegation
// to the shared SoftLliBridge (mirrored by the real-time path).
// ===========================================================================

void CycleSlipManager::recordAmbiguitySnapshot(const StateManager& state_manager,
                                              double epoch_now_s) {
    soft_lli_.recordSnapshot(state_manager, epoch_now_s);
}

std::set<std::string> CycleSlipManager::getHeldPendingBridgeKeys() const {
    return soft_lli_.heldKeys();
}

void CycleSlipManager::resetSoftLliBridge() {
    soft_lli_.reset();
}

} // namespace GnssProcessor
