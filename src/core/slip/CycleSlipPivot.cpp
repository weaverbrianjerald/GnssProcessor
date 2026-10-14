/**
 * @file CycleSlipPivot.cpp
 * @brief The BSSD pivot frame sections of @c CycleSlipManager::detectAndRepair:
 *        S9 (which pivots slipped), S12 (the frame repaired in place when the
 *        pivot's integer is known) and S13 (the forced handover and the
 *        deferred repairs in the new frame).
 *
 * All three are no-ops without BSSD (@c estimation.between_satellite_sd).
 * Bodies moved verbatim from CycleSlipManager.cpp (card UOM step U3;
 * scripts/uom_u3_verify.py).
 */

#include "slip/CycleSlipManager.hpp"
#include "slip/CycleSlipEpochWork.hpp"
#include <algorithm>
#include <cmath>
#include <iterator>

namespace GnssProcessor {

// ===========================================================================
// S9: pivot culprits
// ===========================================================================

void CycleSlipManager::scanPivotCulprits(SlipEpochWork& w) const {
    const auto& log           = w.log;
    const auto& epoch_prefix  = w.epoch_prefix;
    const auto& meas_model    = w.meas_model;
    const auto& te_culprits_by_sat = w.te_culprits_by_sat;
    const auto& te_suppress_by_sat = w.te_suppress_by_sat;
    const auto& slips_by_sat  = w.slips_by_sat;
    auto& all_pairs_by_sat    = w.all_pairs_by_sat;
    auto& pivot_affected_cons_sig = w.pivot_affected_cons_sig;

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
    // construction a sat that did not slip on S, so its
    // dN_newpivot = 0 and each remaining slipped sat's SD
    // adjustment is its own dN · λ (this includes the old pivot,
    // which becomes a regular non-pivot with a non-zero SD value
    // and its own dN_oldpivot to apply).  Covariance is preserved
    // through both the handover transform (an S-transform) and
    // the integer step.
    //
    // Granularity is per (cons, signal): a sat with two culprit
    // signals, one pivot-affected and one not, gets the
    // pre-handover integer repair on the unaffected signal and
    // the post-handover deferred repair on the pivot-affected one.
    if (!config_.enableBetweenSatelliteSd()) return;

    const auto& pivots = meas_model.getBssdPivotSatellites();
    for (const auto& [sat_id, slips] : slips_by_sat) {
        if (slips.empty()) continue;
        const std::string cons = PppUtils::getSystemString(sat_id);
        auto piv_it = pivots.find(cons);
        if (piv_it == pivots.end()) continue;
        if (piv_it->second != sat_id) continue;  // not the pivot

        // This sat is the pivot for its constellation and has
        // slipped MW pairs.  Identify which signals of the pivot
        // are culprits; those (cons, signal) pairs get the
        // deferred repair after handover.  Cross-consistency
        // handles 3+ frequencies and returns nothing in the
        // dual-frequency case, which falls back to TurboEdit's
        // MW+GF resolution so that a common-mode dual-frequency
        // pivot slip still goes through the in-place or handover
        // path instead of a full reset.
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

// ===========================================================================
// S12: the pivot frame repaired in place
// ===========================================================================

bool CycleSlipManager::confidentlyCleanOnSignal(const SlipEpochWork& w,
                                                const std::string& sat,
                                                const std::string& sig) const {
    const auto& lli_sats         = w.lli_sats;
    const auto& slips_by_sat     = w.slips_by_sat;
    const auto& all_pairs_by_sat = w.all_pairs_by_sat;

    // Confidence test for the non-slipped-peer frame shift inside the
    // block: a sat is "confidently clean" on @p sig only if it has a
    // mature MW arc involving @p sig (so a slip would have been seen)
    // and @p sig is not among its identified culprits.  This separates
    // an unslipped peer (lossless -dN_pivot shift) from one that could
    // not be evaluated this epoch (reset instead).
    // A receiver-declared loss of lock never counts as
    // affirmative cleanliness.  The LLI sat has no pair slips
    // (the detector skips pair processing) yet its preserved MW
    // arcs still appear mature, so without this guard the
    // zero-slips-anywhere branch below would classify a sat
    // whose integer continuity just broke arbitrarily as
    // "confidently clean" and give it a covariance-preserving
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
}

void CycleSlipManager::repairAcrossSlippedPivotInPlace(SlipEpochWork& w) {
    const auto& meas_model = w.meas_model;
    auto& pivot_affected_cons_sig  = w.pivot_affected_cons_sig;
    auto& deferred_integer_repairs = w.deferred_integer_repairs;
    auto& deferred_resets          = w.deferred_resets;

    // --- Repair-in-place across a slipped pivot (relative integers) ---
    // The handover block below re-anchors the SD frame onto a clean
    // satellite before applying each peer's integer.  That requires a
    // clean satellite, and a common-mode event has none (for example
    // a receiver E6-band re-acquisition that slips every Galileo L6C
    // at once); the handover then refuses and every peer is reset,
    // losing its convergence.
    //
    // A clean anchor is not required: the pivot needs a known slip
    // integer, not an absence of slip.  The SD ambiguity state is
    // N_sd(peer) = N(peer) − N(pivot), so a simultaneous slip moves
    // the true SD by (dN_peer − dN_pivot).  Shifting each peer's SD
    // ambiguity by that relative integer repairs the frame in place:
    // the pivot is retained, no handover and no clean anchor are
    // needed, and the covariance is preserved.  This is the
    // identity-handover case of the block below, with the integer
    // step taken relative to the (slipped but known) pivot.
    //
    // Eligibility is integer confidence: the pivot's own per-signal
    // integer is present in deferred_integer_repairs (it passed
    // computeCulpritSlipIntegers' cross-pair consistency check) for
    // every pivot-affected signal of the constellation.  The decision
    // is all-or-nothing per constellation because the BSSD pivot is
    // per constellation: handling some signals in place while a
    // same-constellation handover fires for the rest would re-anchor
    // (and re-seed) the frame just repaired.  Constellations that do
    // not qualify go through the handover-or-reset path unchanged.
    //
    // A wrong pivot integer would appear next epoch as a coherent
    // per-constellation residual offset, which the KF post-fit
    // pivot-slip detector catches and turns into a forced handover
    // (Application.cpp); no separate revocation is needed here.  MW
    // arc EWMA stats are observation-space and are not affected by
    // the state-level integer step.
    if (!config_.enableBetweenSatelliteSd() || deferred_integer_repairs.empty()) return;

    const auto& pivots_now = meas_model.getBssdPivotSatellites();

    std::set<std::string> cons_set;
    for (const auto& cs : pivot_affected_cons_sig)
        cons_set.insert(cs.first);

    std::set<std::string> handled_cons;

    for (const auto& cons : cons_set) {
        if (repairConstellationInPlace(w, cons, pivots_now))
            handled_cons.insert(cons);
    }
    counters_.pivot_frames_repaired_in_place += static_cast<long long>(handled_cons.size());

    // Drain handled constellations from the deferred queues and the
    // pivot-affected set so the handover block processes only what
    // still needs a handover (or reset).
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

bool CycleSlipManager::repairConstellationInPlace(
    SlipEpochWork& w,
    const std::string& cons,
    const std::map<std::string, std::string>& pivots_now) {
    const auto& log           = w.log;
    const auto& epoch_prefix  = w.epoch_prefix;
    const auto& modeled_obs   = w.modeled_obs;
    auto& state_manager       = w.state_manager;
    const auto& meas_model    = w.meas_model;
    auto& result              = w.result;
    const auto& epoch_reset_amb_keys     = w.epoch_reset_amb_keys;
    const auto& pivot_affected_cons_sig  = w.pivot_affected_cons_sig;
    const auto& deferred_integer_repairs = w.deferred_integer_repairs;
    const auto& deferred_resets          = w.deferred_resets;

    auto pit = pivots_now.find(cons);
    if (pit == pivots_now.end()) return false;
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
    if (!all_known) return false;  // fall through to handover

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
                // Sat unobserved this epoch or invalid lambda:
                // the relative shift cannot be applied, so reset
                // in the current (pivot-retained) frame.
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
        // The flagged-peer loop above shifts only the sats the
        // MW detector queued.  A non-pivot SV that did not slip
        // still has its SD ambiguity (N_peer − N_pivot) off by
        // exactly −dN_pivot when the pivot slipped, because its
        // reference moved.  Such sats never enter the deferred
        // queues; without this pass their SD ambiguity stays
        // wrong and appears as a single large post-fit residual.
        // Each confidently-clean observed peer is shifted by
        // −dN_pivot (lossless integer step, covariance
        // preserved); peers whose slip status cannot be
        // confirmed this epoch are reset instead of risking a
        // wrong integer.
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
                // with the slipped pivot's carrier; an
                // additional -dN_pivot shift would put it off
                // by exactly dN_pivot · λ.
                if (epoch_reset_amb_keys.count(sat + "_" + sig))
                    continue;

                if (confidentlyCleanOnSignal(w, sat, sig)) {
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

    // Peers with no clean per-pair integer cannot be repaired;
    // they are reset in the current (pivot-retained) SD frame
    // instead of sending the whole constellation to handover.
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
    result.state_modified = true;
    return true;
}

// ===========================================================================
// S13: forced pivot handover and the deferred repairs
// ===========================================================================

std::set<std::string> CycleSlipManager::handoverExcludeSet(const SlipEpochWork& w) const {
    const auto& te                 = w.te;
    const auto& te_culprits_by_sat = w.te_culprits_by_sat;
    const auto& slips_by_sat       = w.slips_by_sat;
    const auto& lli_sats           = w.lli_sats;

    // Build the exclude set: every sat with slip evidence this
    // epoch, from any detector.  It is passed to
    // handlePivotHandover so the forced-handover replacement
    // search cannot pick another sat that is itself mid-slip.
    // That would move the broken SD-frame anchor from one
    // slipping sat to another, leave the deferred integer
    // repairs off by the new pivot's own dN · λ, and repeat the
    // problem next epoch.  MW pair slips alone do not cover it:
    // LLI sats carry only signal-less entries (filtered out of
    // slips_by_sat) yet were just obs-reseeded at reset
    // covariance, and sats flagged by TurboEdit GF-only (the MW
    // blind spot, where MW is silent by definition) are
    // mid-slip or about to be reset by the GF-only pass.
    // Electing either as the new SD anchor would re-reference
    // the whole constellation against a slipped or freshly
    // reset state.
    std::set<std::string> exclude_sats;
    for (const auto& [sat_id, slips] : slips_by_sat) {
        if (!slips.empty()) exclude_sats.insert(sat_id);
    }
    exclude_sats.insert(lli_sats.begin(), lli_sats.end());
    for (const auto& [te_sat, sigs] : te_culprits_by_sat) {
        if (!sigs.empty()) exclude_sats.insert(te_sat);
    }
    exclude_sats.insert(te.reset_sats.begin(), te.reset_sats.end());
    return exclude_sats;
}

void CycleSlipManager::forcePivotHandoverAndApplyDeferred(SlipEpochWork& w) {
    const auto& log           = w.log;
    const auto& epoch_prefix  = w.epoch_prefix;
    const auto& modeled_obs   = w.modeled_obs;
    auto& state_manager       = w.state_manager;
    const auto& meas_model    = w.meas_model;
    const auto& pivot_affected_cons_sig  = w.pivot_affected_cons_sig;
    const auto& deferred_integer_repairs = w.deferred_integer_repairs;
    const auto& deferred_resets          = w.deferred_resets;

    // --- Force pivot handover + apply deferred integer repairs ---
    // Runs when the pre-scan above queued any (sat, signal) whose
    // constellation pivot itself slipped on the same signal, and
    // the in-place block did not handle that constellation.  Steps:
    //   1. Snapshot the current pivot per constellation.
    //   2. requestForcedHandover on each affected constellation.
    //   3. handlePivotHandover(forced_only=true) synchronously
    //      here in the pre-update path; it re-references every SD
    //      ambiguity, iono and sat-bias of the affected cons from
    //      the old pivot to a clean replacement.
    //   4. For each deferred (sat, signal, dN), apply
    //      x(amb_idx) += dN · λ to the post-handover SD ambiguity,
    //      covariance preserved.  This is exactly the integer
    //      carrier slip of the sat itself, because the new pivot
    //      did not slip on this signal (the pivot selector picks
    //      it from the clean candidates).
    //   5. For deferred sats with no clean per-pair integer,
    //      fall back to resetAmbiguityBssdAware in the new frame.
    // MW arc EWMA stats are not touched on purpose: the arc data
    // is in observation space and is unchanged by both the
    // handover and the integer step.
    if (deferred_integer_repairs.empty() && deferred_resets.empty()) return;

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

    std::set<std::string> exclude_sats = handoverExcludeSet(w);

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
    for (const auto& cons : affected_cons)
        if (handoverFired(cons)) ++counters_.forced_pivot_handovers;

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
        ++counters_.integer_repairs;

        // This +delta lands in the new (post-forced-handover) SD
        // frame, while the smoother's x_pred is in the old frame;
        // folding it into x_pred would leave a residual pivot
        // offset.  The forced handover is disturbance-triggered (the
        // pivot slipped), so the smoother segments this
        // constellation anyway; the sat is also recorded as a reset
        // so it is segmented even if the handover classification
        // missed it (audit F1 / R4).
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

} // namespace GnssProcessor
