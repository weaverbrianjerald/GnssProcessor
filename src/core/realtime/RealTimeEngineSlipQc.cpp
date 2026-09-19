/**
 * @file RealTimeEngineSlipQc.cpp
 * @brief Cycle-slip detection and repair for RealTimeEngine::processEpoch.
 *
 * The largest single seam.  Roughly two thirds of it is reachable only with
 * estimation.cycle_slip.turbo_edit armed (the TurboEdit joint MW+GF repair
 * and the soft-LLI gap bridge); the remainder is the MW mandate path that
 * every real-time run takes.
 */

#include "realtime/RealTimeEngine.hpp"
#include "math/TimeUtils.hpp"
#include "common/PppUtils.hpp"
#include <map>
#include <set>

namespace GnssProcessor {

/**
 * @brief Cycle-slip detection / repair (MW + TurboEdit + soft-LLI bridge), run pre-estimation.
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::processEpoch
 * (frozen lines 1371-1631); see scripts/rt_seam_verify.py.
 */
void RealTimeEngine::detectAndRepairSlips(RtEpochContext& ctx) {
    auto log = log_manager_.getLogger("rt_engine");
    const std::string& epoch_prefix = ctx.epoch_prefix;
    const GnssTime& current_time = ctx.current_time;
    const std::vector<PppObservation>& modeled_obs = ctx.modeled_obs;
    const double dt = ctx.dt;

    const auto slip_results = mw_slip_detector_->detectLargeMwResets(modeled_obs);
    const double reset_std = config_.getAmbiguityStd();
    const double epoch_now_s = TimeUtils::continuousGpsSeconds(current_time);

    // Whether a satellite is the BSSD pivot for its constellation (its SD
    // ambiguity is frozen at 0 — a soft-LLI in-frame integer shift is
    // meaningless there; a pivot LLI needs a handover).  Hoisted so both
    // the TurboEdit block and the default LLI loop can consult it.
    auto isBssdPivot = [&](const std::string& sat) -> bool {
        if (!config_.enableBetweenSatelliteSd()) return false;
        const auto& pivots = meas_model_->getBssdPivotSatellites();
        auto it = pivots.find(PppUtils::getSystemString(sat));
        return it != pivots.end() && it->second == sat;
    };

    // --- Gap-hold registration (pre-pass) ---
    // A phase signal that DROPPED this epoch (no observation, no LLI)
    // with a recent converged snapshot is held rather than left to the
    // measurement model's prune-on-first-miss: the held key is
    // prune-protected and KF-excluded until the TurboEdit gap bridge
    // adjudicates the outage on the signal's return (mirrors
    // CycleSlipManager::detectAndRepair).  Must run before resolve() so
    // the resolver sees the signal held-continuous from the first
    // missing epoch.  No-op unless turbo_edit_bridge_lli.
    if (soft_lli_ && turbo_edit_resolver_ && soft_lli_->enabled())
        soft_lli_->registerGapHolds(modeled_obs, *state_manager_,
                                    isBssdPivot, *turbo_edit_resolver_,
                                    epoch_now_s, epoch_prefix);

    // Satellites whose soft-LLI-held ambiguities the bridge integer-repaired
    // this epoch (snapshot+dN·λ in the pre-pass below): both the TurboEdit
    // repair loop and the default reset loop skip them.
    std::set<std::string> soft_lli_repaired;

    // TurboEdit MW+GF joint repair (high-rate): pair the MW wide-lane
    // integer with the geometry-free phase jump to split a dual-frequency
    // slip into per-signal integer repairs (covariance preserved) and to
    // catch the MW dN1==dN2 blind spot.  Satellites it handles are skipped
    // by the default reset loop below; everything else falls back to a
    // plain ambiguity reset (the original real-time behavior).
    std::set<std::string> turbo_handled_sats;
    // MW-only code-domain events (GF affirmatively stable): per-sat phase
    // signals the resolver adjudicated clean.  The default reset loop
    // below skips a non-LLI MW mandate when the mandate's OWN signal
    // (the amb_key suffix) is adjudicated — per-mandate-signal, exactly
    // the post-processing CycleSlipManager rule, so a mandate on a pair
    // outside the GF solve (arc in warm-up / per-signal dropout) still
    // takes its un-adjudicated signal's reset.
    std::map<std::string, std::set<std::string>> code_clean_signals;
    // Satellites with a WL-settle deferral pending (bridged adjudication
    // waiting on the post-restart MW mean): mandates suppressed, holds
    // kept alive.  Filled inside the turbo block, consumed by the
    // default reset loop below.
    std::set<std::string> te_pending;
    if (turbo_edit_resolver_) {
        // The clean-pair set gates the resolver's GF-only equal-slip
        // split on affirmative MW evidence rather than mere MW silence
        // (a wl-locked arc absorbs real slips without emitting a result);
        // the across-gap WL evidence feeds the bridged solve's WL-settle
        // deferral.
        const auto te = turbo_edit_resolver_->resolve(
            slip_results, mw_slip_detector_->getEpochCleanPairs(),
            modeled_obs, dt,
            mw_slip_detector_->getAcrossGapWlEvidence(epoch_now_s));
        te_pending = te.pending_sats;

        // WL-settle deferral: a pending satellite's still-slipped phase
        // must stay out of the filter while the resolver waits — register
        // holds for its recently converged phase signals (idempotent;
        // the held keys feed the KF exclusion and prune protection).
        // Pivots are excluded as everywhere else; their deferral runs to
        // the resolver's timeout-reset.
        if (soft_lli_) {
            for (const auto& psat : te.pending_sats) {
                if (isBssdPivot(psat)) {
                    log->warn("{}TurboEdit WL-settle pending for BSSD "
                              "pivot {} — holds unavailable for a pivot; "
                              "deferral will run to its timeout-reset",
                              epoch_prefix, psat);
                    continue;
                }
                soft_lli_->registerHold(psat, {}, *turbo_edit_resolver_,
                                        epoch_now_s, epoch_prefix);
            }
        }

        // Soft-LLI bridge resolution (pre-pass): apply snapshot+dN·λ
        // covariance-restoring repairs to any held ambiguities the bridge
        // resolved this epoch, BEFORE the per-signal repair loop — a held
        // sat's reset value would otherwise make x += dN·λ wrong.  Repaired
        // sats are skipped by both loops below.  No-op unless something is
        // held (turbo_edit_bridge_lli).
        if (soft_lli_ && !soft_lli_->empty()) {
            int n_rep = 0, n_res = 0;
            const SoftLliBridge::ResetFn reset_fn =
                [&](const std::string& key, const std::string& /*sat*/,
                    const std::string& /*sig*/) {
                    state_manager_->resetAmbiguity(key, reset_std);
                };
            soft_lli_->resolveHolds(te, *turbo_edit_resolver_, modeled_obs,
                                    *state_manager_, epoch_now_s, epoch_prefix,
                                    soft_lli_repaired, n_rep, n_res, reset_fn);
            turbo_handled_sats.insert(soft_lli_repaired.begin(),
                                      soft_lli_repaired.end());
        }

        // Confident per-signal integer repairs (additive, covariance preserved).
        // The BSSD pivot's SD ambiguity is frozen at 0, so an integer shift is
        // meaningless there — reset instead and let the KF coherent-disturbance
        // detector / forwardKfPivotHandovers drive the re-referencing.
        for (const auto& r : te.repairs) {
            if (soft_lli_repaired.count(r.satellite)) continue;  // restored above
            const std::string key = r.satellite + "_" + r.signal;
            const int idx = state_manager_->getAmbiguityIndex(key);
            if (idx < 0) continue;
            if (isBssdPivot(r.satellite)) {
                state_manager_->resetAmbiguity(key, reset_std);
                log->info("{}TurboEdit repair refused for {} {} (BSSD pivot); reset",
                          epoch_prefix, r.satellite, r.signal);
            } else if (!(r.lambda > 0.0)) {
                state_manager_->resetAmbiguity(key, reset_std);
                log->info("{}TurboEdit repair refused for {} {} (invalid wavelength); reset",
                          epoch_prefix, r.satellite, r.signal);
            } else {
                Eigen::VectorXd x = state_manager_->getStateVector();
                const double prev = x(idx);
                x(idx) += static_cast<double>(r.dN) * r.lambda;
                state_manager_->setStateVector(x);
                state_manager_->noteAmbiguityDiscontinuity(key);
                log->info("{}TurboEdit REPAIR {} {}: dN={:+d} cyc, delta={:+.4f} m, "
                          "x: {:.4f} -> {:.4f} (covariance preserved)",
                          epoch_prefix, r.satellite, r.signal, r.dN,
                          static_cast<double>(r.dN) * r.lambda, prev, x(idx));
            }
            turbo_handled_sats.insert(r.satellite);
        }

        // Satellites the GF jump flagged but could not resolve (iono
        // disturbed / non-integer split): reset their phase ambiguities.
        for (const auto& sat : te.reset_sats) {
            const PppObservation* obs = nullptr;
            for (const auto& o : modeled_obs)
                if (o.satellite == sat) { obs = &o; break; }
            if (!obs) continue;
            for (const auto& [sig, val] : obs->observed) {
                (void)val;
                if (sig.empty() || sig[0] != 'L') continue;
                if (sig.find('_') != std::string::npos) continue;
                const std::string key = sat + "_" + sig;
                if (state_manager_->getAmbiguityIndex(key) < 0) continue;
                state_manager_->resetAmbiguity(key, reset_std);
            }
            turbo_handled_sats.insert(sat);
        }

        // MW-only code-domain events: MW flagged but the GF phase was
        // affirmatively stable under trusted conditions — the event
        // lives in the code observable (multipath / NLOS / DLL stress),
        // not the phase.  Record the adjudicated-clean signals so the
        // default loop below SKIPS the mandated phase reset; nothing is
        // touched here.  A wrongly vetoed real slip is a null-line pair
        // that lands ≥ |4·λ1| ≈ 0.76 m in the phase innovation at this
        // epoch's update — the KF residual-gate reset (plus the
        // MW/TurboEdit arc-clear that follows it below) is the backstop.
        for (const auto& ce : te.code_events) {
            code_clean_signals[ce.satellite].insert(ce.sig1);
            code_clean_signals[ce.satellite].insert(ce.sig2);
            log->info("{}TurboEdit code-domain event {} {}/{}: MW flagged "
                      "(dN_WL={:+d}{}) but GF stable (|TD2|={:.4f} m <= "
                      "{:.4f} m) — phase reset suppressed (KF residual "
                      "gate is the backstop)",
                      epoch_prefix, ce.satellite, ce.sig1, ce.sig2,
                      ce.dN_wl, ce.wl_known ? "" : ", integer unknown",
                      ce.td2_m, ce.zero_gate_m);
        }

        // Gap-bridge moot attributions (across-gap slip already absorbed
        // by the prune-and-reseed on the signal's return) and clean
        // adjudications (dN == 0 partners) share the code-event caller
        // semantic: suppress the mandate, touch nothing.
        for (const auto& [msat, sigs] : te.moot_signals) {
            code_clean_signals[msat].insert(sigs.begin(), sigs.end());
            for (const auto& s : sigs)
                log->info("{}TurboEdit bridged moot adjudication {} {}: "
                          "across-gap slip already absorbed by the fresh "
                          "reseed — phase reset suppressed",
                          epoch_prefix, msat, s);
        }
        for (const auto& [csat, sigs] : te.clean_signals)
            code_clean_signals[csat].insert(sigs.begin(), sigs.end());
    }

    // Default reset path for everything TurboEdit did not handle.  LLI
    // resets (empty signal pair) always reset — a loss-of-lock is
    // authoritative; pair slips on a TurboEdit-handled satellite are
    // skipped to preserve its targeted per-signal repair, and a mandate
    // whose OWN signal the code-domain veto adjudicated clean is
    // skipped to preserve the (believed-unslipped) phase ambiguity.
    // The per-mandate-signal test matches CycleSlipManager's
    // signalFromAmbiguityKey rule exactly: a mandate from a pair
    // OUTSIDE the GF solve still resets its un-adjudicated signal, and
    // a combined-IF amb_key (suffix "L1C_L2W") never matches the
    // raw-signal clean set, so the IF ambiguity resets — identical to
    // the post-processing behavior.
    for (const auto& sr : slip_results) {
        const bool is_lli = sr.sig1.empty() || sr.sig2.empty();
        std::string sat = sr.satellite;
        if (sat.empty()) {
            const auto p = sr.amb_key.find('_');
            sat = (p != std::string::npos) ? sr.amb_key.substr(0, p) : sr.amb_key;
        }
        // Bridge-restored this epoch (snapshot+dN·λ in the pre-pass) — its
        // ambiguities are already correct; do not reset them.
        if (soft_lli_repaired.count(sat)) continue;
        if (!is_lli && turbo_handled_sats.count(sat)) continue;
        // WL-settle deferral pending: mandate suppressed (the holds keep
        // the affected phase out of the filter), GF/MW arcs preserved,
        // until the deferral resolves in a later epoch.
        if (!is_lli && te_pending.count(sat)) {
            log->info("{}MW mandate for {} suppressed: TurboEdit WL-settle "
                      "deferral pending (holds active)", epoch_prefix, sat);
            continue;
        }
        if (!is_lli) {
            const auto cit = code_clean_signals.find(sat);
            if (cit != code_clean_signals.end()) {
                const auto p = sr.amb_key.find('_');
                if (p != std::string::npos &&
                    cit->second.count(sr.amb_key.substr(p + 1)))
                    continue;  // mandate's own signal adjudicated clean: keep the phase
            }
        }
        state_manager_->resetAmbiguity(sr.amb_key, reset_std);
        // A receiver-declared loss of lock also invalidates the
        // TurboEdit GF arc levels for the sat: a slip baked into
        // prev_gf would echo into the next epoch's TD2 (sign-flipped)
        // exactly when MW flags the pair with the true integer, and
        // the joint solve would corrupt the just-reseeded ambiguity.
        // The MW EWMA is deliberately preserved across LLI
        // (observation-domain history; see the detector's LLI branch).
        //
        // EXCEPTION — soft-LLI bridge: for an AR-capable, non-pivot sat at
        // high rate, HOLD the ambiguities across the (short) LLI gap and
        // let the bridge integer-repair them at gap return.  That needs the
        // GF arc's pre-gap level to SURVIVE, so do NOT reset it here; the
        // hold snapshots the pre-gap values and marks the signals
        // held-continuous in the resolver.
        if (is_lli && turbo_edit_resolver_) {
            if (soft_lli_ && !isBssdPivot(sat) &&
                soft_lli_->qualifies(sat, epoch_now_s)) {
                soft_lli_->registerHold(sat, { sr.amb_key },
                                        *turbo_edit_resolver_, epoch_now_s,
                                        epoch_prefix);
                // GF arc intentionally PRESERVED (no resetSatellite).
            } else {
                turbo_edit_resolver_->resetSatellite(sat);
            }
        }
    }

    // Advance the soft-LLI measured-spacing marker for the high-rate gate.
    if (soft_lli_) soft_lli_->advanceEpoch(epoch_now_s);
}

} // namespace GnssProcessor
