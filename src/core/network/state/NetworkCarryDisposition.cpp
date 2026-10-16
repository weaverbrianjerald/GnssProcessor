/**
 * @file NetworkCarryDisposition.cpp
 * @brief Implementation of the decided-once fold disposition (card PRM phase 1b).  The
 *        design and the order of the clauses are in the header's file comment.
 */

#include "network/state/NetworkCarryDisposition.hpp"

#include "network/state/NetworkRunContext.hpp"
#include "common/GnssIdentity.hpp"   // letterOfSat
#include "network/solve/NetworkClockAnchor.hpp"
#include "network/carry/NetworkPivotFrameQ.hpp"
#include "math/TimeUtils.hpp"

#include <algorithm>
#include <map>

// The run-end reports at the end of this file (card NOM step TD1b):
#include "config/GnssConfig.hpp"
#include "network/runend/NetworkRunEnd.hpp"
#include "network/diag/NetworkRunTraces.hpp"
#include <fmt/format.h>
#include <memory>
#include <spdlog/spdlog.h>
#include <string>
#include "network/driver/NetworkEpoch.hpp"   // NetworkEpoch (card NOM3 step EP1)
#include "network/driver/NetworkRun.hpp"     // NetworkRun (card NOM3 step EP1)
#include "network/driver/NetworkSetupResult.hpp"
#include "rinex/MultiRinexSync.hpp"

