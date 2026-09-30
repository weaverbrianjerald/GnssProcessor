/**
 * @file NetworkPbHoldClone.cpp
 * @brief The smoothed phase-bias hold's clock clone (card SMH): the pure advance and
 *        re-harvest of a clone row, and the two seams that run them each epoch.
 *
 * Seam contract: free functions over the run context that own no state; every clone
 * lives in @c ctx.carry.smh_clone.
 */

#include "filter/NetworkPbHoldClone.hpp"

#include "app/NetworkRunContext.hpp"
#include "filter/NetworkRunTypes.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace GnssProcessor {

namespace {

/// The key of a clone term in the dense carry's identity.
std::string termKey(const PbHoldCloneTerm& t) {
    return sharedCarryKey(t.kind, t.sat, t.signal);
}

/// Add @p d to the pinned clock term of @p sat in @p terms, or append one.
void addPinnedClock(std::vector<PbHoldCloneTerm>& terms, const std::string& sat,
                    double d, double value, Eigen::Index col) {
    for (PbHoldCloneTerm& t : terms) {
        if (t.kind == SharedParamKind::Clock && t.sat == sat && !(t.p_prev > 0.0)) {
            t.coeff += d;
            return;
        }
    }
    PbHoldCloneTerm t;
    t.kind   = SharedParamKind::Clock;
    t.sat    = sat;
    t.coeff  = d;
    t.s_prev = value;
    t.p_prev = 0.0;
    t.col    = col;
    terms.push_back(t);
}

void killClone(NetworkRunContext& ctx, PbHoldClone& c, PbHoldCloneDeath why) {
    c.alive = false;
    c.death = why;
    ++ctx.counters.smh_deaths[static_cast<int>(why)];
}

}  // namespace

