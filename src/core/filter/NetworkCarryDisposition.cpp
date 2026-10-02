/**
 * @file NetworkCarryDisposition.cpp
 * @brief Implementation of the decided-once fold disposition (card PRM phase 1b).  The
 *        design and the order of the clauses are in the header's file comment.
 */

#include "filter/NetworkCarryDisposition.hpp"

#include "app/NetworkRunContext.hpp"
#include "filter/NetworkClockAnchor.hpp"
#include "math/TimeUtils.hpp"

#include <algorithm>

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
        d.q_applied_m2 = q * (seed_gap > 0.0 ? seed_gap : std::max(0.0, dt));
        if (k.cta_on && cp.kind == SharedParamKind::Clock) {
            const ClockAnchorFold f = clockAnchorFoldDecide(
                ctx, cp.sat, cp.var, dt, cta_now_s, seed_gap > 0.0 ? readmit_h : 0.0);
            d.q_applied_m2 = f.inc_m2;
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
    ++ctx.counters.prm_decide_epochs;
}

}  // namespace GnssProcessor
