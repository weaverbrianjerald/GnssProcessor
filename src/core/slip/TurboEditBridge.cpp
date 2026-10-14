/**
 * @file TurboEditBridge.cpp
 * @brief The TurboEdit gap bridge inside pass 2 (the header's "Gap-spanning
 *        GF bridge" note): steps A1 and A2 of a row.
 *
 *   - A1 @c rerouteLateAcrossGap: a row whose chain already has its own TD2
 *     but whose pre-gap stash is still valid, with a late MW flag or a
 *     pending WL-settle deferral, is re-routed to the drift-corrected
 *     across-gap jump (or the deferral is aborted on a new chain event);
 *   - A2 @c adjudicateBridged: for a bridged row with an MW flag or a pending
 *     deferral, the trust gates, then (@c adjudicateTrustedBridge) the
 *     code-domain event, the coarse decline, or the across-gap candidate
 *     solve in three steps: @c collectBridgeWlCandidates (the WL integer
 *     candidates and their source), @c selectBridgeCandidate (the
 *     fractional, GF-separation and sibling-affirmation gates) and
 *     @c applyBridgeSolve (the unique solution dispatched per signal by
 *     continuity, the WL-settle deferral in @c deferBridgeAdjudication, or
 *     the conservative reset);
 *   - @c affirmedZeroSignals: the sibling-pair zero affirmation the
 *     candidate gates use.
 */

#include "slip/TurboEditResolver.hpp"
#include "slip/TurboEditEpochWork.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace GnssProcessor {

// =====================================================================
// The sibling-pair zero affirmation
// =====================================================================

// --- Sibling-pair zero affirmation (gap-bridge constraint) -----------
// The GF equation is degenerate along the null-line lattice: bridged
// candidates k and k±2 can fit the same across-gap jump within mm, and
// only the MW evidence (biased by code noise at re-acquisition) separates
// them.  Another GF arc of the same satellite often carries the missing
// information.  An arc that (a) has cross-frequency leverage, (b) stayed
// chain-continuous through the bridged interval (no break, so no blind
// gap; a slip on either signal at any epoch of the interval would have
// flagged its TD2), (c) is calibration-mature, and (d) has no current
// exceedance or cooldown affirms dN = 0 for both of its signals.
// Example: an L1C/L2L chain that runs unbroken through an L2W outage
// affirms dN(L1C) = 0, which selects k = +28, (0, −28) over the
// float-favored k = +26, (−9, −35).  Residual risk: a simultaneous
// null-line pair on the sibling itself is invisible to GF, the same
// documented exposure as the code-event veto, backstopped by the KF
// innovation gate.
std::set<std::string> TurboEditResolver::affirmedZeroSignals(
    const EpochWork& w, const std::string& sat, const std::string& exclude_key,
    double ref_epoch_s) const {
    const auto& pe_by_key = w.pe_by_key;

    std::set<std::string> affirmed;
    const std::string prefix = sat + "_";
    for (const auto& [k2, row2] : pe_by_key) {
        if (k2 == exclude_key) continue;
        if (k2.rfind(prefix, 0) != 0) continue;
        const PairEpoch& q = *row2;
        if (q.bridged) continue;                 // its own chain broke
        if (q.gf_exceeds) continue;              // flagged this epoch
        if (std::abs(q.lambda1 - q.lambda2) < 1e-6) continue;  // no GF leverage
        const auto a2 = gf_arcs_.find(k2);
        if (a2 == gf_arcs_.end()) continue;
        const GfArc& arc2 = a2->second;
        if (arc2.cooldown_remaining > 0) continue;             // recent event
        if (arc2.diff_window.size() < kCodeEventMinMadSamples) continue;
        // Chain continuity back past the pre-gap reference: the
        // current unbroken run must have started at or before it.
        if (!(arc2.last_spacing_s > 0.0) || arc2.count < 2) continue;
        const double chain_start = arc2.last_epoch_s
            - static_cast<double>(arc2.count - 1) * arc2.last_spacing_s;
        if (chain_start > ref_epoch_s + 1e-6) continue;
        // Observation-domain continuity of both signals through the
        // interval (below-mask epochs count as missing).
        const auto c1 = sig_runs_.find(prefix + q.sig1);
        const auto c2 = sig_runs_.find(prefix + q.sig2);
        if (c1 == sig_runs_.end() ||
            c1->second.continuous_since_s > ref_epoch_s + 1e-6) continue;
        if (c2 == sig_runs_.end() ||
            c2->second.continuous_since_s > ref_epoch_s + 1e-6) continue;
        affirmed.insert(q.sig1);
        affirmed.insert(q.sig2);
    }
    return affirmed;
}

