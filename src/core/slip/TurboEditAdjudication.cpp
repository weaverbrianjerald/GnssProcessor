/**
 * @file TurboEditAdjudication.cpp
 * @brief TurboEdit pass 2: steps R5 to R7 of @c TurboEditResolver::resolve.
 *
 * R5 adjudicates one pass-1 row at a time (@c adjudicateRow), in six steps
 * over one @c RowWork:
 *   - A0 @c openRow: the row's evidence (IVI, TD2 noise σ, split gate, MW
 *     flag, GF flag);
 *   - A1 @c rerouteLateAcrossGap: a late MW flag or a pending WL-settle
 *     deferral re-routed to the across-gap jump (TurboEditBridge.cpp);
 *   - A2 @c adjudicateBridged (TurboEditBridge.cpp) for a bridged row, or
 *     A3 @c adjudicateFlagged for a flagged one: the MW-only code-domain veto
 *     (@c classifyCodeEvent), then ignore, reset or the joint split;
 *   - A4 @c traceRow: one row of turbo_edit_trace.csv;
 *   - A5 @c advanceArc: re-seed the chain after a slip, or roll it forward.
 * Every adjudication stages its per-signal integers through
 * @c stageInteger.  R6 (@c reconcileStagedIntegers) turns the staging into
 * repairs, clean signals and moot signals per satellite, and R7
 * (@c withdrawContradictedVerdicts) withdraws the code events and pending
 * deferrals of satellites that ended the epoch in a reset.
 */

#include "slip/TurboEditResolver.hpp"
#include "slip/TurboEditEpochWork.hpp"
#include "math/TimeUtils.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>