PbHoldCloneStep pbHoldCloneAdvance(const PbHoldCloneInputs& in, const PbHoldClone& c) {
    PbHoldCloneStep out;
    const CouplingExactEpoch* xe = in.xe;
    if (!xe || !xe->ok) {
        out.death = PbHoldCloneDeath::Unfactored;
        return out;
    }
    if (std::abs(c.stamp_s - xe->stash_s) > 1e-6) {
        out.death = PbHoldCloneDeath::Stale;
        return out;
    }
    const std::string parent = sharedCarryKey(SharedParamKind::Clock, c.sat, "");
    std::vector<std::pair<int, double>> pterms;
    std::vector<PbHoldCloneTerm>        kept;         // pinned and outside-block terms
    std::map<char, double>              out_clk_def;  // clock-family deficit of those terms
    double dep_var = 0.0;                             // marginalized terms
    double tu_var  = 0.0;                             // kappa terms outside the block
    double rev     = 0.0;                             // the border's revision along the row
    for (const PbHoldCloneTerm& tm : c.terms) {
        const std::string key = termKey(tm);
        const auto pit = xe->p_of_key.find(key);
        if (pit != xe->p_of_key.end()) {
            // A stashed dense key: the one rule (S updated, D re-attributed through P).
            pterms.emplace_back(pit->second, tm.coeff);
            if (xe->s_of_p[static_cast<std::size_t>(pit->second)] >= 0) ++out.n_s;
            else                                                       ++out.n_d;
            continue;
        }
        const char   cc     = tm.sat.empty() ? '\0' : tm.sat[0];
        const bool   is_clk = (tm.kind == SharedParamKind::Clock);
        const Eigen::Index col = in.col_of ? in.col_of(tm) : -1;
        if (!(tm.p_prev > 0.0)) {
            // Datum-pinned: exactly known, kept as harvested, no revision.
            if (col < 0) {
                if (is_clk) out_clk_def[cc] += tm.coeff;
                continue;
            }
            PbHoldCloneTerm k = tm;
            k.col = col;
            kept.push_back(k);
            continue;
        }
        // Outside the dense block.  The clone's own clock there is card 1d's route in
        // all but name, which the card forbids: the clone dies.
        if (key == parent) {
            out.death = PbHoldCloneDeath::ParentOutside;
            return out;
        }
        ++out.n_outside;
        const double nan   = std::numeric_limits<double>::quiet_NaN();
        const auto   qo    = xe->q_outside.find(key);
        const double prior = in.prior_of ? in.prior_of(tm) : nan;
        const double post  = (col >= 0 && in.post_of_col) ? in.post_of_col(col) : nan;
        if (col < 0 || qo == xe->q_outside.end() || !std::isfinite(prior)
            || !std::isfinite(post)) {
            // Departed, or no fold inflation known for it: marginalized.
            dep_var += tm.coeff * tm.coeff * tm.p_prev;
            if (is_clk) out_clk_def[cc] += tm.coeff;
            continue;
        }
        // A diagonal leftover: the fold's own kappa, exact for an uncorrelated key.
        const double q   = std::max(0.0, qo->second);
        const double kap = tm.p_prev / (tm.p_prev + q);
        const double m   = kap * tm.coeff;
        tu_var += tm.coeff * tm.coeff * kap * q;
        if (is_clk) out_clk_def[cc] += tm.coeff - m;
        PbHoldCloneTerm k = tm;
        k.coeff = m;
        k.col   = col;
        kept.push_back(k);
        rev += m * (post - prior);
    }
    const CouplingExactRow xr = couplingExactRow(*xe, pterms);
    if (!xr.ok || !xr.m_new.allFinite() || !std::isfinite(xr.var_add)) {
        out.death = PbHoldCloneDeath::RowFailed;
        return out;
    }
    for (Eigen::Index s = 0; s < xe->nS(); ++s) {
        const double m = xr.m_new(s);
        if (m == 0.0) continue;
        const int p = xe->p_of_s[static_cast<std::size_t>(s)];
        PbHoldCloneTerm k;
        if (p >= 0 && static_cast<std::size_t>(p) < in.desc_of_p.size()) k = in.desc_of_p[static_cast<std::size_t>(p)];
        k.coeff = m;
        k.col   = xe->col_of_s[static_cast<std::size_t>(s)];
        // An S key is a free column (p_prev > 0 marks it so until the re-harvest sets the
        // new stash's variance); only a pinned term carries p_prev = 0.
        const double pp = (p >= 0 && p < xe->P.rows()) ? xe->P(p, p) : 0.0;
        k.s_prev = in.prior_s(s);
        k.p_prev = pp > 0.0 ? pp : 1e-12;
        out.terms.push_back(k);
        rev += m * (in.post_s(s) - in.prior_s(s));
    }
    // The clock-family deficit onto the stash's pinned pivot, as for an ambiguity row: the
    // row keeps its harvest identity along the declared clock gauge.  A pinned column is
    // exactly known, so the deficit adds no revision.
    std::map<char, double> def = xr.clk_deficit;
    for (const auto& [cc, d] : out_clk_def) def[cc] += d;
    for (const auto& [cc, d] : def) {
        if (d == 0.0) continue;
        const auto pv = xe->pivot.find(cc);
        const auto ps = in.pivot_sat.find(cc);
        if (pv == xe->pivot.end() || ps == in.pivot_sat.end()) {
            out.deficit_orphan = true;
            continue;
        }
        addPinnedClock(kept, ps->second, d, pv->second.second, pv->second.first);
    }
    out.terms.insert(out.terms.end(), kept.begin(), kept.end());
    out.value = c.value - rev;
    out.var   = c.var + xr.var_add + dep_var + tu_var;
    const auto pp = xe->p_of_key.find(parent);
    if (pp != xe->p_of_key.end()) {
        const int s = xe->s_of_p[static_cast<std::size_t>(pp->second)];
        if (s >= 0) {
            out.parent_in_s = true;
            out.dW          = xe->q_s(s);
        }
    }
    out.ok = true;
    return out;
}

