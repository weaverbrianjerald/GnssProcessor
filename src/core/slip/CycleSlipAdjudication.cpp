/**
 * @file CycleSlipAdjudication.cpp
 * @brief Section S11 of @c CycleSlipManager::detectAndRepair (one satellite's
 *        MW mandates adjudicated and acted on), its culprit-analysis helpers,
 *        and the per-epoch MW summary line (S14).
 *
 * Bodies moved verbatim from CycleSlipManager.cpp (card UOM step U3;
 * scripts/uom_u3_verify.py).
 */

#include "slip/CycleSlipManager.hpp"
#include "slip/CycleSlipEpochWork.hpp"
#include <algorithm>

namespace GnssProcessor {

std::map<std::string, int> CycleSlipManager::computeCulpritSlipIntegers(
    const std::vector<MwSlipResult>& slipped_pairs,
    const std::set<std::string>& culprits) const {

    auto log = log_manager_.getLogger("cycle_slip_manager");

    // For each culprit signal, gather slip integer estimates from the
    // slipped pairs in which it is the only culprit signal.
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
    // contributes evidence.  Disagreement means the cross-consistency
    // analysis or the slip-integer rounding is wrong for this signal, so
    // the signal is disqualified from repair and falls back to reset.
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
        // All pairs slipped, so there is no clean reference: inconclusive.
        return {};
    }

    // The culprits are the signals that appear in slipped pairs and in no clean pair.
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
// S11: one satellite's MW mandates
// ===========================================================================

void CycleSlipManager::handleSatelliteMandates(SlipEpochWork& w,
                                               const std::string& sat_id,
                                               const std::set<std::string>& amb_keys) {
    const auto& log           = w.log;
    const auto& epoch_prefix  = w.epoch_prefix;
    const auto& modeled_obs   = w.modeled_obs;
    auto& state_manager       = w.state_manager;
    const auto& meas_model    = w.meas_model;
    const auto& epoch_now_s   = w.epoch_now_s;
    auto& result              = w.result;
    const auto& te            = w.te;
    const auto& te_culprits_by_sat = w.te_culprits_by_sat;
    auto& te_slips_by_sat     = w.te_slips_by_sat;
    auto& te_suppress_by_sat  = w.te_suppress_by_sat;
    const auto& soft_lli_repaired = w.soft_lli_repaired;
    auto& reset_sats          = w.reset_sats;
    const auto& slips_by_sat  = w.slips_by_sat;
    auto& all_pairs_by_sat    = w.all_pairs_by_sat;
    const auto& lli_sats      = w.lli_sats;
    auto& epoch_reset_amb_keys = w.epoch_reset_amb_keys;
    const auto& pivot_affected_cons_sig = w.pivot_affected_cons_sig;
    auto& deferred_integer_repairs = w.deferred_integer_repairs;
    auto& deferred_resets     = w.deferred_resets;

    // Soft-LLI bridge already integer-repaired this satellite's
    // held ambiguities in the pre-pass (snapshot+dN·λ); its MW
    // mandate (typically wl_known=false → full reset) does not
    // override that covariance-restoring repair.
    if (soft_lli_repaired.count(sat_id)) return;
    // WL-settle deferral: the resolver is adjudicating this
    // satellite's across-gap event against a settling MW
    // estimate.  Its mandate is suppressed (the holds keep the
    // affected phase out of the filter), and its GF arcs are left
    // untouched until the deferral resolves to a repair, reset,
    // or timeout in a later epoch.
    if (te.pending_sats.count(sat_id)) {
        log->info("{}MW mandate for {} suppressed: TurboEdit "
                  "WL-settle deferral pending (holds active)",
                  epoch_prefix, sat_id);
        return;
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
    // the same repair and BSSD pivot-deferral logic below;
    // culprits with a cross-consistent integer are repaired
    // (additive, covariance preserved), the rest are reset.
    // LLI authority: a satellite with a receiver-declared loss
    // of lock takes the full-reset fall-through unconditionally.
    // No MW pair evidence exists on an LLI epoch (the detector
    // skips pair processing), so cross-consistency cannot fire,
    // and TurboEdit's GF-only resolution is not adopted because
    // its ΔN_WL = 0 premise cannot be checked across a lock loss.
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
    // flags two MW pairs at once on a 3-freq sat) can give
    // cross-consistency a "culprit" whose phase the resolver's
    // GF evidence adjudicated as affirmatively still.  The worst
    // case is a covariance-preserved integer repair onto a phase
    // that never moved (the second pair's unusable integer leaves
    // a single "consistent" candidate).  The two readings cannot
    // both hold, and for repairs the cost asymmetry is reversed,
    // so contradictory evidence demotes the satellite to the
    // conservative full-reset fall-through, atomically: both the
    // culprits and the code-event adjudication are withdrawn.
    // (TE-sourced culprits cannot contradict, because the
    // resolver's staging already demotes repair-vs-zero
    // conflicts to reset_sats and withdraws the events; the
    // guard is cheap and general.)
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
                    // Defer: the integer adjustment is applied
                    // post-handover in the new SD frame.  The
                    // state vector is not touched now because the
                    // handover transform expects the pre-slip SD
                    // values (the old pivot's frozen 0 is the
                    // implicit reference); applying dN here in the
                    // old frame would be off by dN_pivot · λ.
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
            // (GF arc in warm-up, per-signal dropout) still
            // carries an un-adjudicated mandate, which takes its
            // reset here instead of being dropped by the
            // `continue` below.  This also covers the combined-IF
            // mode: per-raw-signal culprit keys do not exist in
            // the IF state (the repair above does nothing for
            // them), but the IF amb_key arrives here
            // un-adjudicated and is reset, so the slip is not
            // lost in that mode.
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
            return;
        }
    }