// =====================================================================
// A1: the late across-gap re-route
// =====================================================================

void TurboEditResolver::rerouteLateAcrossGap(EpochWork& w, RowWork& row) {
    const auto& log = w.log;
    const auto& epoch_now_s = w.epoch_now_s;
    auto& out = w.out;
    auto& pe = row.pe;
    auto& arc = row.arc;
    const auto& sat = row.sat;
    const auto& mw_flagged = row.mw_flagged;

    // --- Late across-gap re-route -----------------------------------
    // The chain already has its own TD2 (row not bridged) but the pre-gap
    // stash is still valid, and either (a) MW, whose locked-arc TD
    // confirmation lags the chain rebuild by one or more epochs, has only
    // now delivered its across-gap flag, or (b) a WL-settle deferral is
    // pending on this pair and is re-evaluated.  The rebuilt chain's TD2
    // cannot see the across-gap jump by construction, so a flat TD2 here
    // does not affirm GF stability; evaluated as-is, the MW-only veto
    // would classify a genuine re-lock slip (for example −28·λ₂ = 6.84 m
    // across a 2-epoch dropout) as a code event.  The row is re-routed to
    // the drift-corrected across-gap jump and the standard bridged
    // adjudication runs.  This applies only when the chain's own TD2 does
    // not exceed its gate: a same-epoch slip the chain can see is
    // adjudicated by the chain.  For a pending deferral, such a new event
    // also aborts the wait (the level reference is no longer the only
    // unknown), and the satellite is demoted to the conservative reset.
    if (!pe.bridged && (mw_flagged || arc.wl_settle_pending) &&
        arc.bridge_valid) {
        const double span_s    = epoch_now_s - arc.bridge_epoch_s;
        const double stash_cap = arc.wl_settle_pending
            ? bridge_max_gap_s_ + wl_settle_max_s_
            : bridge_max_gap_s_;
        if (pe.gf_exceeds && arc.wl_settle_pending) {
            log->warn("TurboEdit WL-settle deferral ABORTED for {} {}/{}: "
                      "the post-gap GF chain flagged a new event while "
                      "waiting — conservative reset",
                      sat, pe.sig1, pe.sig2);
            out.reset_sats.insert(sat);
            arc.bridge_valid      = false;
            arc.wl_settle_pending = false;
            held_across_lli_.erase(sat + "_" + pe.sig1);
            held_across_lli_.erase(sat + "_" + pe.sig2);
        } else if (!pe.gf_exceeds && span_s > 0.0 && span_s <= stash_cap) {
            const double drift = std::isfinite(arc.bridge_rate_mps)
                                     ? arc.bridge_rate_mps * span_s : 0.0;
            pe.td2     = pe.gf - arc.bridge_gf - drift;
            pe.gap_s   = span_s;
            pe.bridged = true;
            if (mw_flagged && !arc.wl_settle_pending)
                log->info("TurboEdit late across-gap adjudication {} {}/{}: "
                          "MW flag arrived after the GF chain rebuilt "
                          "(TD-confirm lag); re-evaluating against the "
                          "pre-gap level (dGF={:+.4f} m over {:.1f} s)",
                          sat, pe.sig1, pe.sig2, pe.td2, span_s);
        } else if (!pe.gf_exceeds) {
            arc.bridge_valid      = false;
            arc.wl_settle_pending = false;
        }
    }
}

// =====================================================================
// A2: the gap-bridge adjudication
// =====================================================================

