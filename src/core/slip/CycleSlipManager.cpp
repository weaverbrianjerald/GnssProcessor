/**
/**
* @file CycleSlipManager.cpp
* @brief Unified MW-only cycle slip detection and reset coordinator.
*
* Coordinates MW slip detection with multi-frequency cross-consistency
* analysis and targeted ambiguity resets.
*/

#include "slip/CycleSlipManager.hpp"
#include <cmath>
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
      mw_detector_(config, log_manager) {
}

// ===========================================================================
// Public API
// ===========================================================================

std::map<std::string, int> CycleSlipManager::getResolvedWlAmbiguities() const {
    return mw_detector_.getResolvedWlAmbiguities();
}

std::map<std::string, double> CycleSlipManager::getResolvedWlFractions() const {
    return mw_detector_.getResolvedWlFractions();
}

void CycleSlipManager::resetSatellite(const std::string& satellite) {
    mw_detector_.resetSatellite(satellite);
}

void CycleSlipManager::resetSatelliteSignal(const std::string& satellite,
                                              const std::string& signal) {
    mw_detector_.resetSatelliteSignal(satellite, signal);
}

void CycleSlipManager::notifyPivotForcedHandover(const std::string& satellite,
                                                  const std::string& reason) {
    mw_detector_.notifyPivotForcedHandover(satellite, reason);
}

void CycleSlipManager::setCorrections(const GnssCorrections* corrections) {
    mw_detector_.setCorrections(corrections);
}

bool CycleSlipManager::enableMwTracing(const std::filesystem::path& log_dir) {
    return mw_detector_.enableTracing(log_dir);
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

    // --- MW / LLI detection (all dual-frequency pairs including EWL) ---
    std::set<std::string> reset_sats;
    {
        const auto slip_results = mw_detector_.detectLargeMwResets(modeled_obs);

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
            for (const auto& stats_key : mw_detector_.getActiveArcKeys()) {
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
            for (const auto& sr : slip_results) {
                const std::string sat_id = sr.satellite.empty()
                    ? satIdFromAmbiguityKey(sr.amb_key) : sr.satellite;
                reset_keys_by_sat[sat_id].insert(sr.amb_key);
                // Cache one representative slip for post-estimation repair
                last_mw_slips_[sat_id] = sr;
            }

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
                    // treatment.
                    const auto pivot_culprits = identifySlippedSignals(
                        slips, all_pairs_by_sat[sat_id]);
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
                const auto& sat_all_pairs = all_pairs_by_sat[sat_id];
                const auto slips_it = slips_by_sat.find(sat_id);
                const bool has_pair_slips = slips_it != slips_by_sat.end()
                                         && !slips_it->second.empty();

                // --- Multi-frequency cross-consistency analysis ---
                // If this satellite has 3+ MW pairs (slipped + clean), try to
                // identify which individual signal slipped rather than resetting all.
                if (has_pair_slips && sat_all_pairs.size() >= 2
                    && slips_it->second.size() < sat_all_pairs.size()) {
                    const auto culprit_signals = identifySlippedSignals(
                        slips_it->second, sat_all_pairs);
                    if (!culprit_signals.empty()) {
                        // Compute per-(culprit signal) integer slips: when the
                        // slipped pair was wl_locked AND has a known integer,
                        // the lone-culprit interpretation gives an unambiguous
                        // signal-level shift.  Pairs with cross-consistent
                        // candidates qualify for repair (additive integer
                        // adjustment, covariance preserved); the rest fall
                        // back to reset.
                        const auto per_signal_slips = computeCulpritSlipIntegers(
                            slips_it->second, culprit_signals);

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
                                log->info("{}MW cross-consistency: targeted reset {} (culprit signal {})",
                                          epoch_prefix, targeted_key, culprit);
                            }
                            ++result.n_repaired;
                            result.state_modified = true;
                        }
                        reset_sats.insert(sat_id);
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
                    ++result.n_reset;
                    result.state_modified = true;
                }
                reset_sats.insert(sat_id);

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

                // Build the exclude set: every sat with any slipped MW
                // pair this epoch.  Passed to handlePivotHandover so
                // the forced-handover replacement search can never
                // pick another sat that is itself mid-slip — that
                // would simply move the broken SD-frame anchor from
                // one slipping sat to another, leaving the deferred
                // integer repairs off by the new pivot's own dN · λ
                // and forcing us back into the same problem next epoch.
                std::set<std::string> exclude_sats;
                for (const auto& [sat_id, slips] : slips_by_sat) {
                    if (!slips.empty()) exclude_sats.insert(sat_id);
                }

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

            if (result.n_repaired > 0 || result.n_reset > 0)
                log->warn("{}MW: {} targeted, {} full reset",
                          epoch_prefix, result.n_repaired,
                          result.n_reset);
        }
    }

    return result;
}

} // namespace GnssProcessor