    // --- TurboEdit no-state-action adjudications ---------------
    // No culprit resolution exists for this satellite, but the
    // resolver adjudicated its MW pair flags as needing no state
    // action: a code-domain artifact (GF affirmatively stable;
    // the event is in the code observable), a gap-bridge moot
    // attribution (the across-gap slip was already absorbed by
    // the prune-and-reseed on the signal's return), or a
    // confidently clean partner (dN == 0).  The mandated phase
    // resets for the adjudicated signals are suppressed instead
    // of taking the full-reset fall-through.  The KF is the
    // backstop, and the cost asymmetry favours suppression: a
    // wrongly suppressed real slip puts at least 0.73 m (3λ2 on
    // L2, 4λ1 ≈ 0.76 m on L1 for the (4,3) pair; see
    // TurboEditResolver.hpp) into that signal's phase innovation
    // at this epoch's update and
    // takes the residual-gate reset one epoch late, whereas a
    // wrongly honoured mandate discards converged ambiguities
    // (and AR continuity) immediately.  Mandates on signals
    // outside the adjudication (a pair whose GF arc was in
    // warm-up or per-signal dropout, combined-IF amb_keys) still
    // take their reset, as in the residual-mandate rule above.
    // LLI sats do not reach this block (gated above): a
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
            return;
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
    // is preserved across LLI on purpose (observation-domain
    // history; see the detector's LLI branch), but a GF arc
    // carries a level: a slip left in prev_gf would reappear in
    // the next epoch's TD2 with flipped sign at the epoch MW
    // flags the pair with the true integer, and the joint solve
    // would then emit a confident wrong repair onto the
    // just-reseeded ambiguity.
    //
    // Exception, the soft-LLI bridge (turbo_edit_bridge_lli): for
    // an AR-capable, non-pivot satellite at high rate, the
    // ambiguities are held across the short LLI gap and the
    // bridge integer-repairs them at gap return.  That needs the
    // GF arc's pre-gap level, so the arc is not reset here; the
    // hold registration snapshots the pre-gap values and marks
    // the signals held-continuous in the resolver.  The BSSD
    // pivot is excluded (its SD ambiguity is frozen at 0; a pivot
    // LLI needs a handover, not an in-frame integer shift).
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
            // GF arc kept on purpose (no resetSatellite).
        } else {
            turbo_->resetSatellite(sat_id);
        }
    }

    // --- Sat-wide state re-inflation ---
    // Runs after the full-reset fall-through above, which resets
    // every MANDATED key of the satellite: all its phase
    // ambiguities when the mandate covers them, a subset when it
    // does not (e.g. a triple-frequency satellite whose only
    // mature MW pair slipped).  The re-inflation is
    // satellite-wide either way.
    // When every phase ambiguity of a satellite has been
    // reset, the new ambiguities reseed against the current
    // (iono, sat-bias) state and inherit whatever partition
    // existed before the reset.  In uncombined PPP-RTK that
    // partition can absorb slant iono non-uniquely (the iono
    // state holds I_true − δ/α, the sat-bias holds b_true + δ).
    // Without re-inflating the iono and bias variances here,
    // the wrong partition persists for the rest of the arc and
    // the L5 / E5* / B2a code residual carries the unmodelled
    // component for hundreds of epochs.
    //
    // BSSD pivot sats are skipped: PivotManager freezes their
    // iono and bias states at (0, 0), so a reinflation would
    // either do nothing (the diag==0 guard in StateManager)
    // or break the BSSD frame.
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
                // Constant-velocity model: decorrelate the companion
                // rate too (no-op unless enableIonoRate).  0.0 => the
                // initial_std.ionosphere_rate default variance.
                state_manager.reinflateIonosphereRateVariance(sat_id, 0.0);
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

// ===========================================================================
// S14: the per-epoch MW summary line
// ===========================================================================

void CycleSlipManager::logMwEpochSummary(const SlipEpochWork& w) const {
    const auto& log          = w.log;
    const auto& epoch_prefix = w.epoch_prefix;
    const auto& result       = w.result;

    if (result.n_repaired > 0 || result.n_reset > 0 ||
        result.n_code_events > 0)
        log->warn("{}MW: {} targeted, {} full reset, {} code-event "
                  "veto(es)",
                  epoch_prefix, result.n_repaired,
                  result.n_reset, result.n_code_events);
}

} // namespace GnssProcessor
