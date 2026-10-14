/**
 * @file TurboEditChain.cpp
 * @brief TurboEdit pass 1: steps R2 to R4 of @c TurboEditResolver::resolve.
 *
 * Owns the per-epoch reading of the observations before any adjudication:
 *   - R2 @c indexMwFlags: the MW detector's wide-lane flags by arc key;
 *   - R3 @c collectChainRows: for every observation above the mask, the
 *     per-signal continuity record (@c trackSignalContinuity), then for every
 *     dual-band phase pair the GF value, the chain restart on a break
 *     (@c restartArcOnBreak, which may stash a pre-gap level for the gap
 *     bridge), the warm-up (a bridged row, or @c rollWarmUpChain), and the
 *     TD2 row with the cross-satellite noise pool (@c collectPairRow);
 *   - R4 @c assessIonosphere: the cross-satellite IVI per constellation.
 * Pass 1 writes the GF arcs and the rows; pass 2 (TurboEditAdjudication.cpp)
 * reads the rows and decides.
 */

#include "slip/TurboEditResolver.hpp"
#include "slip/TurboEditEpochWork.hpp"
#include "math/TimeUtils.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace GnssProcessor {

// =====================================================================
// R2: the MW flag index
// =====================================================================

void TurboEditResolver::indexMwFlags(EpochWork& w) const {
    const auto& mw_results = w.mw_results;
    auto& mw_by_pair = w.mw_by_pair;

    // --- Index MW wide-lane slips by exact pair key (dedup the two-per-pair entries) ---
    for (const auto& sr : mw_results) {
        if (sr.sig1.empty() || sr.sig2.empty()) continue;  // LLI / signal-less: not a pair slip
        const std::string sat = turbo_edit::satOf(sr);
        if (sat.empty()) continue;
        auto& f = mw_by_pair[arcKey(sat, sr.sig1, sr.sig2)];
        // Prefer any entry that carries a usable integer for this pair.
        if (sr.slip_wl_known && !f.known) { f.dN_wl = sr.slip_wl_cycles; f.known = true; }
        if (!std::isfinite(f.wl_float) && std::isfinite(sr.slip_wl_float))
            f.wl_float = sr.slip_wl_float;
    }
}

// =====================================================================
// R3: pass 1, one row per dual-band phase pair
// =====================================================================

void TurboEditResolver::collectChainRows(EpochWork& w) {
    const auto& observations = w.observations;
    const auto& cache = w.cache;
    auto& pe_by_key = w.pe_by_key;

    // --- Pass 1: GF + TD2 for every dual-band phase pair; collect IVI pools ---
    for (const auto& obs : observations) {
        if (obs.satellite.empty()) continue;
        if (!std::isfinite(obs.elevation) || obs.elevation < config_.getElevationMaskDeg())
            continue;
        const double epoch_s = TimeUtils::continuousGpsSeconds(obs.time);

        trackSignalContinuity(obs, epoch_s);

        for (const auto& pr : collectPhasePairs(obs)) {
            collectPairRow(w, obs, pr, epoch_s);
        }
    }

    // Row lookup by arc key for the sibling-pair affirmation below.  Rows
    // are this epoch's snapshot; pass 2 mutates arcs, never `cache`.
    for (const auto& row : cache) pe_by_key[row.key] = &row;
}

void TurboEditResolver::trackSignalContinuity(const PppObservation& obs, double epoch_s) {
    // Per-signal phase-continuity bookkeeping for the gap bridge's
    // moot-vs-repair dispatch (observation-domain; below-mask epochs
    // were excluded above and therefore count as missing, matching the
    // filter's own usage of these observations).
    for (const auto& [sig, val] : obs.raw_observed) {
        if (sig.size() < 2 || sig[0] != 'L') continue;
        if (sig.find('_') != std::string::npos) continue;
        if (!std::isfinite(val)) continue;
        const std::string skey = obs.satellite + "_" + sig;
        const auto ls = sig_runs_.find(skey);
        if (ls == sig_runs_.end() ||
            (prev_epoch_s_ > 0.0 && ls->second.last_seen_s < prev_epoch_s_ - 1e-6)) {
            sig_runs_[skey].continuous_since_s = epoch_s;
        }
        sig_runs_[skey].last_seen_s = epoch_s;
    }
}