namespace GnssProcessor {

bool carryColumnPresent(const NetworkEstimator& est, const CarryPrior& cp) {
    try {
        switch (cp.kind) {
            case SharedParamKind::Clock:     return est.clockColumn(cp.sat) >= 0;
            case SharedParamKind::CodeBias:  return est.codeBiasColumn(cp.sat, cp.signal) >= 0;
            case SharedParamKind::PhaseBias: return est.phaseBiasColumn(cp.sat, cp.signal) >= 0;
            default:                         return false;
        }
    } catch (...) {
        return false;
    }
}

CarryFoldDecision carryFoldDecide(const NetworkRunContext& ctx, const NetworkEstimator& est,
                                  const CarryPrior& cp, double dt, double cta_now_s,
                                  double readmit_now_s, const EpochEvents& events) {
    CarryFoldDecision d;
    d.var_decided_m2  = cp.var;
    d.value_decided_m = cp.value;
    const auto& k = ctx.knobs;
    if (cp.kind == SharedParamKind::VtecSh) {             // the SH prior is folded before the gate
        d.disposition = CarryDisposition::Held;
        return d;
    }
    if (cp.kind == SharedParamKind::PhaseBias
        && (events.phase_datum.eq34_reref.count({cp.sat, cp.signal})
            || ctx.carry.phase_anchor_reref.count({cp.sat, cp.signal}))) {
        d.disposition = CarryDisposition::Reref;
        return d;
    }
    if (k.clk_model_on && cp.kind == SharedParamKind::Clock) {       // clock is on clock_osc
        d.disposition = CarryDisposition::OnOscillator;
        return d;
    }
    const double q = ctx.params.q(cp.kind);   // the registry (card PRM phase 1)
    const double readmit_h = k.readmit_seed_horizon_s;
    double seed_gap = 0.0;
    if (cp.t_park_s > 0.0) {
        if (!(readmit_h > 0.0) || !carryColumnPresent(est, cp)) {
            d.disposition = CarryDisposition::Parked;
            return d;
        }
        seed_gap = readmit_now_s - cp.t_park_s;
        if (!(seed_gap > 0.0) || seed_gap > readmit_h) {
            d.disposition = CarryDisposition::Parked;
            return d;
        }
    }
    d.seed_gap_s = seed_gap;
    if (cp.inflate_q) {
        d.disposition = CarryDisposition::Reinit;
    } else {
        d.disposition = (seed_gap > 0.0) ? CarryDisposition::Seeded : CarryDisposition::Folded;
        d.dt_applied_s = (seed_gap > 0.0) ? seed_gap : std::max(0.0, dt);
        d.rate_m2ps    = q;
        d.q_applied_m2 = q * d.dt_applied_s;
        if (k.cta_on && cp.kind == SharedParamKind::Clock) {
            const ClockAnchorFold f = clockAnchorFoldDecide(
                ctx, cp.sat, cp.var, dt, cta_now_s, seed_gap > 0.0 ? readmit_h : 0.0);
            d.q_applied_m2 = f.inc_m2;
            d.dt_applied_s = f.dt_s;
            d.rate_m2ps    = f.rate_m2ps;
            d.anchored     = f.anchored;
            d.decoupled    = f.decoupled;
            if (!(seed_gap > 0.0))
                d.disposition = f.anchored ? CarryDisposition::Anchored : CarryDisposition::Loose;
        }
        // Card CTU G5: a switch-on seam's level noise (the dense fold adds it).
        if (!ctx.carry.clm_seam_q.empty()
            && (cp.kind == SharedParamKind::Clock || cp.kind == SharedParamKind::PhaseBias)) {
            const auto sq = ctx.carry.clm_seam_q.find(cp.sat);
            if (sq != ctx.carry.clm_seam_q.end()) {
                d.has_seam  = true;
                d.seam_q_m2 = sq->second;
            }
        }
    }
    d.departed = !carryColumnPresent(est, cp);
    return d;
}

double carryFoldVarEff(const NetworkRunContext& ctx, const CarryPrior& cp,
                       const CarryFoldDecision& d, double var, bool dense,
                       ClockAnchorFold* anchor) {
    const auto& k = ctx.knobs;
    // A monitor-flagged clock folds at the reinit variance: a deliberate soft refloat,
    // outside the floors, the VCE factor and the seam (each excludes inflate_q).
    if (d.disposition == CarryDisposition::Reinit) return k.clk_mon_reinit_var;
    double var_eff;
    if (k.cta_on && cp.kind == SharedParamKind::Clock) {
        ClockAnchorFold f;
        f.inc_m2    = d.q_applied_m2;
        f.anchored  = d.anchored;
        f.decoupled = d.decoupled;
        f = clockAnchorFoldAt(ctx, f, var);
        if (anchor) *anchor = f;
        var_eff = f.var_eff;
    } else {
        var_eff = var + d.q_applied_m2;
    }
    // The B4 floor of a phase or code bias (the clock has its own model and monitor).
    const double floor = (cp.kind == SharedParamKind::PhaseBias) ? k.pb_var_floor
                       : (cp.kind == SharedParamKind::CodeBias)  ? k.cb_var_floor
                                                                 : 0.0;
    if (floor > 0.0) var_eff = std::max(var_eff, floor);
    // The VCE apply factor (M2 v2): the LASER 5-95 variance factor, applied after the hand
    // model has formed the variance (q over the gap, then the B4 floor).  A reinit is
    // excluded above: it is a deliberate soft refloat, not the hand carry model, and the
    // monitor skips those rows too, so there is no measured factor for them.  An unarmed
    // family's factor is 1.
    if (k.vce_apply_on) {
        const int slot = (cp.kind == SharedParamKind::PhaseBias) ? NetworkCarryVceApply::kPb
                       : (cp.kind == SharedParamKind::CodeBias)  ? NetworkCarryVceApply::kCb
                                                                 : -1;
        var_eff = var_eff * ctx.carry.vce_apply.factor(slot, cp.sat);
    }
    if (dense && d.has_seam) var_eff += d.seam_q_m2;
    return var_eff;
}

void networkCarryDecide(NetworkRun& run, const NetworkEpoch& ep) {
    // ---- From the run and the epoch (card NOM3 step EP2; generated by
    //      scripts/nom_ep2_adapters.py: the explicit form's parameters, each bound to the
    //      argument the retired adapter forwarded). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    NetworkRunContext& ctx = run.ctx;
    const NetworkEstimator& est = run.engines.est;
    const GnssTime& t = ep.se.time;
    double dt = ep.clock.dt;
    const EpochEvents& events = ep.events;
    // ---- End of the bindings ----
    if (ctx.carry.prev_carry.empty()) return;
    const double now_s = TimeUtils::continuousGpsSeconds(t);
    const double cta_now_s     = ctx.knobs.cta_on ? now_s : 0.0;
    const double readmit_now_s = (ctx.knobs.readmit_seed_horizon_s > 0.0) ? now_s : 0.0;
    for (CarryPrior& cp : ctx.carry.prev_carry)
        cp.fold = carryFoldDecide(ctx, est, cp, dt, cta_now_s, readmit_now_s, events);
    networkCarryPivotFrame(ctx);
    ++ctx.counters.prm_decide_epochs;
}

void networkCarryPivotFrame(NetworkRunContext& ctx) {
    if (!ctx.knobs.pfq_on) return;
    PivotFrameCounters& C = ctx.counters.pfq;
    ++C.epochs;
    const auto folds = [](const CarryFoldDecision& d) {
        return carryDispositionFolds(d.disposition) && d.disposition != CarryDisposition::Reinit;
    };
    // The pivot's rate, per constellation.  The stash skips pinned columns, so the pinned
    // pivot is not carried and has no decision of its own: its rate is the model's physical
    // per-satellite rate, the anchor's q with the anchor armed (card Q3's measured walk of
    // the corrections) and the legacy rate without it.  The legacy rate is not used for an
    // armed anchor: under the anchor it is the LOOSE decoupling bound, not a walk.  A
    // carried pivot (no current posture carries one) uses its own decided rate.
    const double r_model = ctx.knobs.cta_on ? ctx.knobs.cta_q
                                            : ctx.params.q(SharedParamKind::Clock);
    std::map<char, double> rate_of;
    net::forEach(ctx.entities.constellations, &net::ConstellationState::pfq_pivot,
                 [&](System sys, const std::string& sat) {
        const char cc = rinexLetter(sys);
        const CarryPrior* pv = nullptr;
        for (const CarryPrior& cp : ctx.carry.prev_carry)
            if (cp.kind == SharedParamKind::Clock && cp.sat == sat) { pv = &cp; break; }
        double r = 0.0;
        if (pv && folds(pv->fold) && pv->fold.rate_m2ps > 0.0) {
            r = pv->fold.rate_m2ps;
            if (pv->fold.anchored) ++C.pivot_anchored;
            else                   ++C.pivot_loose;
        } else {
            r = r_model;
            ++C.pivot_model;
        }
        rate_of[cc] = r;
        if (r > C.rate_max) C.rate_max = r;
    });
    // Every other folding clock of the constellation takes the pivot's walk over its own
    // interval (pivotFrameDiag); the pairwise term between two of them is added where the
    // covariance is dense (the C1 fold and the exact coupling update), from pfq_rate_m2ps.
    for (CarryPrior& cp : ctx.carry.prev_carry) {
        if (cp.kind != SharedParamKind::Clock || cp.sat.empty()) continue;
        CarryFoldDecision& d = cp.fold;
        d.pfq_rate_m2ps = 0.0;
        d.pfq_add_m2    = 0.0;
        const char cons = letterOfSat(cp.sat);            // the key of rate_of
        const std::string* pv = net::get(ctx.entities.constellations, systemOfSat(cp.sat),
                                         &net::ConstellationState::pfq_pivot);
        if (pv == nullptr) continue;                      // no pivot recorded for it
        if (cp.sat == *pv) continue;                      // the pivot is 0 by definition
        if (!folds(d)) continue;
        const double r = rate_of[cons];
        const double add = pivotFrameDiag(r, d.dt_applied_s);
        if (!(add > 0.0)) continue;
        d.q_applied_m2  += add;
        d.pfq_rate_m2ps  = r;
        d.pfq_add_m2     = add;
        ++C.keys;
        C.add_sum_m2 += add;
    }
}

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

/**
 * @brief Run-end section: the pivot-frame clock noise (PFQ).
 *
 * The pivot-frame clock noise (card PFQ): clocks that took the pivot's walk, at which
 * rate source, and the pairs the dense fold and the exact update carried.
 *
 * Gate: returns 0 at once unless `ctx.knobs.pfq_on`.
 *
 * @param v The run at its end (filter/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
static int networkPivotFrameQRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const std::shared_ptr<spdlog::logger>& log = v.log;

    if (!ctx.knobs.pfq_on) return 0;
    // Card PFQ run-end telemetry (invariant 4): how many clocks took the pivot's walk, at
    // which rate source, and how many pairs the dense fold and the exact update carried.
    const PivotFrameCounters& P = ctx.counters.pfq;
    log->info("Network (card PFQ / pivot-frame clock noise): {} decision pass(es); {} clock "
              "key-epoch(s) took r_p dt (summed {:.4e} m^2); pivot rate: the model's (pinned "
              "pivot, not carried) {} / a carried pivot anchored {} / loose {} "
              "(constellation-epochs), largest r_p {:.3e} m^2/s; pairs: {} in the dense fold, "
              "{} (ordered) in the exact update's Q_S.",
              P.epochs, P.keys, P.add_sum_m2, P.pivot_model, P.pivot_anchored,
              P.pivot_loose, P.rate_max, P.dense_pairs, P.cxu_pairs);
    if (P.keys == 0)
        log->warn("Network (card PFQ): network.clock_pivot_frame_q is ARMED but no clock took "
                  "the term (no recorded pivot, or no folding clock): the card is INERT for "
                  "this run.");
    return 0;
}

/**
 * @brief Run-end section: the fold's dispositions of the carried border (PRM 1b).
 *
 * The fold's dispositions of the carried border over the run (card PRM phase 1b), with
 * the `param_dispositions` manifest record.
 *
 * Gate: each section under its own condition, as in the teardown: always;
 * `C.prm_undecided_at_fold > 0`.
 *
 * @param v The run at its end (filter/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
static int networkCarryDispositionTableRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    NetworkRunTraces& traces = v.traces;

    // Card PRM phase 1b: what the fold applied to the carried border over the run, per
    // kind and disposition (key-epochs), decided once before PASS B; the same counts go
    // to the run manifest as a `param_dispositions` record.
    const auto& C = ctx.counters;
    static const char* const kKindName[4] = {"sat_clock", "sat_code_bias", "sat_phase_bias",
                                             "vtec_sh"};
    std::string js = "{\"record\":\"param_dispositions\",\"decide_epochs\":"
                   + std::to_string(C.prm_decide_epochs) + ",\"kinds\":[";
    for (int kk = 0; kk < 4; ++kk) {
        std::string line;
        std::string jk = fmt::format("{}{{\"kind\":\"{}\",\"departed\":{}", kk ? "," : "",
                                     kKindName[kk], C.prm_departed[kk]);
        long long tot = 0;
        for (int dd = 1; dd < kCarryDispositions; ++dd) {
            const long long n = C.prm_disp[kk][dd];
            jk += fmt::format(",\"{}\":{}", carryDispositionName(static_cast<CarryDisposition>(dd)), n);
            tot += n;
            if (n > 0)
                line += fmt::format("{}{} {}", line.empty() ? "" : ", ",
                                    carryDispositionName(static_cast<CarryDisposition>(dd)), n);
        }
        js += jk + "}";
        if (tot > 0)
            log->info("Network (param model, run end): {} fold dispositions over the run: {}"
                      " ({} of the folded with the column departed).",
                      kKindName[kk], line, C.prm_departed[kk]);
    }
    js += fmt::format("],\"reexpressed_after_decision\":{},\"reexpressed_epochs\":{},"
                      "\"reexpressed_max_dvar_m2\":{:.6e},\"undecided_at_fold\":{}}}",
                      C.prm_reexpressed, C.prm_reexpressed_epochs,
                      C.prm_reexpressed_max_dvar, C.prm_undecided_at_fold);
    traces.appendRunManifestRecord(config_.getNetworkConfig(), js);
    log->info("Network (card PRM phase 1b): the border's fold decided once before PASS B "
              "over {} epoch(s); {} key-epoch(s) re-expressed between the decision and the "
              "fold in {} epoch(s) (largest variance change {:.3e} m^2: a clock-pivot "
              "handover's S-transform, after the coupling rows were built); {} entr(ies) "
              "met undecided at the fold.",
              C.prm_decide_epochs, C.prm_reexpressed, C.prm_reexpressed_epochs,
              C.prm_reexpressed_max_dvar, C.prm_undecided_at_fold);
    if (C.prm_undecided_at_fold > 0)
        log->warn("Network (card PRM phase 1b): {} entr(ies) reached the fold with no "
                  "decision this epoch and were decided there; the coupling rows did not "
                  "read a decision for them.", C.prm_undecided_at_fold);
    return 0;
}

// Run-end report of this subsystem: its sections in print order (card NOM2 step TD2;
// declared in filter/NetworkCarryDisposition.hpp).
int networkCarryDispositionRunEnd(const NetworkRunEndView& v) {
    return networkRunEndSections(v, {
        networkPivotFrameQRunEnd,
        networkCarryDispositionTableRunEnd
    });
}

// ---- End of the run-end reports ----

}  // namespace GnssProcessor