std::vector<PbHoldCloneTerm> pbHoldCloneRebase(
    const std::vector<PbHoldCloneTerm>& terms,
    const std::map<std::string, std::pair<double, double>>& stash,
    const std::map<std::string, double>& pinned,
    const std::function<std::pair<double, double>(Eigen::Index)>& fallback) {
    double mx = 0.0;
    for (const PbHoldCloneTerm& t : terms) mx = std::max(mx, std::abs(t.coeff));
    std::vector<PbHoldCloneTerm> out;
    std::map<std::string, std::size_t> at;
    for (const PbHoldCloneTerm& t : terms) {
        if (!(std::abs(t.coeff) > kCouplingSparsifyRelThresh * mx)) continue;
        const std::string key = termKey(t);
        PbHoldCloneTerm k = t;
        const auto sit = stash.find(key);
        const auto pit = pinned.find(key);
        if (sit != stash.end()) {
            k.s_prev = sit->second.first;
            k.p_prev = sit->second.second;
        } else if (pit != pinned.end()) {
            k.s_prev = pit->second;
            k.p_prev = 0.0;
        } else if (t.col >= 0 && fallback) {
            const auto [v, var] = fallback(t.col);
            k.s_prev = v;
            k.p_prev = std::max(var, 1e-12);
        } else {
            continue;
        }
        const auto ait = at.find(key);
        if (ait != at.end()) {
            out[ait->second].coeff += k.coeff;
            continue;
        }
        at[key] = out.size();
        out.push_back(k);
    }
    return out;
}

CouplingExactEpoch pbHoldCloneRestrictEpoch(const CouplingExactEpoch& xe,
                                            const std::vector<int>& unfolded) {
    CouplingExactEpoch out = xe;
    if (unfolded.empty() || !xe.ok) return out;
    std::vector<int> new_of_old(static_cast<std::size_t>(xe.nS()), -1);
    out.p_of_s.clear();
    out.col_of_s.clear();
    std::vector<double> qs, sh;
    for (Eigen::Index s = 0; s < xe.nS(); ++s) {
        const int p = xe.p_of_s[static_cast<std::size_t>(s)];
        if (std::find(unfolded.begin(), unfolded.end(), p) != unfolded.end()) {
            out.s_of_p[static_cast<std::size_t>(p)]  = -1;
            out.route_p[static_cast<std::size_t>(p)] = static_cast<int>(CouplingExactDRoute::Reinit);
            continue;
        }
        new_of_old[static_cast<std::size_t>(s)] = static_cast<int>(out.p_of_s.size());
        out.s_of_p[static_cast<std::size_t>(p)] = static_cast<int>(out.p_of_s.size());
        out.p_of_s.push_back(p);
        out.col_of_s.push_back(xe.col_of_s[static_cast<std::size_t>(s)]);
        qs.push_back(xe.q_s(s));
        sh.push_back(xe.shat_s(s));
    }
    out.q_s    = Eigen::Map<const Eigen::VectorXd>(qs.data(), static_cast<Eigen::Index>(qs.size()));
    out.shat_s = Eigen::Map<const Eigen::VectorXd>(sh.data(), static_cast<Eigen::Index>(sh.size()));
    out.q_off.clear();
    for (const auto& [ia, ib, v] : xe.q_off) {
        if (ia < 0 || ib < 0 || ia >= xe.nS() || ib >= xe.nS()) continue;
        const int na = new_of_old[static_cast<std::size_t>(ia)];
        const int nb = new_of_old[static_cast<std::size_t>(ib)];
        if (na >= 0 && nb >= 0) out.q_off.emplace_back(na, nb, v);
    }
    couplingExactFactor(out);
    return out;
}