void TurboEditResolver::adjudicateBridged(EpochWork& w, RowWork& row) {
    auto& out = w.out;
    const auto& pe = row.pe;
    auto& arc = row.arc;
    const auto& sat = row.sat;
    const auto& mwit = row.mwit;
    const auto& mw_flagged = row.mw_flagged;
    const auto& iono_stable_cons = row.iono_stable_cons;
    auto& dN_wl_trace = row.dN_wl_trace;
    auto& wl_known_trace = row.wl_known_trace;
    auto& action = row.action;

    // === Gap-bridge adjudication (see the header's bridge note) ===
    // Only MW flags (or an in-progress WL-settle deferral) are
    // adjudicated across a gap.  A GF-only bridged exceedance has no
    // affirmable MW-clean evidence across a re-acquisition and no
    // mandate to suppress, so it stays under the KF innovation
    // backstop, as it would without the bridge.  An untrusted or
    // uninformed bridge likewise leaves the MW mandate to the caller,
    // the same outcome as without the bridge.
    if (!(mw_flagged || arc.wl_settle_pending)) return;
    if (mw_flagged) {
        dN_wl_trace    = mwit->second.known ? mwit->second.dN_wl : 0;
        wl_known_trace = mwit->second.known;
    }
    bool actioned  = false;
    const bool trusted =
        iono_stable_cons &&
        !turbo_edit::pairHasLockLoss(*pe.obs, pe.sig1, pe.sig2) &&
        arc.diff_window.size() >= kCodeEventMinMadSamples &&
        std::abs(pe.lambda1 - pe.lambda2) >= 1e-6;
    if (trusted) actioned = adjudicateTrustedBridge(w, row);
    // An untrusted epoch (IVI burst, LLI on the pair) while a
    // deferral is pending neither adjudicates nor abandons it: the
    // caller keeps suppressing until trust returns or the timeout
    // fires.
    if (!actioned && arc.wl_settle_pending) {
        out.pending_sats.insert(sat);
        action = "defer";
    }
    if (actioned) {
        arc.bridge_valid      = false;  // one-shot
        arc.wl_settle_pending = false;
        // The held-across-LLI assertion is consumed by the same
        // action that consumes the bridge: whatever the bridge
        // decided (repair / reset / code event), the caller will
        // release its hold this epoch.
        held_across_lli_.erase(sat + "_" + pe.sig1);
        held_across_lli_.erase(sat + "_" + pe.sig2);
    }
}

bool TurboEditResolver::adjudicateTrustedBridge(EpochWork& w, RowWork& row) const {
    const auto& log = w.log;
    auto& out = w.out;
    const auto& pe = row.pe;
    const auto& arc = row.arc;
    const auto& sat = row.sat;
    const auto& mwit = row.mwit;
    const auto& mw_flagged = row.mw_flagged;
    auto& sigma_trace = row.sigma_trace;
    auto& did_slip = row.did_slip;
    const auto& dN_wl_trace = row.dN_wl_trace;
    auto& action = row.action;
    bool actioned = false;

    const double denom = pe.lambda1 - pe.lambda2;
    const double mad_sigma = computeMadSigma(arc.diff_window);
    const double widen =
        (arc.bridge_dt0_s > 0.0 && pe.gap_s > arc.bridge_dt0_s)
            ? std::sqrt(pe.gap_s / arc.bridge_dt0_s) : 1.0;
    const double zero_gate =
        kCodeEventZeroSigma * mad_sigma * widen;
    // Rate-adaptive evidence over the gap: the σ the split
    // rounds against is the arc's own, extrapolated by the
    // same random-walk factor as the gates.  The zero
    // affirmation also requires lattice discrimination, with
    // a cap that is not widened, because the lattice does not
    // stretch with the gap.
    const double sigma_bridge = mad_sigma * widen;
    sigma_trace = sigma_bridge;
    const double zero_cap =
        kVetoZeroGateMaxLambdaFrac * std::abs(denom);
    if (mw_only_veto_ && mw_flagged && !arc.wl_settle_pending &&
        std::isfinite(zero_gate) &&
        zero_gate > 0.0 && zero_gate <= zero_cap &&
        std::abs(pe.td2) <= zero_gate) {
        // Flat across the gap: the GF level did not move, so
        // the MW step is in the code observable (a
        // re-acquisition DLL transient); a code event.
        stageInteger(w, sat, pe.sig1, 0, pe.lambda1, 0.0);
        stageInteger(w, sat, pe.sig2, 0, pe.lambda2, 0.0);
        out.code_events.push_back({ sat, pe.sig1, pe.sig2,
                                    dN_wl_trace,
                                    mwit->second.known,
                                    pe.td2, zero_gate });
        action = "code_event";
        log->info("TurboEdit bridged code-domain event {} {}/{}: "
                  "MW flagged across a {:.1f} s gap but the GF "
                  "level is unmoved (|dGF|={:.4f} m <= {:.4f} m) "
                  "— phase preserved",
                  sat, pe.sig1, pe.sig2, pe.gap_s,
                  pe.td2, zero_gate);
        actioned = true;
    } else if (!splitCapable(sigma_bridge, denom)) {
        // Across-gap evidence too coarse to attribute
        // per-signal integers (the gap-widened σ exceeds the
        // split gate: coarse rate, long span, or a noisy
        // arc).  A pending WL-settle deferral cannot resolve
        // against a gate that only widens with the wait, so
        // it aborts to the conservative reset.  Otherwise the
        // resolver takes no action and the caller's MW
        // mandate fires, as it would without the bridge.
        if (arc.wl_settle_pending) {
            log->warn("TurboEdit WL-settle deferral ABORTED "
                      "for {} {}/{}: bridged GF σ={:.4f} m "
                      "exceeds the split gate {:.4f} m — "
                      "conservative reset",
                      sat, pe.sig1, pe.sig2, sigma_bridge,
                      split_max_sigma_ * std::abs(denom));
            out.reset_sats.insert(sat);
            did_slip = true;
            action = "reset";
            actioned = true;
        } else {
            log->info("TurboEdit bridged adjudication {} "
                      "{}/{} declined: GF σ={:.4f} m over a "
                      "{:.1f} s gap exceeds the split gate "
                      "{:.4f} m — MW mandate left to the "
                      "caller",
                      sat, pe.sig1, pe.sig2, sigma_bridge,
                      pe.gap_s,
                      split_max_sigma_ * std::abs(denom));
            action = "coarse_gf";
        }
    } else {
        // Across-gap attribution.  WL candidate sources, in
        // order of trust: (1) the published MW integer; (2) at
        // the flag epoch, round(float) ± kBridgeKWindow (3)
        // from the instantaneous re-acquisition float, which
        // NL code bias can offset by several cycles (the wide
        // window admits that bias instead of letting it make
        // a wrong candidate unique within a narrow window);
        // (3) on deferral epochs, the settled across-gap
        // estimate (post-restart MW Hatch mean minus the
        // stashed pre-jump level), used only once it is
        // confidently roundable, and taken as its rounded
        // integer alone (no window; see below).  The GF
        // equation alone is degenerate along the null-line
        // lattice ((0,−3) and (9,4) fit +0.733 and +0.736 m
        // for GPS L1/L2; only k separates them), so a unique
        // candidate is required after the fractional,
        // GF-separation and sibling-affirmation gates.
        BridgeSolve solve;
        collectBridgeWlCandidates(w, row, solve);
        selectBridgeCandidate(w, row, solve, denom, zero_gate);
        actioned = applyBridgeSolve(w, row, solve);
    }
    return actioned;
}