void TurboEditResolver::collectPairRow(EpochWork& w, const PppObservation& obs,
                                       const PhasePair& pr, double epoch_s) {
    auto& cache = w.cache;
    auto& cons_td2 = w.cons_td2;

    const double gf = computeGfMeters(obs, pr.phase1, pr.phase2);
    if (!std::isfinite(gf)) return;
    const double lam1 = ppp_utils_.getWavelength(obs.system, pr.phase1, obs.satellite);
    const double lam2 = ppp_utils_.getWavelength(obs.system, pr.phase2, obs.satellite);
    if (!(lam1 > 0.0) || !(lam2 > 0.0)) return;

    const std::string key = arcKey(obs.satellite, pr.phase1, pr.phase2);
    auto& arc = gf_arcs_[key];

    restartArcOnBreak(arc, epoch_s);

    // Warm-up epochs (count < 2): no TD2 yet.  With a fresh bridge the
    // pair is cached against the drift-corrected across-gap jump
    // instead (pass 2 adjudicates it and rolls the chain, as for
    // normal cached rows); otherwise the chain rolls forward inline.
    if (arc.count < 2) {
        bool cached_bridged = false;
        if (arc.bridge_valid) {
            const double span_s = epoch_s - arc.bridge_epoch_s;
            if (span_s > 0.0 && span_s <= bridge_max_gap_s_) {
                const double drift =
                    std::isfinite(arc.bridge_rate_mps)
                        ? arc.bridge_rate_mps * span_s : 0.0;
                const double dgf_gap = gf - arc.bridge_gf - drift;
                // Gate from the preserved calibration, widened by the
                // random-walk factor √(Δt/dt₀): the arc's own
                // per-interval variability extrapolated over the gap.
                double thresh_unfloored =
                    std::numeric_limits<double>::quiet_NaN();
                const double base = computeAdaptiveThreshold(
                    arc, key, obs.elevation, &thresh_unfloored);
                const double widen =
                    (arc.bridge_dt0_s > 0.0 && span_s > arc.bridge_dt0_s)
                        ? std::sqrt(span_s / arc.bridge_dt0_s) : 1.0;
                const bool exceeds = std::isfinite(base) &&
                                     std::abs(dgf_gap) > base * widen;
                // Bridged rows feed neither the IVI pool nor the
                // cross-sat TD2 pool: the across-gap jump is not a
                // TD2 sample and would bias both statistics.
                cache.push_back({ &obs, key, pr.phase1, pr.phase2, gf,
                                  dgf_gap, lam1, lam2,
                                  base * widen, thresh_unfloored * widen,
                                  exceeds, /*bridged=*/true, span_s });
                cached_bridged = true;
            } else {
                arc.bridge_valid = false;  // span exceeded the cap
            }
        }
        if (!cached_bridged) rollWarmUpChain(arc, gf, epoch_s);
        return;
    }

    const double td2    = gf - 2.0 * arc.prev_gf + arc.prev_prev_gf;
    double thresh_unfloored = std::numeric_limits<double>::quiet_NaN();
    const double thresh = computeAdaptiveThreshold(arc, key, obs.elevation,
                                                   &thresh_unfloored);
    const bool gf_exceeds = std::isfinite(thresh) && std::abs(td2) > thresh;

    expireBridgeStash(arc, epoch_s);

    cache.push_back({ &obs, key, pr.phase1, pr.phase2, gf, td2, lam1, lam2,
                      thresh, thresh_unfloored, gf_exceeds });
    cons_td2[obs.satellite[0]].push_back(td2);

    // Cross-sat TD2 noise pool (source of the warmup floor): samples
    // are zenith-normalized so the pooled MAD is a homoscedastic
    // estimate (xsatSigmaFloor re-expands per arc).  Exceeding samples
    // are excluded, because a slip TD2 is a discrete event, not noise.
    if (!gf_exceeds) {
        const std::string gkey = xsatGroupKey(key);
        if (!gkey.empty())
            xsat_devs_[gkey].push_back(td2 / turbo_edit::gfElevWeight(obs.elevation));
    }
}

