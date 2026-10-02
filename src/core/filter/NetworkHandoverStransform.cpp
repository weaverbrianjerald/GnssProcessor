/**
 * @file NetworkHandoverStransform.cpp
 * @brief Implementation of the clock-pivot handover S-transform (queue 1(b),
 *        docs/SPEC_pivot_visibility_gauge_defect.md §8).
 */

#include "filter/NetworkHandoverStransform.hpp"

#include "filter/NetworkPivotHandover.hpp"   // pivotHandoverSignalled
#include "filter/NetworkSolveGlue.hpp"       // networkClockDatumDeclaration
#include "math/TimeUtils.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

#include <fmt/format.h>

namespace GnssProcessor {

ClockStransformResult applyClockStransform(
    NetworkCarryState& carry,
    const std::vector<std::pair<std::string, double>>& v_c,
    const std::string& q, CarryMechanism mech, CarrySiteCounts* counts) {
    ClockStransformResult out;
    if (v_c.empty() || q.empty()) return out;

    // Membership and coefficients come from the structural direction alone.
    std::map<std::string, double> coeff;
    for (const auto& e : v_c) coeff.emplace(e.first, e.second);

    const auto qit = coeff.find(q);
    if (qit == coeff.end() || !(std::abs(qit->second) > 0.0)) return out;
    const double vq = qit->second;

    // x_q_pre / var_q: the incoming pivot's carried level in the outgoing frame (the first
    // carried clock entry of q).  Absent: nothing to re-reference from.
    const CarryPrior* qrow = nullptr;
    forEachCarriedReference(carry, carrySiteBit(CarrySite::PrevCarry), [&](const CarriedRef& r) {
        if (!qrow && r.kind == SharedParamKind::Clock && r.sat == q) qrow = r.prior;
        return false;
    });
    if (qrow == nullptr) return out;
    out.c0    = qrow->value / vq;
    out.var_q = qrow->var;
    const double c0 = out.c0, var_q = out.var_q;

    // Before the rows, the transform also reaches the satellites of the constellation that
    // are not registered this epoch (a parked entry, a coupling term on an absent clock): the
    // structural direction is the constellation's common shift, coefficient 1
    // (NetworkSbasis::clockNullDirection).
    const bool wide = (mech == CarryMechanism::HandoverBeforeRows);
    const std::string cq = NetworkSbasis::constellationOf(q);
    const auto coeffOf = [&](const std::string& sat, double& vi) {
        const auto cit = coeff.find(sat);
        if (cit != coeff.end()) { vi = cit->second; return true; }
        if (wide && !sat.empty() && NetworkSbasis::constellationOf(sat) == cq) { vi = 1.0; return true; }
        return false;
    };
    // x <- x - v_c*c0 ; var <- var + v_c[i]^2 * var_q.  Clocks only: every other shared
    // family has a zero component along this direction.
    CarrySiteCounts local;
    CarrySiteCounts& c = counts ? *counts : local;
    const long long before = c.moved[static_cast<int>(CarrySite::PrevCarry)];
    forEachCarriedReference(
        carry, carryMechanismSites(mech),
        [&](const CarriedRef& r) -> bool {
            if (r.kind != SharedParamKind::Clock) return false;
            double vi = 0.0;
            if (!coeffOf(r.sat, vi)) return false;
            *r.value -= vi * c0;
            if (r.site == CarrySite::PrevCarry) *r.var += vi * vi * var_q;
            else if (r.var && *r.var > 0.0) *r.var += vi * vi * var_q;   // a free term's p_prev
            return true;
        },
        &c);
    out.rows = c.moved[static_cast<int>(CarrySite::PrevCarry)] - before;
    if (out.rows == 0) return out;
    out.applied = true;

    // Exactness self-check (§8.4 INC-3).
    bool q_seen = false;
    forEachCarriedReference(carry, carrySiteBit(CarrySite::PrevCarry), [&](const CarriedRef& r) {
        if (!q_seen && r.kind == SharedParamKind::Clock && r.sat == q) {
            out.x_q_post = *r.value;
            q_seen = true;
        }
        return false;
    });
    out.selfcheck_ok = std::abs(out.x_q_post) <= kStsExactTolM
                    && std::abs(out.x_q_post / vq) <= kStsExactTolM;
    return out;
}

long long undoClockStransform(NetworkCarryState& carry, char constellation, double c0,
                              double var_q, CarrySiteCounts* counts) {
    return forEachCarriedReference(
        carry, carryMechanismSites(CarryMechanism::HandoverBeforeRows),
        [&](const CarriedRef& r) -> bool {
            if (r.kind != SharedParamKind::Clock || r.sat.empty()) return false;
            if (NetworkSbasis::constellationOf(r.sat) != std::string(1, constellation))
                return false;
            *r.value += c0;
            if (r.site == CarrySite::PrevCarry) *r.var -= var_q;
            else if (r.var && *r.var > 0.0) *r.var -= var_q;   // a free term's p_prev
            return true;
        },
        counts);
}

void networkHandoverStransformBeforeRows(NetworkRunContext& ctx,
                                         const GnssConfig& config_,
                                         const std::shared_ptr<spdlog::logger>& log,
                                         NetworkRunTraces& traces,
                                         const NetworkSbasis::SatHealth& sat_health,
                                         const NetworkEstimator& est,
                                         const GnssTime& t,
                                         const std::vector<std::string>& registered_clock_sats,
                                         double clk_anchor_sigma) {
    (void)log;   // seam signature is uniform across the run loop
    // The record is per-epoch state: cleared first, so the solve glue never reads an earlier
    // epoch's preview, and empty with the knob off.
    ctx.carry.sts_pre.clear();
    if (!ctx.knobs.handover_stransform_before_rows) return;

    // The election and the handover test the solve glue will make after PASS B, previewed
    // (both pure; one declaration).  The health filter is rebuilt here without its counters,
    // which the solve glue's election keeps.
    const SbasisDeclaration decl = networkClockDatumDeclaration(ctx, config_, clk_anchor_sigma);
    std::set<std::string> hlt_ineligible;
    if (ctx.knobs.sat_health != 0) {
        const double t_s = TimeUtils::continuousGpsSeconds(t);
        for (const std::string& s : registered_clock_sats)
            if (sat_health.unhealthyAt(s, t_s)) hlt_ineligible.insert(s);
    }
    NetworkSbasis::Plan plan;
    try {
        NetworkSbasis sb;
        plan = sb.buildSharedDatum(est, registered_clock_sats, decl,
                                   ctx.knobs.sticky_pivot ? &ctx.carry.last_pivot : nullptr,
                                   ctx.knobs.sat_health != 0 ? &hlt_ineligible : nullptr);
    } catch (const std::exception&) {
        return;   // the solve glue's election fails the same way and skips the epoch
    }
    std::set<char> changed;
    const bool datum_changed = pivotHandoverSignalled(
        plan.clock_pivot, ctx.carry.prev_pivot, ctx.carry.last_pivot,
        ctx.knobs.pivot_vr_signal, changed);
    // A transform an earlier preview applied and no stash has re-expressed since (the stash
    // clears the record): the epoch that applied it was skipped.  Kept when this epoch elects
    // the same pivot; otherwise undone, so the carry is back in the outgoing datum, the one
    // the handover test compares against, before anything else moves it.
    for (auto it = ctx.carry.sts_carry_pivot.begin(); it != ctx.carry.sts_carry_pivot.end();) {
        const auto e = plan.clock_pivot.find(std::string(1, it->first));
        if (e != plan.clock_pivot.end() && e->second == it->second.pivot) {
            ++it;
            continue;
        }
        CarrySiteCounts undo_cc;
        const long long n = undoClockStransform(ctx.carry, it->first, it->second.c0,
                                                it->second.var_q, &undo_cc);
        ++ctx.counters.prm_ref_calls[static_cast<int>(CarryMechanism::HandoverBeforeRows)];
        carryTallyMoved(ctx.counters.prm_ref_moved, CarryMechanism::HandoverBeforeRows, undo_cc);
        ++ctx.counters.sts_pre_undone;
        traces.emitEvent(t, "STRANSFORM", "sts_undone", it->second.pivot, "", it->second.c0,
                         std::nullopt, "undo",
                         fmt::format("const={};elected={};refs={}", it->first,
                                     e != plan.clock_pivot.end() ? e->second : std::string("none"),
                                     n));
        it = ctx.carry.sts_carry_pivot.erase(it);
    }
    if (!datum_changed) return;

    for (char c : changed) {
        const std::string key(1, c);
        const auto pit = plan.clock_pivot.find(key);
        if (pit == plan.clock_pivot.end() || pit->second.empty()) continue;
        const std::string& q = pit->second;   // the incoming pivot
        const auto v_c = NetworkSbasis::clockNullDirection(registered_clock_sats, key);
        if (v_c.empty()) continue;
        NetworkCarryState::StsBeforeRows& pre = ctx.carry.sts_pre[c];
        pre.pivot = q;
        const auto held = ctx.carry.sts_carry_pivot.find(c);
        if (held != ctx.carry.sts_carry_pivot.end() && held->second.pivot == q) {
            // Detected again after a skipped epoch: the carry is already in q's datum, where
            // a second transform would move nothing (c0 = 0) and add var_q again.
            pre.already = true;
            ++ctx.counters.sts_pre_redetected;
            continue;
        }
        // Every clock reference of the constellation the rows read, through the one
        // traversal (card PRM phase 2): the carried clocks, the coupling terms on them and
        // the stash's pivot value.
        CarrySiteCounts prm_cc;
        const ClockStransformResult r = applyClockStransform(
            ctx.carry, v_c, q, CarryMechanism::HandoverBeforeRows, &prm_cc);
        ++ctx.counters.prm_ref_calls[static_cast<int>(CarryMechanism::HandoverBeforeRows)];
        carryTallyMoved(ctx.counters.prm_ref_moved, CarryMechanism::HandoverBeforeRows, prm_cc);
        pre.applied      = r.applied;
        pre.c0           = r.c0;
        pre.var_q        = r.var_q;
        pre.rows         = r.rows;
        pre.x_q_post     = r.x_q_post;
        pre.selfcheck_ok = r.selfcheck_ok;
        if (r.applied) {
            ctx.carry.sts_carry_pivot[c] = NetworkCarryState::StsCarryDatum{q, r.c0, r.var_q};
            ++ctx.counters.sts_pre_edges;
        }
    }
}

void networkHandoverStransform(NetworkRunContext& ctx,
                               const GnssConfig& config_,
                               const std::shared_ptr<spdlog::logger>& log,
                               NetworkRunTraces& traces,
                               const GnssTime& t,
                               const NetworkSbasis::Plan& plan,
                               bool datum_changed,
                               const std::set<char>& changed_consts,
                               const std::vector<std::string>& registered_clock_sats) {
    (void)config_;   // seam signature is uniform across the run loop

    // The release set is per-epoch state.  Clear it unconditionally and first, so a
    // handover epoch's release cannot reach the next epoch's folds (which would
    // un-skip a retired datum without any signal).  Every non-handover epoch, and
    // every epoch with the knob off, therefore leaves an empty set: the off path is
    // byte-identical regardless of call order.
    ctx.carry.sts_consts.clear();
    ctx.carry.sts_c0.clear();

    if (!datum_changed || changed_consts.empty()) return;

    // Count every handover edge whether or not the transform is armed (the baseline
    // count increment 0 called for), so an off run reports the population an on run
    // acts on.
    for (char c : changed_consts) ++ctx.counters.pivot_handovers[c];

    if (!ctx.knobs.handover_stransform) return;

    for (char c : changed_consts) {
        const std::string key(1, c);
        const auto pit = plan.clock_pivot.find(key);
        if (pit == plan.clock_pivot.end() || pit->second.empty()) continue;
        const std::string& q = pit->second;   // the incoming pivot

        // v_c: the structural direction that NetworkSbasis owns (§8.1 rule 1).
        const auto v_c = NetworkSbasis::clockNullDirection(registered_clock_sats, key);
        if (v_c.empty()) continue;

        // With network.handover_stransform_before_rows the transform ran before PASS B
        // (networkHandoverStransformBeforeRows) and this seam does the bookkeeping from its
        // record.  The preview elected with the same declaration and inputs; an edge it did
        // not see is counted and transformed here, after the rows, as without the knob.
        ClockStransformResult r;
        bool before_rows = false;
        if (ctx.knobs.handover_stransform_before_rows) {
            const auto pre = ctx.carry.sts_pre.find(c);
            if (pre != ctx.carry.sts_pre.end() && pre->second.pivot == q) {
                if (pre->second.already) {
                    // The carry is already in the incoming datum (a re-detection after a
                    // skipped epoch): release the fold, transform nothing.  Counted as an edge
                    // with c0 = 0, as the transform after PASS B counts a re-detection (it runs
                    // the kernel again, which moves nothing), so the PIVOT row and the run-end
                    // clause read alike.
                    ++ctx.counters.sts_edges;
                    ctx.counters.sts_last_c0 = 0.0;
                    ctx.carry.sts_c0[c] = 0.0;
                    ctx.carry.sts_consts.insert(c);
                    traces.emitEvent(t, "STRANSFORM", "sts_already", q, "", std::nullopt,
                                     std::nullopt, "release",
                                     fmt::format("const={};reason=carry_in_incoming_datum", key));
                    continue;
                }
                r.applied      = pre->second.applied;
                r.c0           = pre->second.c0;
                r.var_q        = pre->second.var_q;
                r.rows         = pre->second.rows;
                r.x_q_post     = pre->second.x_q_post;
                r.selfcheck_ok = pre->second.selfcheck_ok;
                before_rows = true;
            } else {
                ++ctx.counters.sts_pre_mismatch;
                log->warn("Network (card PRM phase 2): the handover of {} to {} was not seen by "
                          "the preview before PASS B; it is transformed after the rows.",
                          key, q);
            }
        }
        if (!before_rows) {
            // Every carried clock of the constellation through the one traversal (card PRM
            // phase 2).
            CarrySiteCounts prm_cc;
            r = applyClockStransform(ctx.carry, v_c, q, CarryMechanism::Handover, &prm_cc);
            ++ctx.counters.prm_ref_calls[static_cast<int>(CarryMechanism::Handover)];
            carryTallyMoved(ctx.counters.prm_ref_moved, CarryMechanism::Handover, prm_cc);
        }
        if (!r.applied) {
            ++ctx.counters.sts_noop_no_carry;
            traces.emitEvent(t, "STRANSFORM", "sts_noop", q, "", std::nullopt, std::nullopt,
                             "skip",
                             fmt::format("const={};reason=no_carried_level_for_incoming_pivot",
                                         key));
            continue;
        }

        ctx.counters.sts_applied += r.rows;
        ++ctx.counters.sts_edges;
        ctx.counters.sts_last_c0 = r.c0;
        ctx.counters.sts_max_abs_c0 = std::max(ctx.counters.sts_max_abs_c0, std::abs(r.c0));
        ctx.carry.sts_c0[c] = r.c0;
        ctx.carry.sts_consts.insert(c);   // release the fold skip for this constellation (INC-1)

        if (!r.selfcheck_ok) {
            ++ctx.counters.sts_selfcheck_fail;
            log->warn("Network (queue 1(b) STRANSFORM): exactness self-check FAILED for {} — "
                      "after the transform the incoming pivot {} carries {:.3e} m (expected 0)."
                      "  The transform is mis-wired; the carry it released is NOT in the "
                      "incoming datum.",
                      key, q, r.x_q_post);
            traces.emitEvent(t, "STRANSFORM", "sts_selfcheck", q, "", r.x_q_post, kStsExactTolM,
                             "fail", fmt::format("const={};c0={:.6e}", key, r.c0));
        }

        // The decoupling edge, written as an event with identities (invariant #7,
        // §8.4 INC-2).  The re-coupling edge is the transformed carry's first fold:
        // counted as sts_rows_retained at the fold sites and summarised in the
        // run-end clause.
        traces.emitEvent(t, "STRANSFORM", "sts_applied", q, "", r.c0, std::nullopt,
                         "transform",
                         fmt::format("const={};c0={:.6f};rows={};var_q={:.6e};nv={}{}",
                                     key, r.c0, r.rows, r.var_q, v_c.size(),
                                     before_rows ? ";before_rows=1" : ""));
    }
}

}  // namespace GnssProcessor