void TurboEditResolver::collectBridgeWlCandidates(const EpochWork& w,
                                                  const RowWork& row,
                                                  BridgeSolve& solve) const {
    const auto& log = w.log;
    const auto& wl_gap_evidence = w.wl_gap_evidence;
    const auto& pe = row.pe;
    const auto& arc = row.arc;
    const auto& sat = row.sat;
    const auto& mwit = row.mwit;
    const auto& mw_flagged = row.mw_flagged;
    auto& k_cands = solve.k_cands;
    auto& k_published = solve.k_published;
    auto& k_settled = solve.k_settled;

    if (mw_flagged && mwit->second.known) {
        k_cands.push_back(mwit->second.dN_wl);
        k_published = true;
    } else if (arc.wl_settle_pending) {
        // Deferral epoch: only the settled across-gap
        // estimate is consulted.  It is confidently
        // roundable by the settle criterion, so it selects
        // its rounded integer directly; a window around it
        // would re-admit the lattice ambiguity that the
        // wait resolves.
        const auto evit = wl_gap_evidence.find(pe.key);
        if (evit != wl_gap_evidence.end() &&
            evit->second.stable) {
            k_cands.push_back(static_cast<int>(
                std::llround(evit->second.float_cycles)));
            k_settled = true;
            log->info("TurboEdit WL-settle for {} {}/{}: "
                      "across-gap WL settled at {:+.2f} "
                      "cycles (σ_mean={:.3f}, n={}) — "
                      "re-running the bridged solve with "
                      "k={:+d}",
                      sat, pe.sig1, pe.sig2,
                      evit->second.float_cycles,
                      evit->second.sigma_mean_cycles,
                      evit->second.post_count,
                      k_cands.back());
        }
    } else if (mw_flagged &&
               std::isfinite(mwit->second.wl_float)) {
        // Flag epoch: the instantaneous re-acquisition
        // float, admitted with a wide window for its
        // NL-code bias.
        const int r = static_cast<int>(
            std::llround(mwit->second.wl_float));
        for (int kk = r - kBridgeKWindow;
             kk <= r + kBridgeKWindow; ++kk)
            k_cands.push_back(kk);
    }
}

