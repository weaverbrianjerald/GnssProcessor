/**
 * @file HandoverStransform.cpp
 * @brief Implementation of the clock-pivot handover S-transform (queue 1(b),
 *        docs/SPEC_pivot_visibility_gauge_defect.md §8).
 */

#include "network/solve/HandoverStransform.hpp"

#include "common/GnssIdentity.hpp"
#include "network/solve/PivotHandover.hpp"   // pivotHandoverSignalled
#include "network/solve/ClockDatum.hpp"       // networkClockDatumDeclaration
#include "math/TimeUtils.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

#include <fmt/format.h>

// The run-end reports at the end of this file:
#include "network/state/RunContext.hpp"
#include "network/driver/SetupResult.hpp"
#include "config/GnssConfig.hpp"
#include "network/runend/RunEnd.hpp"
#include <memory>
#include <spdlog/spdlog.h>
#include <string>
#include "network/driver/Epoch.hpp"   // NetworkEpoch
#include "network/driver/Run.hpp"     // NetworkRun
#include "rinex/MultiRinexSync.hpp"

namespace GnssProcessor {

ClockStransformResult applyClockStransform(
    NetworkCarryState& carry, net::NetworkEntities& entities,
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
    forEachCarriedReference(carry, entities, carrySiteBit(CarrySite::PrevCarry), [&](const CarriedRef& r) {
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
    const System sys_q = systemOfSat(q);
    const auto coeffOf = [&](const std::string& sat, double& vi) {
        const auto cit = coeff.find(sat);
        if (cit != coeff.end()) { vi = cit->second; return true; }
        if (wide && !sat.empty() && systemOfSat(sat) == sys_q) { vi = 1.0; return true; }
        return false;
    };
    // x <- x - v_c*c0 ; var <- var + v_c[i]^2 * var_q.  Clocks only: every other shared
    // family has a zero component along this direction.
    CarrySiteCounts local;
    CarrySiteCounts& c = counts ? *counts : local;
    const long long before = c.moved[static_cast<int>(CarrySite::PrevCarry)];
    forEachCarriedReference(
        carry, entities, carryMechanismSites(mech),
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
    forEachCarriedReference(carry, entities, carrySiteBit(CarrySite::PrevCarry), [&](const CarriedRef& r) {
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

ClockStransformDenseResult applyClockStransformDense(NetworkCarryState& carry, net::NetworkEntities& entities, char c,
                                                     const std::string& q,
                                                     const std::string& p_out) {
    ClockStransformDenseResult out;
    SharedCarryCov& pcc = carry.prev_carry_cov;
    const Eigen::Index k = pcc.P.rows();
    if (k == 0 || pcc.P.cols() != k || pcc.row.empty()) {
        out.reason = "empty_block";
        return out;
    }
    if (q.empty() || q == p_out) {
        out.reason = "q_is_p";
        return out;
    }
    // c0: the incoming pivot's carried level (the structural direction's coefficient is 1).
    const CarryPrior* qcp = nullptr;
    for (const CarryPrior& cp : carry.prev_carry)
        if (cp.kind == SharedParamKind::Clock && cp.sat == q) {
            qcp = &cp;
            break;
        }
    if (qcp == nullptr) {
        out.reason = "no_q_entry";
        return out;
    }
    const auto qit = pcc.row.find(sharedCarryKey(SharedParamKind::Clock, q, ""));
    if (qit == pcc.row.end() || static_cast<Eigen::Index>(qit->second) >= k) {
        out.reason = "no_q_row";
        return out;
    }
    const Eigen::Index iq = static_cast<Eigen::Index>(qit->second);
    const double c0 = qcp->value;
    const System sys = systemFromLetter(c);

    // u: every clock row of the constellation in the block (its key is "<sat>|CLK").
    std::vector<char> u(static_cast<std::size_t>(k), 0);
    std::vector<Eigen::Index> U;
    for (const auto& [key, row] : pcc.row) {
        if (static_cast<Eigen::Index>(row) >= k) continue;
        if (key.size() > 4 && systemOfSat(key) == sys
            && key.compare(key.size() - 4, 4, "|CLK") == 0) {
            u[row] = 1;
            U.push_back(static_cast<Eigen::Index>(row));
        }
    }
    std::sort(U.begin(), U.end());
    const Eigen::VectorXd r = pcc.P.col(iq);   // a copy: P is updated in place below
    const double pqq = r(iq);
    out.c0    = c0;
    out.pqq   = pqq;
    out.k_old = static_cast<std::size_t>(k);
    out.iq    = static_cast<int>(iq);

    // The constellation's block before, for the contrast check (the update touches only its
    // rows and columns).
    const Eigen::Index nu = static_cast<Eigen::Index>(U.size());
    Eigen::MatrixXd Puu(nu, nu);
    for (Eigen::Index a = 0; a < nu; ++a)
        for (Eigen::Index b = 0; b < nu; ++b) Puu(a, b) = pcc.P(U[a], U[b]);

    // P' = P - u r' - r u' + P_qq u u', on the upper triangle of every (i, j) with u_i or u_j
    // nonzero, then mirrored; a pair with u = 0 on both sides is not touched.
    for (Eigen::Index i = 0; i < k; ++i) {
        for (Eigen::Index j = i; j < k; ++j) {
            const bool ui = u[static_cast<std::size_t>(i)] != 0;
            const bool uj = u[static_cast<std::size_t>(j)] != 0;
            if (!ui && !uj) continue;
            double v = pcc.P(i, j);
            if (ui && uj)  v = v - r(j) - r(i) + pqq;
            else if (ui)   v = v - r(j);
            else           v = v - r(i);
            pcc.P(i, j) = v;
            pcc.P(j, i) = v;
        }
    }
    pcc.P.row(iq).setZero();
    pcc.P.col(iq).setZero();

    // Values: every carried clock of the constellation moves by -c0 (q lands on 0); one with
    // a block row takes P'_ii, one without (parked) the diagonal rule.
    std::set<std::string> carried_clk;
    long long n_rows = 0, n_parked = 0;
    double var_saved = 0.0;
    forEachCarriedReference(
        carry, entities, carryMechanismSites(CarryMechanism::Handover),
        [&](const CarriedRef& ref) -> bool {
            if (ref.kind != SharedParamKind::Clock || ref.site != CarrySite::PrevCarry) return false;
            if (ref.sat.empty() || systemOfSat(ref.sat) != sys) return false;
            carried_clk.insert(ref.sat);
            *ref.value -= c0;
            const auto rit = pcc.row.find(sharedCarryKey(SharedParamKind::Clock, ref.sat, ""));
            if (rit != pcc.row.end() && static_cast<Eigen::Index>(rit->second) < k) {
                const Eigen::Index i = static_cast<Eigen::Index>(rit->second);
                if (ref.sat != q) var_saved += (*ref.var + pqq) - pcc.P(i, i);
                *ref.var = pcc.P(i, i);
                ++n_rows;
            } else {
                *ref.var += pqq;
                ++n_parked;
            }
            return true;
        },
        &out.counts);
    out.rows      = n_rows;
    out.parked    = n_parked;
    out.var_saved = var_saved;

    // The outgoing pivot: exact in the outgoing frame, so its row in the incoming frame is
    // -r_j + P_qq u_j and its value -c0.
    Eigen::Index ip = -1;
    if (!p_out.empty() && systemOfSat(p_out) == sys
        && !pcc.row.count(sharedCarryKey(SharedParamKind::Clock, p_out, ""))
        && !carried_clk.count(p_out)) {
        ip = k;
        pcc.P.conservativeResize(k + 1, k + 1);
        for (Eigen::Index j = 0; j < k; ++j) {
            const double v = (j == iq) ? 0.0
                           : (u[static_cast<std::size_t>(j)] ? -r(j) + pqq : -r(j));
            pcc.P(k, j) = v;
            pcc.P(j, k) = v;
        }
        pcc.P(k, k) = pqq;
        pcc.row[sharedCarryKey(SharedParamKind::Clock, p_out, "")] = static_cast<std::size_t>(k);
        CarryPrior cp;
        cp.kind  = SharedParamKind::Clock;
        cp.sat   = p_out;
        cp.value = -c0;
        cp.var   = pqq;
        carry.prev_carry.push_back(std::move(cp));
        out.p_added = true;
    }
    out.applied = true;

    // Self-check: the incoming pivot lands on 0, its row is exactly 0, the block is finite and
    // symmetric, and every clock contrast keeps its variance (the outgoing pivot's included,
    // whose outgoing-frame row is the zero row).
    for (const CarryPrior& cp : carry.prev_carry)
        if (cp.kind == SharedParamKind::Clock && cp.sat == q) {
            out.x_q_post = cp.value;
            break;
        }
    const bool zero_row = pcc.P.row(iq).cwiseAbs().maxCoeff() == 0.0
                       && pcc.P.col(iq).cwiseAbs().maxCoeff() == 0.0;
    const bool finite = pcc.P.allFinite();
    // Symmetric on the rows and columns the update wrote (the constellation's clocks and the
    // appended outgoing pivot).  The rest of the block is left as the stash wrote it, which
    // is symmetric only to rounding (the solve's covariance), and is not the kernel's to judge.
    bool symmetric = true;
    for (Eigen::Index a = 0; a < nu && symmetric; ++a)
        symmetric = (pcc.P.row(U[a]).transpose() - pcc.P.col(U[a])).cwiseAbs().maxCoeff() == 0.0;
    if (ip >= 0 && symmetric)
        symmetric = (pcc.P.row(ip).transpose() - pcc.P.col(ip)).cwiseAbs().maxCoeff() == 0.0;
    double dev = 0.0, scale = 0.0;
    for (Eigen::Index a = 0; a < nu; ++a) {
        scale = std::max(scale, std::abs(Puu(a, a)));
        for (Eigen::Index b = a + 1; b < nu; ++b) {
            const double before = Puu(a, a) + Puu(b, b) - 2.0 * Puu(a, b);
            const double after  = pcc.P(U[a], U[a]) + pcc.P(U[b], U[b]) - 2.0 * pcc.P(U[a], U[b]);
            dev = std::max(dev, std::abs(after - before));
        }
        if (ip >= 0) {   // x_p - x_j: before, Var(x_j) (x_p exact); after, the appended row
            const double after = pcc.P(ip, ip) + pcc.P(U[a], U[a]) - 2.0 * pcc.P(ip, U[a]);
            dev = std::max(dev, std::abs(after - Puu(a, a)));
        }
    }
    scale = std::max(scale, std::abs(pqq));
    out.contrast_maxdev = dev;
    out.selfcheck_ok = std::abs(out.x_q_post) <= kStsExactTolM && zero_row && finite && symmetric
                    && dev <= kStsDenseContrastTolRel * std::max(scale, 1e-300);
    return out;
}

long long undoClockStransform(NetworkCarryState& carry, net::NetworkEntities& entities,
                              char constellation, double c0,
                              double var_q, CarrySiteCounts* counts) {
    return forEachCarriedReference(
        carry, entities, carryMechanismSites(CarryMechanism::HandoverBeforeRows),
        [&](const CarriedRef& r) -> bool {
            if (r.kind != SharedParamKind::Clock || r.sat.empty()) return false;
            if (systemOfSat(r.sat) != systemFromLetter(constellation)) return false;
            *r.value += c0;
            if (r.site == CarrySite::PrevCarry) *r.var -= var_q;
            else if (r.var && *r.var > 0.0) *r.var -= var_q;   // a free term's p_prev
            return true;
        },
        counts);
}

void networkHandoverStransformBeforeRows(NetworkRun& run, const NetworkEpoch& ep, EpochEvents& events) {
    // ---- From the run and the epoch (generated by
    //      scripts/nom_ep2_adapters.py: the explicit form's parameters, each bound to the
    //      argument the retired adapter forwarded). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config_ = run.config;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    NetworkRunTraces& traces = run.traces;
    const NetworkSbasis::SatHealth& sat_health = run.setup.sat_health;
    const NetworkEstimator& est = run.engines.est;
    const GnssTime& t = ep.se.time;
    const std::vector<std::string>& registered_clock_sats = ep.columns.registered_clock_sats;
    EpochEvents::Handover& handover_ev = events.handover;   // written
    // ---- End of the bindings ----
    (void)log;   // seam signature is uniform across the run loop
    const double clk_anchor_sigma = ctx.knobs.clk_anchor_sigma;   // was a parameter
    // The record is an epoch event: empty with the knob off, and never an earlier epoch's.
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
    // The constellation pivots as the election reads them (one-letter key -> satellite).
    const std::map<std::string, std::string> prev_pivot =
        net::letterMap(ctx.entities.constellations, &net::ConstellationState::prev_pivot);
    const std::map<std::string, std::string> last_pivot =
        net::letterMap(ctx.entities.constellations, &net::ConstellationState::last_pivot);
    NetworkSbasis::Plan plan;
    try {
        NetworkSbasis sb;
        plan = sb.buildSharedDatum(est, registered_clock_sats, decl,
                                   ctx.knobs.sticky_pivot ? &last_pivot : nullptr,
                                   ctx.knobs.sat_health != 0 ? &hlt_ineligible : nullptr);
    } catch (const std::exception&) {
        return;   // the solve glue's election fails the same way and skips the epoch
    }
    std::set<char> changed;
    const bool datum_changed = pivotHandoverSignalled(
        plan.clock_pivot, prev_pivot, last_pivot,
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
        const long long n = undoClockStransform(ctx.carry, ctx.entities, it->first, it->second.c0,
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
        EpochEvents::Handover::StsBeforeRows& pre = handover_ev.sts_pre[c];
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
        // traversal: the carried clocks, the coupling terms on them and
        // the stash's pivot value.
        CarrySiteCounts prm_cc;
        const ClockStransformResult r = applyClockStransform(
            ctx.carry, ctx.entities, v_c, q, CarryMechanism::HandoverBeforeRows, &prm_cc);
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
                               const std::vector<std::string>& registered_clock_sats,
                               EpochEvents& events) {
    (void)config_;   // seam signature is uniform across the run loop

    // The release set is an epoch event: a handover epoch's release cannot reach the next
    // epoch's folds, and every non-handover epoch, and every epoch with the knob off, has
    // an empty set.  The dense record is empty unless a dense edge applies below.
    EpochEvents::Handover& handover_ev = events.handover;

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
            const auto pre = handover_ev.sts_pre.find(c);
            if (pre != handover_ev.sts_pre.end() && pre->second.pivot == q) {
                if (pre->second.already) {
                    // The carry is already in the incoming datum (a re-detection after a
                    // skipped epoch): release the fold, transform nothing.  Counted as an edge
                    // with c0 = 0, as the transform after PASS B counts a re-detection (it runs
                    // the kernel again, which moves nothing), so the PIVOT row and the run-end
                    // clause read alike.
                    ++ctx.counters.sts_edges;
                    ctx.counters.sts_last_c0 = 0.0;
                    handover_ev.sts_c0[c] = 0.0;
                    handover_ev.sts_consts.insert(c);
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
                log->warn("Network (handover S-transform): the handover of {} to {} was not seen by "
                          "the preview before PASS B; it is transformed after the rows.",
                          key, q);
            }
        }
        // The exact S P S^T over the values and the dense block together; an edge
        // the dense kernel cannot apply takes the diagonal kernel below (counted).
        std::string dense_detail;
        bool dense_done = false;
        if (!before_rows && ctx.knobs.handover_stransform_dense) {
            // The outgoing pivot is the frame the stashed block is in: the stash's pinned clock
            // (the exact update's record), else the last pivot the wire declared.
            std::string p_out;
            if (const std::optional<System> csys = net::constellationOfLetter(ctx.entities, c)) {
                const net::ConstellationTable& cons_rows = ctx.entities.constellations;
                if (const auto* cx = net::get(cons_rows, *csys, &net::ConstellationState::cxu_pivot))
                    p_out = cx->first;
                else if (const std::string* lp =
                             net::get(cons_rows, *csys, &net::ConstellationState::last_pivot))
                    p_out = *lp;
            }
            // The constellation's carried clocks that nothing moved between their fold decision
            // (before PASS B, the carry the rows were built on) and this transform.
            std::set<std::string> clean;
            for (const CarryPrior& cp : ctx.carry.prev_carry)
                if (cp.kind == SharedParamKind::Clock && !cp.sat.empty()
                    && systemOfSat(cp.sat) == systemFromLetter(c)
                    && cp.fold.disposition != CarryDisposition::Undecided
                    && cp.value == cp.fold.value_decided_m && cp.var == cp.fold.var_decided_m2)
                    clean.insert(cp.sat);
            const ClockStransformDenseResult d = applyClockStransformDense(ctx.carry, ctx.entities, c, q, p_out);
            if (d.applied) {
                ++ctx.counters.prm_ref_calls[static_cast<int>(CarryMechanism::Handover)];
                carryTallyMoved(ctx.counters.prm_ref_moved, CarryMechanism::Handover, d.counts);
                r.applied      = true;
                r.c0           = d.c0;
                r.var_q        = d.pqq;
                r.rows         = d.rows + d.parked;
                r.x_q_post     = d.x_q_post;
                r.selfcheck_ok = std::abs(d.x_q_post) <= kStsExactTolM;
                EpochEvents::Handover::StsDense& rec = handover_ev.sts_dense[c];
                rec.q            = q;
                rec.p            = p_out;
                rec.c0           = d.c0;
                rec.pqq          = d.pqq;
                rec.k_old        = d.k_old;
                rec.iq           = d.iq;
                rec.p_added      = d.p_added;
                rec.selfcheck_ok = d.selfcheck_ok;
                for (const auto& [rk, row] : ctx.carry.prev_carry_cov.row) {
                    (void)row;
                    if (rk.size() > 4 && systemOfSat(rk) == systemFromLetter(c)
                        && rk.compare(rk.size() - 4, 4, "|CLK") == 0)
                        rec.row_sats.insert(rk.substr(0, rk.size() - 4));
                }
                rec.clean = std::move(clean);
                ++ctx.counters.sts_dense_edges;
                ctx.counters.sts_dense_rows   += d.rows;
                ctx.counters.sts_dense_parked += d.parked;
                if (d.p_added) ++ctx.counters.sts_dense_p_added;
                ctx.counters.sts_dense_var_saved += d.var_saved;
                ctx.counters.sts_dense_contrast_maxdev =
                    std::max(ctx.counters.sts_dense_contrast_maxdev, d.contrast_maxdev);
                if (!d.selfcheck_ok) {
                    ++ctx.counters.sts_dense_selfcheck_fail;
                    log->warn("Network: the dense S-transform's self-check FAILED for "
                              "{} (incoming pivot {}): |x_q| {:.3e} m, contrast change {:.3e} m^2.  "
                              "The block is re-expressed but the clone fold and the anchor keep "
                              "will not use it.",
                              key, q, d.x_q_post, d.contrast_maxdev);
                }
                dense_detail = fmt::format(";dense=1;p={};p_added={};parked={};dvar_saved={:.6e};"
                                           "contrast_dev={:.3e};dense_ok={}",
                                           p_out.empty() ? std::string("none") : p_out,
                                           d.p_added ? 1 : 0, d.parked, d.var_saved,
                                           d.contrast_maxdev, d.selfcheck_ok ? 1 : 0);
                dense_done = true;
            } else {
                ++ctx.counters.sts_dense_fallback;
                traces.emitEvent(t, "STRANSFORM", "sts_dense_fallback", q, "", std::nullopt,
                                 std::nullopt, "fallback",
                                 fmt::format("const={};reason={};p={}", key, d.reason,
                                             p_out.empty() ? std::string("none") : p_out));
            }
        }
        if (!before_rows && !dense_done) {
            // Every carried clock of the constellation through the one traversal.
            CarrySiteCounts prm_cc;
            r = applyClockStransform(ctx.carry, ctx.entities, v_c, q, CarryMechanism::Handover, &prm_cc);
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
        handover_ev.sts_c0[c] = r.c0;
        handover_ev.sts_consts.insert(c);   // release the fold skip for this constellation (INC-1)

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
                         fmt::format("const={};c0={:.6f};rows={};var_q={:.6e};nv={}{}{}",
                                     key, r.c0, r.rows, r.var_q, v_c.size(),
                                     before_rows ? ";before_rows=1" : "", dense_detail));
    }
}

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

/**
 * @brief Run-end section: broadcast satellite health (HLT).
 *
 * Broadcast satellite health on the network path: flagged satellites, the
 * pivot election's skips and fallbacks, and the observations the setup door removed.
 *
 * Gate: returns 0 at once unless `ctx.knobs.sat_health != 0`.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
static int networkSatHealthRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const NetworkSetupResult& setup = v.setup;

    // Broadcast health on the network path (silent unless network.sat_health).
    // Standing invariant #4: the counters are the evidence the rule acted.
    if (!(ctx.knobs.sat_health != 0)) return 0;
    const auto& c = ctx.counters;
    long long removed = 0;
    std::string by_sat;
    for (const auto& [sat, n] : setup.sat_health_removed) {
        removed += n;
        by_sat += fmt::format("{}{} {}", by_sat.empty() ? "" : ", ", sat, n);
    }
    log->info("Network: sat_health '{}': {} (satellite, epoch) registered clock "
              "satellite(s) flagged unhealthy; the clock-pivot election passed over one at "
              "{} (constellation, epoch) and found none healthy at {}; {} observation(s) "
              "removed by the setup door inside the window{}{}.",
              ctx.knobs.sat_health == 1 ? "pivot" : "exclude", c.hlt_unhealthy_registered,
              c.hlt_pivot_skips, c.hlt_pivot_fallbacks, removed,
              by_sat.empty() ? "" : ": ", by_sat);
    if (ctx.knobs.sat_health == 1 && c.hlt_pivot_skips == 0)
        log->warn("Network: sat_health 'pivot' is armed but no pick passed over an "
                  "unhealthy satellite: INERT for this run.");
    if (ctx.knobs.sat_health == 2 && removed == 0)
        log->warn("Network: sat_health 'exclude' is armed but the door removed no "
                  "observation inside the window: INERT for this run.");
    return 0;
}

/**
 * @brief Run-end section: clock-pivot handovers and their S-transform.
 *
 * The clock-pivot handovers and their S-transform (PRM 2, SCL B2).
 *
 * Gate: returns 0 at once unless `!ctx.counters.pivot_handovers.empty() ||
 * ctx.knobs.handover_stransform`.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
static int networkPivotHandoverRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const std::shared_ptr<spdlog::logger>& log = v.log;

    // Clock-pivot handover baseline and the S-transform's ON-path telemetry (queue item
    // 1(b)).  The handover count is reported whether or not the transform is armed:
    // otherwise the edge appears only as a PIVOT event row and in the E/R records, and a
    // run with the transform off could not state the population an armed run acts on
    // (increment 0).  The sts_* clause follows only when the transform is armed.
    // `edges > 0` with `rows_retained == 0` means the INC-1 fold release is miswired: the
    // transform ran and nothing folded (invariant #4).
    if (!(!ctx.counters.pivot_handovers.empty() || ctx.knobs.handover_stransform)) return 0;
    const auto& sc = ctx.counters;
    std::string per_const;
    long long total = 0;
    for (const auto& kv : sc.pivot_handovers) {
        if (!per_const.empty()) per_const += ", ";
        per_const += fmt::format("{}:{}", kv.first, kv.second);
        total += kv.second;
    }
    if (per_const.empty()) per_const = "(none)";
    if (!ctx.knobs.handover_stransform) {
        log->info("Network (clock-pivot handovers): {} datum re-reference(s) over the run "
                  "[{}] — the constellation(s) whose pinned pivot satellite changed.  "
                  "network.handover_stransform is OFF, so each one DROPPED that "
                  "constellation's carried shared priors for one epoch (the free fall "
                  "SPEC_pivot_visibility_gauge_defect §8.2 names).",
                  total, per_const);
    } else {
        log->info("Network (queue 1(b) handover S-TRANSFORM): {} clock-pivot handover(s) "
                  "over the run [{}]; {} edge(s) TRANSFORMED, {} carried row(s) "
                  "re-expressed along the structural clock null direction, {} row(s) "
                  "RETAINED (folded that the baseline skip would have dropped — this is "
                  "the transform's ON-path counter), {} edge(s) no-op (the incoming pivot had "
                  "no carried level), {} self-check failure(s).  |c0| max {:.6f} m, last "
                  "{:.6f} m.{}",
                  total, per_const, sc.sts_edges, sc.sts_applied, sc.sts_rows_retained,
                  sc.sts_noop_no_carry, sc.sts_selfcheck_fail,
                  sc.sts_max_abs_c0, sc.sts_last_c0,
                  (sc.sts_edges > 0 && sc.sts_rows_retained == 0)
                      ? "  ⚠ DEAD WIRING: edges were transformed but NOTHING folded "
                        "— the INC-1 skip release is not reaching the fold sites."
                      : "");
        if (ctx.knobs.handover_stransform_before_rows) {
            log->info("Network (handover S-transform): the handover S-transform ran BEFORE THE "
                      "ROWS: {} edge(s) re-expressed before the fold decision and PASS B "
                      "(the rows of those epochs read the re-expressed carry), {} "
                      "re-detection(s) after a skipped epoch released without a second "
                      "transform, {} transform(s) undone after a skipped epoch (the next "
                      "election chose another pivot), {} edge(s) the preview did not see "
                      "(transformed after the rows).",
                      sc.sts_pre_edges, sc.sts_pre_redetected, sc.sts_pre_undone,
                      sc.sts_pre_mismatch);
            if (sc.sts_pre_mismatch > 0)
                log->warn("Network (handover S-transform): {} handover edge(s) were elected "
                          "differently by the preview before PASS B and by the solve glue; "
                          "the preview's declaration or inputs diverged.",
                          sc.sts_pre_mismatch);
        }
        // The dense kernel's on-path counters (invariant #4: handovers with 0
        // dense edges mean the kernel never ran).
        if (ctx.knobs.handover_stransform_dense) {
            log->info("Network (dense handover S-transform): {} edge(s) "
                      "re-expressed by S P S^T ({} carried clock(s) with a block row, {} "
                      "parked clock(s) by the diagonal rule, {} outgoing pivot(s) appended), "
                      "{} fallback(s) to the diagonal kernel, {} self-check failure(s); "
                      "variance the diagonal bump would have over-stated, summed {:.4e} m^2; "
                      "largest clock-contrast variance change {:.3e} m^2.{}",
                      sc.sts_dense_edges, sc.sts_dense_rows, sc.sts_dense_parked,
                      sc.sts_dense_p_added, sc.sts_dense_fallback,
                      sc.sts_dense_selfcheck_fail, sc.sts_dense_var_saved,
                      sc.sts_dense_contrast_maxdev,
                      (sc.sts_edges > 0 && sc.sts_dense_edges == 0)
                          ? "  DEAD WIRING: edges were transformed and the dense kernel "
                            "applied to none."
                          : "");
            if (ctx.knobs.cta_datum_keep)
                log->info("Network (anchor kept across the handover): {} "
                          "anchored clock(s) kept engaged at a dense-transformed handover "
                          "(no datum decouple, no disc mark), {} datum decouple(s) the keep "
                          "rule refused (no dense edge, no block row, or a loose fold), {} "
                          "datum decouple(s) in all.",
                          sc.cta_datum_kept, sc.cta_datum_kept_refused, sc.cta_trip_datum);
        }
    }
    return 0;
}

// Run-end report of this subsystem: its sections in print order (
// declared in network/solve/HandoverStransform.hpp).
int networkHandoverStransformRunEnd(const NetworkRunEndView& v) {
    return networkRunEndSections(v, {
        networkSatHealthRunEnd,
        networkPivotHandoverRunEnd
    });
}

// ---- End of the run-end reports ----

}  // namespace GnssProcessor
