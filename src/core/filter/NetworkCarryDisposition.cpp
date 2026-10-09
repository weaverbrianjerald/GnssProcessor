/**
 * @file NetworkCarryDisposition.cpp
 * @brief Implementation of the decided-once fold disposition (card PRM phase 1b).  The
 *        design and the order of the clauses are in the header's file comment.
 */

#include "filter/NetworkCarryDisposition.hpp"

#include "app/NetworkRunContext.hpp"
#include "common/GnssIdentity.hpp"   // letterOfSat
#include "filter/NetworkClockAnchor.hpp"
#include "filter/NetworkPivotFrameQ.hpp"
#include "math/TimeUtils.hpp"

#include <algorithm>
#include <map>

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
                                  double readmit_now_s) {
    CarryFoldDecision d;
    d.var_decided_m2  = cp.var;
    d.value_decided_m = cp.value;
    const auto& k = ctx.knobs;
    if (cp.kind == SharedParamKind::VtecSh) {             // the SH prior is folded before the gate
        d.disposition = CarryDisposition::Held;
        return d;
    }
    if (k.phase_hold_on && cp.kind == SharedParamKind::PhaseBias) {   // Fix A hold
        d.disposition = CarryDisposition::Held;
        return d;
    }
    if (cp.kind == SharedParamKind::PhaseBias
        && (ctx.carry.eq34_reref.count({cp.sat, cp.signal})
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

void networkCarryDecide(NetworkRunContext& ctx, const NetworkEstimator& est, const GnssTime& t,
                        double dt) {
    if (ctx.carry.prev_carry.empty()) return;
    const double now_s = TimeUtils::continuousGpsSeconds(t);
    const double cta_now_s     = ctx.knobs.cta_on ? now_s : 0.0;
    const double readmit_now_s = (ctx.knobs.readmit_seed_horizon_s > 0.0) ? now_s : 0.0;
    for (CarryPrior& cp : ctx.carry.prev_carry)
        cp.fold = carryFoldDecide(ctx, est, cp, dt, cta_now_s, readmit_now_s);
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
    for (const auto& [cc, sat] : ctx.carry.pfq_pivot) {
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
    }
    // Every other folding clock of the constellation takes the pivot's walk over its own
    // interval (pivotFrameDiag); the pairwise term between two of them is added where the
    // covariance is dense (the C1 fold and the exact coupling update), from pfq_rate_m2ps.
    for (CarryPrior& cp : ctx.carry.prev_carry) {
        if (cp.kind != SharedParamKind::Clock || cp.sat.empty()) continue;
        CarryFoldDecision& d = cp.fold;
        d.pfq_rate_m2ps = 0.0;
        d.pfq_add_m2    = 0.0;
        const char cons = letterOfSat(cp.sat);            // the key of carry.pfq_pivot
        const auto pv = ctx.carry.pfq_pivot.find(cons);
        if (pv == ctx.carry.pfq_pivot.end()) continue;    // no pivot recorded for it
        if (cp.sat == pv->second) continue;               // the pivot is 0 by definition
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

}  // namespace GnssProcessor