void networkPbHoldCloneUpdate(NetworkRunContext& ctx,
                              const std::shared_ptr<spdlog::logger>& log,
                              const NetworkEstimator& est, const Eigen::VectorXd& x_s,
                              const EpochWorkspace& ws) {
    (void)log;
    if (!ctx.knobs.emit_pb_hold_smoothed) return;
    auto& clones = ctx.carry.smh_clone;
    const long long ep = static_cast<long long>(ctx.counters.n_loop_epochs);

    // The clone lives while its parent's anchor is engaged (section 9).  A clock card Q3's
    // guard decoupled this epoch (a trip, a gap, a retired frame), or one whose fold this
    // epoch was not the anchored one (LOOSE, or no clock prior folded), kills the clone
    // before the exact route reads the epoch: the LOOSE fold's q is a datum device, and
    // the smoother would pass it as motion.  The key bridges on the phase-clock transfer
    // and the birth is queued again for the first stash at which the anchor holds.  The
    // constellation's pinned clock has no anchor fold and passes nothing: exempt from the
    // second test.
    if (ctx.knobs.cta_on) {
        const ClockAnchorState& an = ctx.carry.clk_anchor;
        for (auto& [key, c] : clones) {
            if (!c.alive) continue;
            PbHoldCloneDeath why = PbHoldCloneDeath::None;
            if (an.tripped.count(c.sat)) {
                why = PbHoldCloneDeath::Withdrawn;
            } else {
                const auto pv = ctx.carry.cxu_pivot.find(c.sat.empty() ? '\0' : c.sat[0]);
                const bool pinned = pv != ctx.carry.cxu_pivot.end() && pv->second.first == c.sat;
                const auto it = an.sat.find(c.sat);
                if (!pinned && (it == an.sat.end() || !it->second.folded_anchored))
                    why = PbHoldCloneDeath::Disengaged;
            }
            if (why == PbHoldCloneDeath::None) continue;
            killClone(ctx, c, why);
            PbHoldClone q;
            q.sat        = c.sat;
            q.signal     = c.signal;
            q.generation = c.generation;
            q.queued_ep  = ep;
            q.rebirth    = true;
            ctx.carry.smh_pending.emplace(key, q);   // a newer generation's entry wins
        }
    }

    bool any = false;
    for (const auto& [key, c] : clones) {
        (void)key;
        if (c.alive) { any = true; break; }
    }
    if (!any) return;

    const CouplingExactEpoch& xe0 = ctx.carry.cxu_epoch;
    const Eigen::Index nx = x_s.size();
    // The final solve's priors: the dense fold's capture (trimmed by card Q3's in-epoch
    // refloat) and the diagonal ones.  Their means are the prior means the solve used,
    // after any in-place frame transform of this epoch.
    std::map<std::string, double> folded, diag;
    for (const auto& sp : ws.cap_dense_keys)
        if (sp.kind != SharedParamKind::VtecSh) folded[sharedCarryKey(sp.kind, sp.sat, sp.signal)] = sp.value;
    for (const auto& sp : ws.cap_priors)
        if (sp.kind != SharedParamKind::VtecSh) diag[sharedCarryKey(sp.kind, sp.sat, sp.signal)] = sp.value;

    PbHoldCloneInputs in;
    in.desc_of_p.assign(static_cast<std::size_t>(std::max<Eigen::Index>(0, xe0.P.rows())), PbHoldCloneTerm{});
    for (const CarryPrior& cp : ctx.carry.prev_carry) {
        if (cp.kind == SharedParamKind::VtecSh) continue;
        const auto pit = xe0.p_of_key.find(sharedCarryKey(cp.kind, cp.sat, cp.signal));
        if (pit == xe0.p_of_key.end() || pit->second < 0
            || static_cast<std::size_t>(pit->second) >= in.desc_of_p.size()) continue;
        PbHoldCloneTerm& d = in.desc_of_p[static_cast<std::size_t>(pit->second)];
        d.kind   = cp.kind;
        d.sat    = cp.sat;
        d.signal = cp.signal;
    }
    // An S key the solve did not fold took no carried prior: the Reinit route for the
    // clones (a withdrawn clock, a constellation skipped at an un-transformed handover).
    std::vector<int> unfolded;
    for (Eigen::Index s = 0; s < xe0.nS(); ++s) {
        const int p = xe0.p_of_s[static_cast<std::size_t>(s)];
        if (p < 0 || static_cast<std::size_t>(p) >= in.desc_of_p.size()) continue;
        if (!folded.count(termKey(in.desc_of_p[static_cast<std::size_t>(p)]))) unfolded.push_back(p);
    }
    CouplingExactEpoch xe_r;
    const CouplingExactEpoch* xe = &xe0;
    if (xe0.ok && !unfolded.empty()) {
        xe_r = pbHoldCloneRestrictEpoch(xe0, unfolded);
        xe   = &xe_r;
        ++ctx.counters.smh_epochs_restricted;
        ctx.counters.smh_keys_unfolded += static_cast<long long>(unfolded.size());
    }
    in.xe = xe;
    const Eigen::Index nS = xe->nS();
    in.post_s.setZero(nS);
    in.prior_s.setZero(nS);
    for (Eigen::Index s = 0; s < nS; ++s) {
        const Eigen::Index col = xe->col_of_s[static_cast<std::size_t>(s)];
        in.post_s(s)  = (col >= 0 && col < nx) ? x_s(col) : xe->shat_s(s);
        in.prior_s(s) = xe->shat_s(s);
        const int p = xe->p_of_s[static_cast<std::size_t>(s)];
        if (p >= 0 && static_cast<std::size_t>(p) < in.desc_of_p.size()) {
            const auto it = folded.find(termKey(in.desc_of_p[static_cast<std::size_t>(p)]));
            if (it != folded.end()) in.prior_s(s) = it->second;
        }
    }
    for (const auto& [cc, pv] : ctx.carry.cxu_pivot) in.pivot_sat[cc] = pv.first;
    in.col_of = [&est](const PbHoldCloneTerm& t) -> Eigen::Index {
        try {
            switch (t.kind) {
                case SharedParamKind::Clock:     return est.clockColumn(t.sat);
                case SharedParamKind::PhaseBias: return est.phaseBiasColumn(t.sat, t.signal);
                case SharedParamKind::CodeBias:  return est.codeBiasColumn(t.sat, t.signal);
                default:                         return -1;
            }
        } catch (...) {}
        return -1;
    };
    in.post_of_col = [&x_s, nx](Eigen::Index col) {
        return (col >= 0 && col < nx) ? x_s(col) : std::numeric_limits<double>::quiet_NaN();
    };
    // A key outside the dense block is folded diagonally, or not at all (then NaN, and the
    // term is marginalized).
    in.prior_of = [&diag](const PbHoldCloneTerm& t) {
        const auto it = diag.find(termKey(t));
        return it != diag.end() ? it->second : std::numeric_limits<double>::quiet_NaN();
    };

    const Eigen::MatrixXd& Sig = est.sharedCovariance();
    const Eigen::Index     ns  = Sig.rows();
    for (auto& [key, c] : clones) {
        (void)key;
        if (!c.alive) continue;
        const PbHoldCloneStep st = pbHoldCloneAdvance(in, c);
        if (!st.ok) {
            killClone(ctx, c, st.death);
            continue;
        }
        // A row that had dense terms and lost every one of them carries nothing: the
        // clone is unlinked (a clone born on the pinned clock has no dense term at all).
        if (st.n_s + st.n_d > 0) {
            double mx = 0.0;
            for (const PbHoldCloneTerm& t : st.terms)
                if (t.p_prev > 0.0) mx = std::max(mx, std::abs(t.coeff));
            if (!(mx > 1e-3)) {
                killClone(ctx, c, PbHoldCloneDeath::Unlinked);
                continue;
            }
        }
        // The frame-free passed motion (section 10): the parent's step against the prior
        // mean the solve folded (a declared frame step moved that mean in place, so it is
        // not a step) minus the revision of x0.  A parent with no folded prior cannot be
        // measured; the engagement test above leaves only the unanchored case, so this is
        // the parent outside the fold.
        const Eigen::Index pcol = in.col_of(PbHoldCloneTerm{SharedParamKind::Clock, c.sat, "", 0.0, 0.0, 0.0, -1});
        const std::string pkey  = sharedCarryKey(SharedParamKind::Clock, c.sat, "");
        double prior_p = std::numeric_limits<double>::quiet_NaN();
        if (const auto f = folded.find(pkey); f != folded.end()) prior_p = f->second;
        else if (const auto dg = diag.find(pkey); dg != diag.end()) prior_p = dg->second;
        const bool pinned_parent = in.pivot_sat.count(c.sat.empty() ? '\0' : c.sat[0])
                                   && in.pivot_sat.at(c.sat[0]) == c.sat;
        double step_p = 0.0;
        if (!pinned_parent) {
            if (!(pcol >= 0 && pcol < nx) || !std::isfinite(prior_p)) {
                killClone(ctx, c, PbHoldCloneDeath::ParentOutside);
                continue;
            }
            step_p = x_s(pcol) - prior_p;
        }
        c.phi       += step_p - (st.value - c.value);
        c.phi_ok     = true;
        c.value      = st.value;
        c.var        = st.var;
        c.terms      = st.terms;
        c.updated_ep = ep;
        ++ctx.counters.smh_updates;
        ctx.counters.smh_terms_s       += st.n_s;
        ctx.counters.smh_terms_d       += st.n_d;
        ctx.counters.smh_terms_outside += st.n_outside;
        if (st.deficit_orphan) ++ctx.counters.smh_deficit_orphan;
        if (st.parent_in_s) c.W += st.dW;
        else                c.w_valid = false;
        // The instruments: Phi = c(t|t) - x0(t|t), and the posterior variance of the
        // increment c(t) - c(t0) = (e_c + m)' x + const, sigma2 + g' Sigma g.
        const Eigen::Index pc = pcol;
        std::map<Eigen::Index, double> g;
        if (pc >= 0 && pc < nx) g[pc] += 1.0;
        for (const PbHoldCloneTerm& t : c.terms)
            if (t.col >= 0 && t.p_prev > 0.0) g[t.col] += t.coeff;
        double q = 0.0;
        for (const auto& [a, ga] : g) {
            if (a >= ns) continue;
            for (const auto& [b, gb] : g) {
                if (b >= ns) continue;
                q += ga * gb * Sig(a, b);
            }
        }
        c.vpost = c.var + q;
    }
}