void TurboEditResolver::restartArcOnBreak(GfArc& arc, double epoch_s) const {
    // Restart the arc when its history is not strictly consecutive, on
    // either of two triggers:
    //  (a) Absolute gap: elapsed time exceeds the configured tolerance.
    //  (b) Per-signal dropout: the arc did not update in the previous
    //      epoch (its last update predates prev_epoch_s_).  This pair
    //      was missing while other satellites were observed, so its
    //      prev_gf / prev_prev_gf reference non-adjacent epochs and the
    //      2nd-order time difference would be malformed.  An exact
    //      timestamp comparison makes this rate-agnostic (works at 1 Hz
    //      or 10 Hz) without depending on max_gap_seconds_.
    // Trigger (a) has no count>0 requirement: a post-slip arc has
    // count==0 with a preserved MAD window and a stamped last_epoch_s,
    // and the absolute-gap trigger stays armed so that a long dropout
    // after the slip expires the preserved calibration on return.
    // (Brand-new arcs have last_epoch_s == 0 and are unaffected.)
    const bool absolute_gap =
        arc.last_epoch_s > 0.0 &&
        (epoch_s - arc.last_epoch_s) > max_gap_seconds_;
    const bool skipped_prev_epoch =
        arc.count > 0 && arc.last_epoch_s > 0.0 && prev_epoch_s_ > 0.0 &&
        arc.last_epoch_s < prev_epoch_s_ - 1e-6;
    // Gap bridge: a chain break within turbo_edit_max_gap_s stashes the
    // pre-gap level (requires count >= 2, a level confirmed by at least
    // one consecutive pair) so the across-gap jump can adjudicate
    // post-gap MW flags.  It also preserves the MAD calibration window:
    // a 15 s gap changes the arc's TD2 noise no more than the 1.5 s
    // gaps (max_gap_seconds_ = 1.5 × interval at 1 Hz) that the partial
    // reset already tolerates.  Beyond the cap the two-tier behavior
    // governs: an absolute gap wipes the arc; a per-signal dropout
    // inside max_gap_seconds_ gets a partial reset.  A break during
    // warm-up (count < 2) cannot stash and also voids any older bridge,
    // so bridges do not compound across repeated breaks.
    const double gap_span_s =
        (arc.last_epoch_s > 0.0) ? (epoch_s - arc.last_epoch_s) : 0.0;
    const bool bridgeable =
        (absolute_gap || skipped_prev_epoch) &&
        bridge_max_gap_s_ > 0.0 &&
        gap_span_s > 0.0 && gap_span_s <= bridge_max_gap_s_;
    if (bridgeable) {
        if (arc.count >= 2) {
            arc.bridge_valid    = true;
            arc.bridge_gf       = arc.prev_gf;
            arc.bridge_epoch_s  = arc.last_epoch_s;
            arc.bridge_rate_mps = arc.rate_ewma_mps;
            arc.bridge_dt0_s    = arc.last_spacing_s;
        } else {
            arc.bridge_valid = false;
        }
        arc.prev_gf            = 0.0;
        arc.prev_prev_gf       = 0.0;
        arc.count              = 0;
        arc.last_epoch_s       = 0.0;
        arc.cooldown_remaining = 0;
        arc.rate_ewma_mps  = std::numeric_limits<double>::quiet_NaN();
        arc.last_spacing_s = 0.0;
    } else if (absolute_gap) {
        arc = GfArc{};
    } else if (skipped_prev_epoch) {
        // Partial reset: the TD2 chain is non-consecutive and re-seeds
        // (prev_gf / prev_prev_gf reference non-adjacent epochs), but
        // the MAD window is calibration, not chain state.  The TD2
        // noise level is a slowly varying property of the arc
        // (elevation, tracking, multipath) that a gap inside
        // max_gap_seconds_ does not invalidate.  Wiping it with the
        // chain would leave kMinEpochsForAdaptive (5) extra epochs
        // without a MAD threshold after every short dropout, which
        // are the epochs where post-gap slips need a live GF gate.
        // Keeping it adds no cross-gap differencing: the re-seeded
        // chain builds the first post-gap TD2 from post-gap samples
        // only, and the preserved window only calibrates the gate that
        // tests it.  Cooldown clears, because a pre-gap slip's TD2
        // echo cannot form across a re-seeded chain.
        arc.prev_gf            = 0.0;
        arc.prev_prev_gf       = 0.0;
        arc.count              = 0;
        arc.last_epoch_s       = 0.0;
        arc.cooldown_remaining = 0;
        // With bridging on, this branch is reached only when the gap
        // exceeded the bridge cap, so the stale level is invalidated.
        arc.bridge_valid   = false;
        arc.rate_ewma_mps  = std::numeric_limits<double>::quiet_NaN();
        arc.last_spacing_s = 0.0;
    }
}