namespace GnssProcessor {

// =====================================================================
// R5: one row of pass 2
// =====================================================================

// --- Pass 2: flag (MW ∪ GF-only), joint solve, update arc ---
void TurboEditResolver::adjudicateRow(EpochWork& w, const PairEpoch& cached) {
    RowWork row(cached, gf_arcs_[cached.key]);
    openRow(w, row);                                   // A0  evidence
    rerouteLateAcrossGap(w, row);                      // A1  late across-gap re-route
    if (row.pe.bridged) adjudicateBridged(w, row);     // A2  gap-bridge adjudication
    else if (row.flagged) adjudicateFlagged(w, row);   // A3  MW or GF flag
    traceRow(row);                                     // A4  the trace row
    advanceArc(w, row);                                // A5  the arc update
}

void TurboEditResolver::openRow(const EpochWork& w, RowWork& row) const {
    const auto& ivi_by_cons = w.ivi_by_cons;
    const auto& mw_by_pair = w.mw_by_pair;
    const auto& pe = row.pe;
    const auto& arc = row.arc;
    const auto& sat = row.sat;
    auto& ivi = row.ivi;
    auto& iono_stable_cons = row.iono_stable_cons;
    auto& split_gate_m = row.split_gate_m;
    auto& sigma_trace = row.sigma_trace;
    auto& mwit = row.mwit;
    auto& mw_flagged = row.mw_flagged;
    auto& gf_flagged = row.gf_flagged;
    auto& flagged = row.flagged;

    const char cons = sat[0];
    ivi = ivi_by_cons.count(cons) ? ivi_by_cons.at(cons) : 0.0;
    iono_stable_cons = (ivi < kIviThreshold);

    // Rate-adaptive evidence for this arc (header's "Rate adaptivity"
    // note): the raw TD2 noise σ and the lattice spacing it must
    // discriminate.  sigma_trace is overwritten with the gap-widened
    // value on bridged adjudications; both feed the trace.
    const double denom_row    = pe.lambda1 - pe.lambda2;
    split_gate_m = split_max_sigma_ * std::abs(denom_row);
    sigma_trace = effectiveSigma(arc, pe.key, pe.obs->elevation);

    mwit = mw_by_pair.find(pe.key);
    mw_flagged = (mwit != mw_by_pair.end());
    gf_flagged = pe.gf_exceeds && arc.cooldown_remaining == 0;
    flagged    = mw_flagged || gf_flagged;
}

void TurboEditResolver::stageInteger(EpochWork& w, const std::string& sat,
                                     const std::string& sig, int dN, double lam,
                                     double fr) {
    auto& cand = w.cand;
    auto& conflict_sats = w.conflict_sats;

    // Per-(sat, signal) integer staging, reconciled across the sat's
    // pairs (a disagreement on a shared signal demotes the whole sat
    // to a reset).  Shared by the joint-solve repair branch, the
    // code-event veto (affirmative zeros), and the gap-bridge dispatch.
    auto& sigmap = cand[sat];
    auto it = sigmap.find(sig);
    if (it == sigmap.end()) {
        sigmap[sig] = StagedInteger{ dN, lam, fr };
    } else if (it->second.dN != dN) {
        conflict_sats.insert(sat);  // disagreement across pairs
    }
}

// =====================================================================
// A3: an MW- or GF-flagged row with a TD2 of its own
// =====================================================================

void TurboEditResolver::adjudicateFlagged(EpochWork& w, RowWork& row) const {
    const auto& log = w.log;
    const auto& mw_clean_pairs = w.mw_clean_pairs;
    auto& out = w.out;
    const auto& pe = row.pe;
    const auto& sat = row.sat;
    const auto& mwit = row.mwit;
    const auto& mw_flagged = row.mw_flagged;
    const auto& iono_stable_cons = row.iono_stable_cons;
    const auto& sigma_trace = row.sigma_trace;
    const auto& split_gate_m = row.split_gate_m;
    auto& did_slip = row.did_slip;
    auto& dN1 = row.dN1;
    auto& dN2 = row.dN2;
    auto& frac2 = row.frac2;
    auto& dN_wl_trace = row.dN_wl_trace;
    auto& wl_known_trace = row.wl_known_trace;
    auto& action = row.action;

    int  dN_wl   = 0;
    bool wl_known = true;
    if (mw_flagged) {
        if (mwit->second.known) dN_wl = mwit->second.dN_wl;
        else                    wl_known = false;  // MW slip with no integer → can't split
    } else if (mw_clean_pairs.count(pe.key) == 0) {
        // GF-only flag without affirmative MW-clean evidence for the
        // pair this epoch.  MW silence does not prove ΔN_WL = 0: a
        // wl-locked arc absorbs a real slip into Welford without
        // emitting a result, and an immature (post-gap, cold-start)
        // arc cannot flag at all.  Forcing ΔN_WL = 0 in those states
        // aliases unequal slips into confident wrong dual-signal
        // repairs (GPS L1/L2: λ1/(λ1−λ2) ≈ −3.53 puts ΔN_WL = ±2, ±4,
        // ±6 at frac ≈ 0.06, 0.12, 0.18, inside kMaxFractional).  The
        // wide-lane is treated as unknown, which leads to the reset
        // fallback below.
        wl_known = false;
    }  // else GF-only with MW affirmatively clean → ΔN_WL = 0 holds
    dN_wl_trace = dN_wl;
    wl_known_trace = wl_known;

    const double denom = pe.lambda1 - pe.lambda2;

    const bool code_event = classifyCodeEvent(w, row, dN_wl, wl_known, denom);

    const bool split_capable = splitCapable(sigma_trace, denom);

    if (code_event) {
        // Handled above.
    } else if (!mw_flagged && !split_capable) {
        // GF-only exceedance on an arc that is not split-capable (the
        // coarse-rate iono residual at lattice scale, or a noisy
        // arc): not attributable to per-signal integers and with no
        // MW mandate to honor, so it is ignored.  A reset here would
        // turn every coarse-rate iono transient into a reset that
        // discards converged ambiguities; a genuine equal slip, with
        // a phase-innovation signature of |dN·λ| ≥ 19 cm, is left to
        // the KF residual backstop.  The arc keeps rolling and the
        // exceeding TD2 stays out of the MAD window as usual.
        action = "coarse_gf";
        log->info("TurboEdit {} {}/{}: GF-only exceedance "
                  "(|TD2|={:.4f} m) on a split-incapable arc "
                  "(σ={:.4f} m > {:.2f}×|λ1−λ2|={:.4f} m) — ignored; "
                  "KF residual backstop governs",
                  sat, pe.sig1, pe.sig2, std::abs(pe.td2),
                  sigma_trace, split_max_sigma_, split_gate_m);
    } else if (!iono_stable_cons || !wl_known || std::abs(denom) < 1e-6) {
        // GF jump untrustworthy (iono) / no WL integer / degenerate pair → reset.
        out.reset_sats.insert(sat);
        did_slip = true;
        action = "reset";
    } else if (!split_capable) {
        // The MW mandate stands but the GF evidence cannot split it
        // into per-signal integers (rate-adaptive gate): conservative
        // reset, as without TurboEdit.
        out.reset_sats.insert(sat);
        did_slip = true;
        action = "reset";
        log->info("TurboEdit {} {}/{}: MW flagged (dN_WL={:+d}{}) but "
                  "GF noise too coarse for the per-signal split "
                  "(σ={:.4f} m > {:.2f}×|λ1−λ2|={:.4f} m) — reset "
                  "fallback",
                  sat, pe.sig1, pe.sig2, dN_wl,
                  wl_known ? "" : ", integer unknown",
                  sigma_trace, split_max_sigma_, split_gate_m);
    } else {
        const double dN2_f = (pe.td2 - pe.lambda1 * static_cast<double>(dN_wl)) / denom;
        dN2 = static_cast<int>(std::llround(dN2_f));
        frac2 = std::abs(dN2_f - static_cast<double>(dN2));
        dN1 = dN2 + dN_wl;
        const bool sane = std::abs(dN1) <= kMaxSlipCycles &&
                          std::abs(dN2) <= kMaxSlipCycles;

        if (frac2 <= kMaxFractional && sane && (dN1 != 0 || dN2 != 0)) {
            stageInteger(w, sat, pe.sig1, dN1, pe.lambda1, frac2);
            stageInteger(w, sat, pe.sig2, dN2, pe.lambda2, frac2);
            did_slip = true;
            action = "repair";
        } else if (dN1 == 0 && dN2 == 0) {
            // Resolved to "no slip" (e.g. GF-only false alarm); the arc keeps rolling.
            action = "none";
        } else {
            out.reset_sats.insert(sat);
            did_slip = true;
            action = "reset";
        }
    }
}

bool TurboEditResolver::classifyCodeEvent(EpochWork& w, RowWork& row, int dN_wl,
                                          bool wl_known, double denom) const {
    const auto& log = w.log;
    auto& out = w.out;
    const auto& pe = row.pe;
    const auto& arc = row.arc;
    const auto& sat = row.sat;
    const auto& mw_flagged = row.mw_flagged;
    const auto& iono_stable_cons = row.iono_stable_cons;
    auto& dN1 = row.dN1;
    auto& dN2 = row.dN2;
    auto& frac2 = row.frac2;
    auto& action = row.action;

    // --- MW-only code-domain veto (GF-stable non-slip) -----------
    // MW flagged the pair but the GF jump is affirmatively zero: |TD2|
    // lies within the arc's own measured noise (kCodeEventZeroSigma ×
    // MAD, tighter than the detection gate and, on quiet arcs, tighter
    // than the kMinThresholdM quantum floor, so a genuine (4,3)-class
    // GF jump of ~2.9 cm for GPS L1/L2 still reaches the joint solve
    // below).  With the phase still, no plausible integer pair fits
    // the MW step (see the header's null-line geometry), and the
    // event is classified as a code-domain artifact: both signals are
    // adjudicated clean, the caller skips the phase reset, and the KF
    // phase innovation (standardized residual gate) backstops the
    // remaining null-line-pair risk.
    // Eligibility requires every trust gate the GF evidence rests on:
    // a zero gate that can discriminate (3.5σ at or below
    // kVetoZeroGateMaxLambdaFrac × |λ1−λ2|; at coarse rates the
    // un-cancelled iono residual inflates the MAD until "zero" cannot
    // be told apart from a small slip or a near-null-line pair, and
    // the veto then does not apply), an IVI-stable constellation, a
    // mature arc MAD (a warmup arc cannot affirm zero), no post-slip
    // cooldown echo, and no receiver-declared loss of lock on either
    // signal (an LLI corroborates the slip, and the caller's LLI
    // mandate governs).
    // wl_known is not required: a non-integer MW excursion with a
    // still phase is, if anything, more code-like.
    bool code_event = false;
    if (mw_only_veto_ && mw_flagged && !pe.gf_exceeds &&
        arc.cooldown_remaining == 0 && iono_stable_cons &&
        !turbo_edit::pairHasLockLoss(*pe.obs, pe.sig1, pe.sig2)) {
        // Affirming zero requires more MAD maturity than detection
        // (kCodeEventMinMadSamples > kMinEpochsForAdaptive): a 5-sample
        // MAD has too much estimator variance to support a
        // state-preserving decision.
        const double mad_sigma =
            (arc.diff_window.size() >= kCodeEventMinMadSamples)
                ? computeMadSigma(arc.diff_window)
                : std::numeric_limits<double>::quiet_NaN();
        const double zero_gate = kCodeEventZeroSigma * mad_sigma;
        const double zero_cap =
            kVetoZeroGateMaxLambdaFrac * std::abs(denom);
        if (std::isfinite(zero_gate) && zero_gate > 0.0 &&
            zero_gate <= zero_cap &&
            std::abs(pe.td2) <= zero_gate) {
            code_event = true;
            // Diagnostics only: the null-line pair the measured GF
            // would imply, traced so the implausible magnitude
            // ((9,7), (4,3), ...) is visible; it is never applied.
            if (wl_known && std::abs(denom) >= 1e-6) {
                const double dN2_f =
                    (pe.td2 - pe.lambda1 * static_cast<double>(dN_wl)) / denom;
                dN2 = static_cast<int>(std::llround(dN2_f));
                frac2 = std::abs(dN2_f - static_cast<double>(dN2));
                dN1 = dN2 + dN_wl;
            }
            stageInteger(w, sat, pe.sig1, 0, pe.lambda1, frac2);
            stageInteger(w, sat, pe.sig2, 0, pe.lambda2, frac2);
            out.code_events.push_back({ sat, pe.sig1, pe.sig2,
                                        dN_wl, wl_known, pe.td2, zero_gate });
            action = "code_event";
            log->info("TurboEdit code-domain event {} {}/{}: MW flagged "
                      "(dN_WL={:+d}{}) but GF affirmatively stable "
                      "(|TD2|={:.4f} m <= {:.4f} m) — classified as code "
                      "artifact (implied null-line pair dN1={:+d}, dN2={:+d}); "
                      "phase preserved, KF residual gate is the backstop",
                      sat, pe.sig1, pe.sig2, dN_wl,
                      wl_known ? "" : ", integer unknown",
                      pe.td2, zero_gate, dN1, dN2);
            // did_slip stays false: the GF chain did not jump.  It
            // keeps rolling so the next epoch's veto (a persistent
            // code excursion re-flags after the MW arc restarts at
            // the new level) rests on live evidence, and the clean
            // TD2 feeds the MAD window below as usual.
        }
    }
    return code_event;
}

// =====================================================================
// A4, A5: the trace row and the arc update
// =====================================================================

void TurboEditResolver::traceRow(const RowWork& row) const {
    const auto& pe = row.pe;
    const auto& sat = row.sat;
    const auto& ivi = row.ivi;
    const auto& iono_stable_cons = row.iono_stable_cons;
    const auto& mw_flagged = row.mw_flagged;
    const auto& gf_flagged = row.gf_flagged;
    const auto& dN_wl_trace = row.dN_wl_trace;
    const auto& wl_known_trace = row.wl_known_trace;
    const auto& dN1 = row.dN1;
    const auto& dN2 = row.dN2;
    const auto& frac2 = row.frac2;
    const auto& action = row.action;
    const auto& sigma_trace = row.sigma_trace;
    const auto& split_gate_m = row.split_gate_m;

    // Trace row.
    if (!trace_file_.is_open()) return;
    trace_file_ << std::fixed << std::setprecision(3)
                << current_epoch_.week << "," << current_epoch_.tow << ","
                << sat << "," << PppUtils::getSystemString(pe.obs->system) << ","
                << std::setprecision(2) << pe.obs->elevation << ","
                << pe.sig1 << "," << pe.sig2 << ","
                << std::setprecision(6) << pe.lambda1 << "," << pe.lambda2 << ","
                << pe.gf << "," << pe.td2 << ","
                << (std::isfinite(pe.thresh) ? pe.thresh : -1.0) << ","
                << std::setprecision(4) << ivi << "," << (iono_stable_cons ? 1 : 0) << ","
                << (mw_flagged ? 1 : 0) << "," << (gf_flagged ? 1 : 0) << ","
                << dN_wl_trace << "," << (wl_known_trace ? 1 : 0) << ","
                << dN1 << "," << dN2 << ","
                << std::setprecision(3) << frac2 << "," << action << ","
                << std::setprecision(6)
                << (std::isfinite(pe.thresh_unfloored) ? pe.thresh_unfloored
                                                       : -1.0)
                << "," << (pe.bridged ? 1 : 0) << ","
                // Rate-adaptive evidence: the σ the gates were
                // scaled by (gap-widened on bridged rows; -1 = no
                // estimate) and the split-capability bound.
                << (std::isfinite(sigma_trace) ? sigma_trace : -1.0)
                << "," << split_gate_m
                << "\n";
}

void TurboEditResolver::advanceArc(const EpochWork& w, RowWork& row) {
    const auto& epoch_now_s = w.epoch_now_s;
    const auto& pe = row.pe;
    auto& arc = row.arc;
    const auto& did_slip = row.did_slip;

    // Arc update.
    if (did_slip) {
        // The chain re-seeds; the MAD window is preserved (the same
        // calibration-versus-chain split as the short-gap partial reset).
        // The level jump does not change the TD2 noise, and the window
        // holds only clean (non-exceeding) TD2s, so wiping it would cost
        // kMinEpochsForAdaptive (5) epochs without the arc's own threshold
        // after every slip, with no statistical reason.  With the window
        // intact, post-slip epochs re-arm at the arc's own calibration;
        // the cross-sat floor covers only genuinely fresh arcs.
        //
        // last_epoch_s is stamped (not zeroed) because the window
        // survives: it is the gap clock that lets the absolute-gap
        // trigger expire the preserved calibration.  If a satellite
        // slips, sets, and rises hours later, the stamped time makes
        // absolute_gap fire on return and wipe the arc.  Without it
        // (count==0 and last_epoch_s==0 disarm both gap triggers), an
        // hours-old MAD would govern the mature branch in a different
        // elevation and multipath regime.
        arc.prev_gf = 0.0; arc.prev_prev_gf = 0.0;
        arc.count = 0;
        arc.last_epoch_s = epoch_now_s;
        arc.cooldown_remaining = kCooldownEpochs;  // suppress the TD2 echo next epoch
    } else {
        // Only clean (non-outlier) TD2s enter the MAD window, so a
        // suppressed exceedance cannot inflate the threshold.  A bridged
        // value is an across-gap jump, not a TD2 sample, and is never
        // admitted.
        if (!pe.gf_exceeds && !pe.bridged) {
            arc.diff_window.push_back(pe.td2);
            if (arc.diff_window.size() > kGfWindowSize) arc.diff_window.pop_front();
        }
        const double ep_s = TimeUtils::continuousGpsSeconds(pe.obs->time);
        // GF-rate EWMA (gap-bridge drift correction), consecutive epochs
        // only.  Any chain break already re-seeded in pass 1, so an arc
        // that reaches this point with count >= 1 is consecutive.
        if (arc.count >= 1 && arc.last_epoch_s > 0.0 && ep_s > arc.last_epoch_s) {
            const double dt = ep_s - arc.last_epoch_s;
            const double rate = (pe.gf - arc.prev_gf) / dt;
            arc.rate_ewma_mps = std::isfinite(arc.rate_ewma_mps)
                ? kBridgeRateAlpha * rate +
                      (1.0 - kBridgeRateAlpha) * arc.rate_ewma_mps
                : rate;
            arc.last_spacing_s = dt;
        }
        ++arc.count;
        arc.prev_prev_gf = arc.prev_gf;
        arc.prev_gf      = pe.gf;
        arc.last_epoch_s = ep_s;
        if (arc.cooldown_remaining > 0) --arc.cooldown_remaining;
    }
}

// =====================================================================
// R6, R7: the staged integers into the resolution
// =====================================================================

void TurboEditResolver::reconcileStagedIntegers(EpochWork& w) {
    const auto& cand = w.cand;
    const auto& cand_moot = w.cand_moot;
    const auto& conflict_sats = w.conflict_sats;
    auto& out = w.out;

    // --- Reconcile candidates into repairs (per-sat atomic) ---
    for (const auto& s : conflict_sats) out.reset_sats.insert(s);

    for (const auto& [sat, sigmap] : cand) {
        if (out.reset_sats.count(sat)) continue;  // a failed/conflicting pair → reset whole sat
        const auto moot_it = cand_moot.find(sat);
        for (const auto& [sig, staged] : sigmap) {
            if (staged.dN == 0) {
                // Adjudicated by a confident joint solve: this signal did
                // not slip.  Reported so the caller can exempt it from its
                // own (pair-granular) reset mandates.
                out.clean_signals[sat].insert(sig);
                continue;
            }
            if (moot_it != cand_moot.end() && moot_it->second.count(sig)) {
                // Bridged attribution on a signal that was reseeded during
                // the gap: the fresh seed already absorbs the slip, so the
                // mandate is suppressed and no state changes.
                out.moot_signals[sat].insert(sig);
                continue;
            }
            SignalRepair r;
            r.satellite = sat;
            r.signal    = sig;
            r.dN        = staged.dN;
            r.lambda    = staged.lambda;
            r.frac      = staged.frac;
            out.repairs.push_back(std::move(r));
        }
    }
}

void TurboEditResolver::withdrawContradictedVerdicts(EpochWork& w) {
    auto& out = w.out;

    // Withdraw code events for satellites that ended the epoch in
    // reset_sats (another pair required a reset, or a staged-integer
    // conflict demoted the sat).  Contradictory evidence resolves to the
    // conservative outcome atomically per satellite, as in the emission loop
    // above, which already suppresses such a sat's clean_signals.  Otherwise
    // the caller would skip the reset mandate this resolution just issued.
    if (!out.code_events.empty() && !out.reset_sats.empty()) {
        out.code_events.erase(
            std::remove_if(out.code_events.begin(), out.code_events.end(),
                           [&out](const CodeEvent& ce) {
                               return out.reset_sats.count(ce.satellite) > 0;
                           }),
            out.code_events.end());
    }

    // A satellite that another pair demoted to reset does not also ask the
    // caller to suppress its mandates: its pending adjudication is withdrawn.
    // The reset takes precedence; the arcs, and the pending flag with them,
    // are erased when the caller forwards resetSatellite.
    if (!out.pending_sats.empty()) {
        for (auto it = out.pending_sats.begin(); it != out.pending_sats.end(); )
            it = out.reset_sats.count(*it) ? out.pending_sats.erase(it)
                                           : std::next(it);
    }
}

} // namespace GnssProcessor