void networkPbHoldCloneHarvest(NetworkRunContext& ctx, const NetworkEstimator& est,
                               const Eigen::VectorXd& x_s, double now_s) {
    if (!ctx.knobs.emit_pb_hold_smoothed) return;
    auto& clones  = ctx.carry.smh_clone;
    auto& pending = ctx.carry.smh_pending;
    if (clones.empty() && pending.empty()) return;

    const bool stash_now = ctx.carry.cxu_stash_valid
                           && std::abs(ctx.carry.cxu_stash_s - now_s) <= 1e-6;
    const SharedCarryCov& pcc = ctx.carry.prev_carry_cov;
    std::map<std::string, std::pair<double, double>> stash;   // dense keys of the new stash
    if (stash_now) {
        for (const CarryPrior& cp : ctx.carry.prev_carry) {
            if (cp.kind == SharedParamKind::VtecSh) continue;
            const std::string key = sharedCarryKey(cp.kind, cp.sat, cp.signal);
            const auto rit = pcc.row.find(key);
            if (rit == pcc.row.end() || static_cast<Eigen::Index>(rit->second) >= pcc.P.rows()) continue;
            const Eigen::Index r = static_cast<Eigen::Index>(rit->second);
            stash[key] = {cp.value, pcc.P(r, r)};
        }
    }
    std::map<std::string, double> pinned;   // the constellations' pinned clocks now
    for (const auto& [cc, pv] : ctx.carry.cxu_pivot) {
        (void)cc;
        pinned[sharedCarryKey(SharedParamKind::Clock, pv.first, "")] = pv.second;
    }
    const Eigen::MatrixXd& Sig = est.sharedCovariance();
    const Eigen::Index nx = x_s.size();
    const auto fallback = [&](Eigen::Index col) -> std::pair<double, double> {
        const double v   = (col >= 0 && col < nx) ? x_s(col) : 0.0;
        const double var = (col >= 0 && col < Sig.rows()) ? Sig(col, col) : 0.0;
        return {v, var};
    };
    const long long ep = static_cast<long long>(ctx.counters.n_loop_epochs);

    for (auto& [key, c] : clones) {
        (void)key;
        if (!c.alive) continue;
        if (!stash_now) {
            killClone(ctx, c, PbHoldCloneDeath::NoStash);
            continue;
        }
        if (c.updated_ep != ep) {
            killClone(ctx, c, PbHoldCloneDeath::Missed);
            continue;
        }
        c.terms   = pbHoldCloneRebase(c.terms, stash, pinned, fallback);
        c.stamp_s = now_s;
    }

    for (auto pit_pending = pending.begin(); pit_pending != pending.end();) {
        const std::string& key = pit_pending->first;
        PbHoldClone c = pit_pending->second;
        // A generation that has ended takes its pending birth with it.
        const auto hit = ctx.carry.pb_hold.find(key);
        if (hit == ctx.carry.pb_hold.end() || hit->second.generation != c.generation) {
            ++ctx.counters.smh_pending_dropped;
            pit_pending = pending.erase(pit_pending);
            continue;
        }
        const std::string parent = sharedCarryKey(SharedParamKind::Clock, c.sat, "");
        PbHoldCloneTerm t;
        t.kind  = SharedParamKind::Clock;
        t.sat   = c.sat;
        t.coeff = -1.0;                // x0 = v - (-1)(c - s_prev) = c
        const auto sit = stash.find(parent);
        const auto pit = pinned.find(parent);
        // Section 9: born only while card Q3's anchor holds the clock after this epoch's
        // guard, so every fold the clone passes is an anchored one.
        bool engaged = true;
        if (ctx.knobs.cta_on) {
            const auto ait = ctx.carry.clk_anchor.sat.find(c.sat);
            engaged = ait != ctx.carry.clk_anchor.sat.end() && ait->second.anchored
                      && !ctx.carry.clk_anchor.tripped.count(c.sat);
        }
        if (stash_now && pit != pinned.end()) {
            t.s_prev = pit->second;    // the pinned clock: exactly known, a transfer of zero
            t.p_prev = 0.0;
            ++ctx.counters.smh_births_pinned;
        } else if (stash_now && sit != stash.end() && engaged) {
            t.s_prev = sit->second.first;
            t.p_prev = sit->second.second;
            ++ctx.counters.smh_births;
        } else {
            // Waits for a later stash; the key carries the phase-clock transfer meanwhile.
            if (stash_now && sit != stash.end()) ++ctx.counters.smh_defer_loose;
            else                                 ++ctx.counters.smh_defer_nostash;
            ++pit_pending;
            continue;
        }
        if (c.queued_ep >= 0 && ep > c.queued_ep) {
            ++ctx.counters.smh_births_deferred;
            ctx.counters.smh_defer_ep.push_back(static_cast<double>(ep - c.queued_ep));
        }
        if (c.rebirth) ++ctx.counters.smh_rebirths;
        try { t.col = est.clockColumn(c.sat); } catch (...) { t.col = -1; }
        c.terms      = {t};
        c.value      = t.s_prev;
        c.c_ref      = t.s_prev;
        c.var        = 0.0;
        c.t0_s       = now_s;
        c.stamp_s    = now_s;
        c.alive      = true;
        c.death      = PbHoldCloneDeath::None;
        c.updated_ep = ep;
        c.W          = 0.0;
        c.w_valid    = true;
        // The stated phase-visible variance of the key at birth and the clock's share of
        // it: the inputs of G3's predicted hidden fraction f rho_D (section 5).
        {
            const auto rc = pcc.row.find(parent);
            const auto rp = pcc.row.find(sharedCarryKey(SharedParamKind::PhaseBias, c.sat, c.signal));
            const Eigen::Index n = pcc.P.rows();
            if (t.p_prev > 0.0 && rc != pcc.row.end() && rp != pcc.row.end()
                && static_cast<Eigen::Index>(rc->second) < n && static_cast<Eigen::Index>(rp->second) < n) {
                const Eigen::Index a = static_cast<Eigen::Index>(rc->second);
                const Eigen::Index b = static_cast<Eigen::Index>(rp->second);
                const double pd = pcc.P(a, a) + pcc.P(b, b) - 2.0 * pcc.P(a, b);
                c.pd0 = pd;
                if (pd > 0.0) c.f0 = (pcc.P(a, a) - pcc.P(a, b)) / pd;
            }
        }
        clones[key]  = c;
        pit_pending  = pending.erase(pit_pending);
    }
}

}  // namespace GnssProcessor