void TurboEditResolver::selectBridgeCandidate(const EpochWork& w, const RowWork& row,
                                              BridgeSolve& solve, double denom,
                                              double zero_gate) const {
    const auto& log = w.log;
    const auto& pe = row.pe;
    const auto& arc = row.arc;
    const auto& sat = row.sat;
    const auto& k_cands = solve.k_cands;
    auto& best_k = solve.best_k;
    auto& best_dN2 = solve.best_dN2;
    auto& best_frac = solve.best_frac;
    auto& n_acceptable = solve.n_acceptable;
    auto& n_sibling_pruned = solve.n_sibling_pruned;

    // Sibling-pair affirmation: signals of this sat's
    // continuous, unflagged cross-frequency arcs are
    // affirmed unslipped; candidates that require a
    // nonzero integer on an affirmed signal are pruned.
    const std::set<std::string> affirmed =
        affirmedZeroSignals(w, sat, pe.key, arc.bridge_epoch_s);
    for (const int k : k_cands) {
        const double dN2_f =
            (pe.td2 - pe.lambda1 * static_cast<double>(k)) / denom;
        const int    d2 = static_cast<int>(std::llround(dN2_f));
        const double fr = std::abs(dN2_f - static_cast<double>(d2));
        const int    d1 = d2 + k;
        const bool sane = std::abs(d1) <= kMaxSlipCycles &&
                          std::abs(d2) <= kMaxSlipCycles;
        // GF-separation guard: the candidate's predicted
        // jump must exceed the zero gate.  Otherwise a
        // marginal noise exceedance of the zero gate could
        // be attributed to a null-line pair ((9,7) predicts
        // 3.2 mm for GPS L1/L2) and produce a 9-cycle
        // repair on a continuous signal from noise alone.
        // A candidate set with no separable member falls
        // through to the conservative reset.
        const bool gf_separated =
            std::abs(pe.lambda1 * static_cast<double>(d1) -
                     pe.lambda2 * static_cast<double>(d2)) > zero_gate;
        if (!(fr <= kMaxFractional && sane &&
              (d1 != 0 || d2 != 0) && gf_separated))
            continue;
        if ((d1 != 0 && affirmed.count(pe.sig1)) ||
            (d2 != 0 && affirmed.count(pe.sig2))) {
            ++n_sibling_pruned;
            continue;
        }
        ++n_acceptable;
        if (fr < best_frac) {
            best_frac = fr; best_k = k; best_dN2 = d2;
        }
    }
    if (n_sibling_pruned > 0) {
        std::string aff_txt;
        for (const auto& s : affirmed)
            aff_txt += (aff_txt.empty() ? "" : ",") + s;
        log->info("TurboEdit bridged solve {} {}/{}: "
                  "sibling-pair affirmation pruned {} "
                  "GF-consistent candidate(s) (affirmed-"
                  "unslipped signal(s): {})",
                  sat, pe.sig1, pe.sig2, n_sibling_pruned,
                  aff_txt);
    }
}