void TurboEditResolver::rollWarmUpChain(GfArc& arc, double gf, double epoch_s) {
    if (arc.count == 1 && arc.last_epoch_s > 0.0 &&
        epoch_s > arc.last_epoch_s) {
        const double dt = epoch_s - arc.last_epoch_s;
        const double rate = (gf - arc.prev_gf) / dt;
        arc.rate_ewma_mps = std::isfinite(arc.rate_ewma_mps)
            ? kBridgeRateAlpha * rate +
                  (1.0 - kBridgeRateAlpha) * arc.rate_ewma_mps
            : rate;
        arc.last_spacing_s = dt;
    }
    if (arc.count == 0) arc.prev_gf = gf;
    else { arc.prev_prev_gf = arc.prev_gf; arc.prev_gf = gf; }
    ++arc.count;
    arc.last_epoch_s = epoch_s;
    if (arc.cooldown_remaining > 0) --arc.cooldown_remaining;
}

void TurboEditResolver::expireBridgeStash(GfArc& arc, double epoch_s) const {
    // The chain has a TD2 again, but the pre-gap stash stays valid
    // (time-bounded) until consumed or expired.  The MW locked-arc
    // TD-confirmation path delivers its across-gap flag one or more
    // epochs after the chain regains a TD2.  That flag is adjudicated
    // against the across-gap jump, because the rebuilt chain's TD2
    // cannot see it by construction (a flat post-gap TD2 would pass a
    // genuine re-lock slip to the MW-only veto as a "code event").
    // Pass 2 routes such late MW flags back to the stash.  The stash
    // expires at the cap, measured from the pre-gap level's epoch,
    // extended by the WL-settle window (turbo_edit_wl_settle_max_s)
    // while a deferred adjudication waits on the post-restart MW mean.
    const double stash_cap = arc.wl_settle_pending
        ? bridge_max_gap_s_ + wl_settle_max_s_
        : bridge_max_gap_s_;
    if (arc.bridge_valid &&
        (epoch_s - arc.bridge_epoch_s) > stash_cap) {
        arc.bridge_valid      = false;
        arc.wl_settle_pending = false;
    }
}

// =====================================================================
// R4: the cross-satellite IVI
// =====================================================================

void TurboEditResolver::assessIonosphere(EpochWork& w) const {
    const auto& cons_td2 = w.cons_td2;
    auto& ivi_by_cons = w.ivi_by_cons;
    auto& out = w.out;

    // --- Cross-satellite IVI per constellation; global stability flag ---
    double max_ivi = 0.0;
    for (const auto& [cons, td2v] : cons_td2) {
        const double ivi = computeIvi(td2v);
        ivi_by_cons[cons] = ivi;
        max_ivi = std::max(max_ivi, ivi);
    }
    out.max_ivi = max_ivi;
    out.iono_stable = (max_ivi < kIviThreshold);

    // (No high-rate affirmation: GF trust is evidence-gated per arc.  The
    // split-capability gate and the veto zero-gate cap scale with the
    // measured TD2 noise, which the un-cancelled coarse-rate iono residual
    // inflates.  A 30 s file is therefore demoted as far as its own noise
    // requires, with or without an INTERVAL header.)
}

} // namespace GnssProcessor