bool TurboEditResolver::applyBridgeSolve(EpochWork& w, RowWork& row,
                                         const BridgeSolve& solve) const {
    const auto& log = w.log;
    auto& out = w.out;
    auto& cand_moot = w.cand_moot;
    const auto& pe = row.pe;
    const auto& arc = row.arc;
    const auto& sat = row.sat;
    auto& did_slip = row.did_slip;
    auto& dN1 = row.dN1;
    auto& dN2 = row.dN2;
    auto& frac2 = row.frac2;
    auto& dN_wl_trace = row.dN_wl_trace;
    auto& action = row.action;
    const auto& k_cands = solve.k_cands;
    const auto& k_published = solve.k_published;
    const auto& k_settled = solve.k_settled;
    const auto& best_k = solve.best_k;
    const auto& best_dN2 = solve.best_dN2;
    const auto& best_frac = solve.best_frac;
    const auto& n_acceptable = solve.n_acceptable;
    bool actioned = false;

    if (n_acceptable == 1) {
        dN2 = best_dN2;
        dN1 = best_dN2 + best_k;
        frac2 = best_frac;
        dN_wl_trace = best_k;
        // Dispatch per signal by observation-domain
        // continuity: a signal observed through every gap
        // epoch kept its ambiguity and gets an integer
        // repair; a signal that missed an epoch was pruned
        // and reseeded, so its slip is moot.
        bool any_repair = false;
        auto dispatch = [&](const std::string& sig, int dN,
                            double lam) {
            stageInteger(w, sat, sig, dN, lam, frac2);
            if (dN == 0) return;
            const std::string sigkey = sat + "_" + sig;
            // A signal the caller holds across an LLI gap
            // counts as continuous: the hold preserved its
            // pre-gap value, so the across-gap integer is a
            // real repair, not an already-absorbed reseed.
            // Other signals use observation-domain
            // continuity.
            const auto cs = sig_runs_.find(sigkey);
            const bool continuous =
                held_across_lli_.count(sigkey) > 0 ||
                (cs != sig_runs_.end() &&
                 cs->second.continuous_since_s <= arc.bridge_epoch_s + 1e-6);
            if (continuous) any_repair = true;
            else            cand_moot[sat].insert(sig);
        };
        dispatch(pe.sig1, dN1, pe.lambda1);
        dispatch(pe.sig2, dN2, pe.lambda2);
        action = any_repair ? "repair" : "moot";
        log->info("TurboEdit bridged attribution {} {}/{}: "
                  "dGF={:+.4f} m over {:.1f} s + k={:+d} → "
                  "(dN1={:+d}, dN2={:+d}), frac={:.3f} — {}{}",
                  sat, pe.sig1, pe.sig2, pe.td2, pe.gap_s,
                  best_k, dN1, dN2, frac2,
                  any_repair ? "repair (continuous signal)"
                             : "moot (reseeded during gap)",
                  arc.wl_settle_pending
                      ? " [WL-settle deferred adjudication]"
                      : "");
        actioned = true;
    } else if (wl_settle_enabled_ && !k_published &&
               !k_settled) {
        actioned = deferBridgeAdjudication(w, row, n_acceptable);
    } else if (!k_cands.empty()) {
        // Active failure: the across-gap jump fits no
        // candidate, or more than one, and deferral is
        // unavailable; conservative reset.
        out.reset_sats.insert(sat);
        did_slip = true;
        action = "reset";
        actioned = true;
    }
    // k_cands empty (no deferral): no WL information at
    // all.  The resolver takes no action and the caller's
    // own mandate fires, as it would without the bridge.
    return actioned;
}

bool TurboEditResolver::deferBridgeAdjudication(EpochWork& w, RowWork& row,
                                                int n_acceptable) const {
    const auto& log = w.log;
    const auto& epoch_now_s = w.epoch_now_s;
    auto& out = w.out;
    const auto& pe = row.pe;
    auto& arc = row.arc;
    const auto& sat = row.sat;
    auto& did_slip = row.did_slip;
    auto& action = row.action;
    bool actioned = false;

    // Float-derived evidence is absent, not yet
    // settled, or still ambiguous (n_acceptable ≥ 2:
    // candidates separated by a null-line pair).  A
    // guess can produce a confident wrong repair (for
    // example (−9, −35) against a true (0, −28)), so
    // the adjudication is deferred while the
    // post-restart MW mean settles, bounded by the
    // configured cap.  The caller keeps the holds
    // active and suppresses the MW mandate via
    // out.pending_sats.
    if (!arc.wl_settle_pending) {
        arc.wl_settle_pending = true;
        arc.wl_settle_start_s = epoch_now_s;
        log->info("TurboEdit WL-settle deferral START "
                  "for {} {}/{}: dGF={:+.4f} m solid "
                  "but the WL evidence is {} — waiting "
                  "for the post-restart MW mean "
                  "(cap {:.1f} s)",
                  sat, pe.sig1, pe.sig2, pe.td2,
                  n_acceptable >= 2
                      ? "lattice-ambiguous"
                      : "unsettled",
                  wl_settle_max_s_);
    }
    if (epoch_now_s - arc.wl_settle_start_s >
        wl_settle_max_s_) {
        log->warn("TurboEdit WL-settle deferral TIMED "
                  "OUT for {} {}/{} after {:.1f} s — "
                  "conservative reset",
                  sat, pe.sig1, pe.sig2,
                  epoch_now_s - arc.wl_settle_start_s);
        out.reset_sats.insert(sat);
        did_slip = true;
        action = "reset";
        actioned = true;
    } else {
        out.pending_sats.insert(sat);
        action = "defer";
        // Not actioned: the stash and the held marks
        // persist to the next epoch's re-evaluation.
    }
    return actioned;
}

} // namespace GnssProcessor
