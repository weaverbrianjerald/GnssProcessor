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
#include "common/SignalBand.hpp"          // the band pair's wavelengths (card SMD)
#include "filter/NetworkClockAnchor.hpp"  // clockAnchorPhaseClockWeights (card SMD)
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

/// Card SMD: a block clone's death (its own counters; the epoch kept for the hold).
void killBlock(NetworkRunContext& ctx, PbHoldClone& c, PbHoldCloneDeath why, long long ep) {
    c.alive   = false;
    c.death   = why;
    c.died_ep = ep;
    ++ctx.counters.smh_pv_deaths[static_cast<int>(why)];
}

/// Card SMD: queue a block clone's birth again within its generation (after a withdrawal, a
/// disengagement, a followed step or a lost pair).  A newer generation's entry wins.
void queueBlockBirth(NetworkRunContext& ctx, const std::string& key, const PbHoldClone& c,
                     long long ep) {
    PbHoldClone q;
    q.sat        = c.sat;
    q.signal     = c.signal;
    q.generation = c.generation;
    q.queued_ep  = ep;
    q.rebirth    = true;
    q.block      = true;
    ctx.carry.smh_block_pending.emplace(key, q);
}

/// Card SMH section 9's engagement test for a clone of either map: @c Withdrawn when card
/// Q3's guard decoupled the parent clock this epoch, @c Disengaged when its fold was not the
/// anchored one (the constellation's pinned clock exempt), otherwise @c None.
PbHoldCloneDeath engagementDeath(const NetworkRunContext& ctx, const PbHoldClone& c) {
    const ClockAnchorState& an = ctx.carry.clk_anchor;
    if (an.tripped.count(c.sat)) return PbHoldCloneDeath::Withdrawn;
    const auto pv = ctx.carry.cxu_pivot.find(c.sat.empty() ? '\0' : c.sat[0]);
    const bool pinned = pv != ctx.carry.cxu_pivot.end() && pv->second.first == c.sat;
    const auto it = an.sat.find(c.sat);
    if (!pinned && (it == an.sat.end() || !it->second.folded_anchored))
        return PbHoldCloneDeath::Disengaged;
    return PbHoldCloneDeath::None;
}

}  // namespace

PbHoldCloneStep pbHoldCloneAdvance(const PbHoldCloneInputs& in, const PbHoldClone& c) {
    const std::vector<PbHoldCloneOwn> clock_alone{PbHoldCloneOwn{}};   // the parent clock, g = 1
    return pbHoldCloneAdvanceRow(in, c.sat, clock_alone, c.terms, c.value, c.var, c.stamp_s);
}

PbHoldCloneStep pbHoldCloneAdvanceRow(const PbHoldCloneInputs& in, const std::string& sat,
                                      const std::vector<PbHoldCloneOwn>& own,
                                      const std::vector<PbHoldCloneTerm>& row_terms,
                                      double value, double var, double stamp_s) {
    PbHoldCloneStep out;
    const CouplingExactEpoch* xe = in.xe;
    if (!xe || !xe->ok) {
        out.death = PbHoldCloneDeath::Unfactored;
        return out;
    }
    if (std::abs(stamp_s - xe->stash_s) > 1e-6) {
        out.death = PbHoldCloneDeath::Stale;
        return out;
    }
    // The row's own keys (the parent clock for card SMH's row; the clock and the band pair
    // for card SMD's phase-visible row).
    std::vector<std::string> own_key(own.size());
    for (std::size_t i = 0; i < own.size(); ++i)
        own_key[i] = sharedCarryKey(own[i].kind, sat, own[i].signal);
    std::vector<std::pair<int, double>> pterms;
    std::vector<PbHoldCloneTerm>        kept;         // pinned and outside-block terms
    std::map<char, double>              out_clk_def;  // clock-family deficit of those terms
    double dep_var = 0.0;                             // marginalized terms
    double tu_var  = 0.0;                             // kappa terms outside the block
    double rev     = 0.0;                             // the border's revision along the row
    for (const PbHoldCloneTerm& tm : row_terms) {
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
        // all but name, which the card forbids: the clone dies (so does a row whose other
        // own key, a phase bias of card SMD's pair, is there).
        for (std::size_t i = 0; i < own_key.size(); ++i) {
            if (key == own_key[i]) {
                out.death       = PbHoldCloneDeath::ParentOutside;
                out.outside_own = static_cast<int>(i);
                return out;
            }
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
    out.value = value - rev;
    out.var   = var + xr.var_add + dep_var + tu_var;
    // The fold's inflation along the functional, g'Qg over the own keys, when every one of
    // them is in S (for the clock alone, its q).  The seam's off-diagonal noise lists pairs
    // of distinct S columns only; its diagonal is in q_s.
    std::vector<int> own_s(own.size(), -1);
    bool all_s = !own.empty();
    for (std::size_t i = 0; i < own.size(); ++i) {
        const auto pp = xe->p_of_key.find(own_key[i]);
        if (pp != xe->p_of_key.end()) own_s[i] = xe->s_of_p[static_cast<std::size_t>(pp->second)];
        if (own_s[i] < 0) all_s = false;
    }
    if (all_s) {
        double dW = 0.0;
        for (std::size_t i = 0; i < own.size(); ++i)
            dW += own[i].g * own[i].g * xe->q_s(own_s[i]);
        if (own.size() > 1) {
            for (const auto& [ia, ib, v] : xe->q_off) {
                if (ia == ib) continue;
                for (std::size_t a = 0; a < own.size(); ++a) {
                    if (own_s[a] != ia) continue;
                    for (std::size_t b = 0; b < own.size(); ++b)
                        if (own_s[b] == ib) dW += own[a].g * own[b].g * v;
                }
            }
        }
        out.parent_in_s = true;
        out.dW          = dW;
    }
    out.ok = true;
    return out;
}

PbHoldPair pbHoldClonePickPair(const std::string& sat, const std::map<std::string, double>& pb_var) {
    PbHoldPair out;
    std::map<int, std::pair<std::string, double>> best;   // band -> (signal, stashed variance)
    for (const auto& [sig, v] : pb_var) {
        if (!(v > 0.0) || !std::isfinite(v)) continue;
        const FrequencyBand band = classifySignalForSat(sat, sig).band;
        if (!(bandNominalWavelengthMeters(band) > 0.0)) continue;
        const int b = static_cast<int>(band);
        const auto it = best.find(b);
        if (it == best.end() || v < it->second.second) best[b] = {sig, v};
    }
    if (best.size() < 2) return out;
    std::vector<double> alpha;
    for (const auto& [b, sv] : best) {
        (void)b;
        out.sig.push_back(sv.first);
        const double lam = bandNominalWavelengthMeters(classifySignalForSat(sat, sv.first).band);
        alpha.push_back(lam * lam);
    }
    out.w = clockAnchorPhaseClockWeights(alpha);
    if (out.w.size() != out.sig.size()) {
        out.sig.clear();
        out.w.clear();
    }
    return out;
}

std::vector<PbHoldCloneOwn> pbHoldCloneOwnPv(const PbHoldPair& pair) {
    std::vector<PbHoldCloneOwn> own;
    own.reserve(pair.sig.size() + 1);
    own.push_back(PbHoldCloneOwn{});   // the clock, g = 1
    for (std::size_t j = 0; j < pair.sig.size() && j < pair.w.size(); ++j)
        own.push_back(PbHoldCloneOwn{SharedParamKind::PhaseBias, pair.sig[j], -pair.w[j]});
    return own;
}

double pbHoldCloneRowValueVar(const std::vector<PbHoldCloneTerm>& terms, double var,
                              const Eigen::MatrixXd& Sig) {
    std::map<Eigen::Index, double> m;
    for (const PbHoldCloneTerm& t : terms)
        if (t.col >= 0 && t.p_prev > 0.0) m[t.col] += t.coeff;
    const Eigen::Index ns = Sig.rows();
    double q = 0.0;
    for (const auto& [a, ma] : m) {
        if (a >= ns) continue;
        for (const auto& [b, mb] : m) {
            if (b >= ns) continue;
            q += ma * mb * Sig(a, b);
        }
    }
    return var + q;
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
    auto& blocks = ctx.carry.smh_block;   // card SMD's block clones
    if (ctx.knobs.cta_on) {
        for (auto& [key, c] : clones) {
            if (!c.alive) continue;
            const PbHoldCloneDeath why = engagementDeath(ctx, c);
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
        for (auto& [key, c] : blocks) {
            if (!c.alive) continue;
            const PbHoldCloneDeath why = engagementDeath(ctx, c);
            if (why == PbHoldCloneDeath::None) continue;
            killBlock(ctx, c, why, ep);
            queueBlockBirth(ctx, key, c, ep);
        }
    }

    // Card SMD section 3: a block clone follows the phase datum's declared steps of its pair
    // by retiring at them, before the exact route reads the epoch.  A re-reference withholds
    // the key's prior (no frame-free step exists); a new anchor tenure without one is a
    // continuous handover, whose pin at the successor's ledger value steps the family by an
    // undeclared amount.  The key bridges on the phase-clock transfer and the birth is queued
    // for this epoch's stash, on the new frame.  (The emitter's marks are read at the hold.)
    {
        const auto& pa_reref   = ctx.carry.phase_anchor_reref;
        const auto& eq34_reref = ctx.carry.eq34_reref;
        const auto& basis      = ctx.carry.phase_basis;
        for (auto& [key, c] : blocks) {
            if (!c.alive) continue;
            PbHoldFollow f = PbHoldFollow::None;
            for (const std::string& sg : c.pv_pair.sig) {
                if (pa_reref.count({c.sat, sg}) || eq34_reref.count({c.sat, sg})) {
                    f = PbHoldFollow::Reref;
                    break;
                }
            }
            if (f == PbHoldFollow::None) {
                for (const std::string& sg : c.pv_pair.sig) {
                    const auto bit = basis.find(c.sat + "_" + sg);
                    if (bit != basis.end() && bit->second.elected_ep == ep) {
                        f = PbHoldFollow::Tenure;
                        break;
                    }
                }
            }
            if (f == PbHoldFollow::None) continue;
            c.follow = f;
            ++ctx.counters.smh_pv_follow[static_cast<int>(f)];
            killBlock(ctx, c, PbHoldCloneDeath::Followed, ep);
            queueBlockBirth(ctx, key, c, ep);
        }
    }

    bool any = false;
    for (const auto& [key, c] : clones) {
        (void)key;
        if (c.alive) { any = true; break; }
    }
    for (const auto& [key, c] : blocks) {
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

    // Card SMD: the block clones, both rows on one lifetime.  The clock row takes card SMH's
    // rule; the phase-visible row takes the same rule over its functional {c: 1, pair: -w_j}.
    // A failure of the clock row is card SMH's structural death; a pair phase bias outside
    // the block, without a column or without a folded prior is PairLost (the birth queued
    // again on the next stash, which may pick another pair).
    const auto prior_of_key = [&folded, &diag](const std::string& k) {
        if (const auto f = folded.find(k); f != folded.end()) return f->second;
        if (const auto dg = diag.find(k); dg != diag.end()) return dg->second;
        return std::numeric_limits<double>::quiet_NaN();
    };
    const auto unlinked = [](const PbHoldCloneStep& st) {
        if (st.n_s + st.n_d == 0) return false;   // a row on the pinned clock alone
        double mx = 0.0;
        for (const PbHoldCloneTerm& t : st.terms)
            if (t.p_prev > 0.0) mx = std::max(mx, std::abs(t.coeff));
        return !(mx > 1e-3);
    };
    const auto quad = [&Sig, ns](const std::map<Eigen::Index, double>& g) {
        double q = 0.0;
        for (const auto& [a, ga] : g) {
            if (a >= ns) continue;
            for (const auto& [b, gb] : g) {
                if (b >= ns) continue;
                q += ga * gb * Sig(a, b);
            }
        }
        return q;
    };
    for (auto& [key, c] : blocks) {
        if (!c.alive) continue;
        const PbHoldCloneStep sc = pbHoldCloneAdvance(in, c);
        if (!sc.ok) {
            killBlock(ctx, c, sc.death, ep);
            continue;
        }
        const std::vector<PbHoldCloneOwn> own = pbHoldCloneOwnPv(c.pv_pair);
        const PbHoldCloneStep sd = pbHoldCloneAdvanceRow(in, c.sat, own, c.pv_terms, c.pv_value,
                                                         c.pv_var, c.stamp_s);
        if (!sd.ok) {
            if (sd.death == PbHoldCloneDeath::ParentOutside && sd.outside_own > 0) {
                killBlock(ctx, c, PbHoldCloneDeath::PairLost, ep);
                queueBlockBirth(ctx, key, c, ep);
            } else {
                killBlock(ctx, c, sd.death, ep);
            }
            continue;
        }
        if (unlinked(sc) || unlinked(sd)) {
            killBlock(ctx, c, PbHoldCloneDeath::Unlinked, ep);
            continue;
        }
        // The frame-free steps (card SMH section 10): the clock's, and each pair phase bias's
        // against the prior mean the solve folded.
        const Eigen::Index pcol = in.col_of(PbHoldCloneTerm{SharedParamKind::Clock, c.sat, "", 0.0, 0.0, 0.0, -1});
        const double prior_c = prior_of_key(sharedCarryKey(SharedParamKind::Clock, c.sat, ""));
        const bool pinned_parent = in.pivot_sat.count(c.sat.empty() ? '\0' : c.sat[0])
                                   && in.pivot_sat.at(c.sat[0]) == c.sat;
        double step_c = 0.0;
        if (!pinned_parent) {
            if (!(pcol >= 0 && pcol < nx) || !std::isfinite(prior_c)) {
                killBlock(ctx, c, PbHoldCloneDeath::ParentOutside, ep);
                continue;
            }
            step_c = x_s(pcol) - prior_c;
        }
        const std::size_t np = c.pv_pair.sig.size();
        std::vector<double>       step_p(np, std::numeric_limits<double>::quiet_NaN());
        std::vector<Eigen::Index> pb_col(np, -1);
        bool lost = false;
        for (std::size_t j = 0; j < np; ++j) {
            const std::string& sg = c.pv_pair.sig[j];
            const Eigen::Index col = in.col_of(PbHoldCloneTerm{SharedParamKind::PhaseBias, c.sat, sg, 0.0, 0.0, 0.0, -1});
            const double pr = prior_of_key(sharedCarryKey(SharedParamKind::PhaseBias, c.sat, sg));
            if (!(col >= 0 && col < nx) || !std::isfinite(pr)) {
                lost = true;
                break;
            }
            pb_col[j] = col;
            step_p[j] = x_s(col) - pr;
        }
        if (lost) {
            killBlock(ctx, c, PbHoldCloneDeath::PairLost, ep);
            queueBlockBirth(ctx, key, c, ep);
            continue;
        }
        double step_d = step_c;
        for (std::size_t j = 0; j < np; ++j) step_d -= c.pv_pair.w[j] * step_p[j];
        // Both rows move: the passed motion is the step less the revision of x0.
        c.phi       += step_c - (sc.value - c.value);
        c.phi_ok     = true;
        c.value      = sc.value;
        c.var        = sc.var;
        c.terms      = sc.terms;
        c.pv_phi    += step_d - (sd.value - c.pv_value);
        c.pv_phi_ok  = true;
        // Card REV: the revision of D(t0) and the step it splits from (Phi_D + R = S_D).
        c.pv_rev    += sd.value - c.pv_value;
        c.pv_sd     += step_d;
        c.pv_value   = sd.value;
        c.pv_var     = sd.var;
        c.pv_terms   = sd.terms;
        // The functional's own step, read at the hold against the emitter's bar: a level
        // update and an ionosphere-direction update cancel in D, a re-realization of a pair
        // phase bias does not (it steps D by w_j times its step).
        c.pv_step_d  = step_d;
        c.updated_ep = ep;
        ++ctx.counters.smh_pv_updates;
        if (sc.parent_in_s) c.W += sc.dW;
        else                c.w_valid = false;
        if (sd.parent_in_s) c.pv_W += sd.dW;
        else                c.pv_w_valid = false;
        // The posterior variances of the two increments, sigma2 + g' Sigma g with g the
        // functional on this epoch's columns plus the row's coefficients.
        std::map<Eigen::Index, double> gc, gd;
        if (pcol >= 0 && pcol < nx) {
            gc[pcol] += 1.0;
            gd[pcol] += 1.0;
        }
        for (std::size_t j = 0; j < np; ++j) gd[pb_col[j]] -= c.pv_pair.w[j];
        for (const PbHoldCloneTerm& t : c.terms)
            if (t.col >= 0 && t.p_prev > 0.0) gc[t.col] += t.coeff;
        for (const PbHoldCloneTerm& t : c.pv_terms)
            if (t.col >= 0 && t.p_prev > 0.0) gd[t.col] += t.coeff;
        c.vpost    = c.var + quad(gc);
        c.pv_vpost = c.pv_var + quad(gd);
        // Card REV: the row's stated variance of D(t0), the filtered variance of D now
        // (the functional alone) and the walk this epoch added along D.
        c.pv_vd0 = pbHoldCloneRowValueVar(c.pv_terms, c.pv_var, Sig);
        std::map<Eigen::Index, double> gown;
        if (pcol >= 0 && pcol < nx) gown[pcol] += 1.0;
        for (std::size_t j = 0; j < np; ++j) gown[pb_col[j]] -= c.pv_pair.w[j];
        c.pv_pd = quad(gown);
        c.pv_dw = sd.parent_in_s ? sd.dW : 0.0;
    }
}

void networkPbHoldCloneHarvest(NetworkRunContext& ctx, const NetworkEstimator& est,
                               const Eigen::VectorXd& x_s, double now_s) {
    if (!ctx.knobs.emit_pb_hold_smoothed) return;
    auto& clones  = ctx.carry.smh_clone;
    auto& pending = ctx.carry.smh_pending;
    if (clones.empty() && pending.empty() && ctx.carry.smh_block.empty()
        && ctx.carry.smh_block_pending.empty()) return;

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

    // Card SMD: the block clones, re-harvested row by row, and born under card SMH's rule
    // when the stash also holds the satellite's phase biases on two bands (the pair, fixed
    // for the clone's life).
    auto& blocks   = ctx.carry.smh_block;
    auto& bpending = ctx.carry.smh_block_pending;
    for (auto& [key, c] : blocks) {
        (void)key;
        if (!c.alive) continue;
        if (!stash_now) {
            killBlock(ctx, c, PbHoldCloneDeath::NoStash, ep);
            continue;
        }
        if (c.updated_ep != ep) {
            killBlock(ctx, c, PbHoldCloneDeath::Missed, ep);
            continue;
        }
        c.terms    = pbHoldCloneRebase(c.terms, stash, pinned, fallback);
        c.pv_terms = pbHoldCloneRebase(c.pv_terms, stash, pinned, fallback);
        c.stamp_s  = now_s;
    }
    if (bpending.empty()) return;
    std::map<std::string, std::map<std::string, double>> pb_var_of;   // sat -> signal -> stashed variance
    if (stash_now) {
        for (const CarryPrior& cp : ctx.carry.prev_carry) {
            if (cp.kind != SharedParamKind::PhaseBias) continue;
            const auto sit = stash.find(sharedCarryKey(cp.kind, cp.sat, cp.signal));
            if (sit != stash.end()) pb_var_of[cp.sat][cp.signal] = sit->second.second;
        }
    }
    for (auto bit = bpending.begin(); bit != bpending.end();) {
        const std::string& key = bit->first;
        PbHoldClone c = bit->second;
        const auto hit = ctx.carry.pb_hold.find(key);
        if (hit == ctx.carry.pb_hold.end() || hit->second.generation != c.generation) {
            ++ctx.counters.smh_pv_pending_dropped;
            bit = bpending.erase(bit);
            continue;
        }
        const std::string parent = sharedCarryKey(SharedParamKind::Clock, c.sat, "");
        PbHoldCloneTerm t;
        t.kind  = SharedParamKind::Clock;
        t.sat   = c.sat;
        t.coeff = -1.0;
        const auto sit = stash.find(parent);
        const auto pit = pinned.find(parent);
        bool engaged = true;
        if (ctx.knobs.cta_on) {
            const auto ait = ctx.carry.clk_anchor.sat.find(c.sat);
            engaged = ait != ctx.carry.clk_anchor.sat.end() && ait->second.anchored
                      && !ctx.carry.clk_anchor.tripped.count(c.sat);
        }
        bool on_pinned = false;
        if (stash_now && pit != pinned.end()) {
            t.s_prev  = pit->second;
            t.p_prev  = 0.0;
            on_pinned = true;
        } else if (stash_now && sit != stash.end() && engaged) {
            t.s_prev = sit->second.first;
            t.p_prev = sit->second.second;
        } else {
            if (stash_now && sit != stash.end()) ++ctx.counters.smh_pv_defer_anchor;
            else                                 ++ctx.counters.smh_pv_defer_nostash;
            ++bit;
            continue;
        }
        PbHoldPair pair;
        if (const auto pv = pb_var_of.find(c.sat); pv != pb_var_of.end())
            pair = pbHoldClonePickPair(c.sat, pv->second);
        if (pair.sig.empty()) {
            ++ctx.counters.smh_pv_defer_nopair;
            ++bit;
            continue;
        }
        if (on_pinned) ++ctx.counters.smh_pv_births_pinned;
        else           ++ctx.counters.smh_pv_births;
        if (c.rebirth) ++ctx.counters.smh_pv_rebirths;
        try { t.col = est.clockColumn(c.sat); } catch (...) { t.col = -1; }
        // The clock row: card SMH's birth.
        c.terms      = {t};
        c.value      = t.s_prev;
        c.c_ref      = t.s_prev;
        c.var        = 0.0;
        c.phi        = 0.0;
        c.phi_ok     = false;
        c.W          = 0.0;
        c.w_valid    = true;
        c.vpost      = 0.0;
        // The phase-visible row: {c: -1, p_j: +w_j}, so x0 = c - sum_j w_j p_j.
        std::string pname;
        c.pv_pair  = pair;
        c.pv_terms = {t};
        double d0  = t.s_prev;
        for (std::size_t j = 0; j < pair.sig.size(); ++j) {
            const auto& sv = stash.at(sharedCarryKey(SharedParamKind::PhaseBias, c.sat, pair.sig[j]));
            PbHoldCloneTerm tp;
            tp.kind   = SharedParamKind::PhaseBias;
            tp.sat    = c.sat;
            tp.signal = pair.sig[j];
            tp.coeff  = pair.w[j];
            tp.s_prev = sv.first;
            tp.p_prev = sv.second;
            try { tp.col = est.phaseBiasColumn(c.sat, pair.sig[j]); } catch (...) { tp.col = -1; }
            c.pv_terms.push_back(tp);
            d0    -= pair.w[j] * sv.first;
            pname += (j ? "+" : "") + pair.sig[j];
        }
        ++ctx.counters.smh_pv_pairs[pname];
        c.pv_value   = d0;
        c.pv_var     = 0.0;
        c.pv_phi     = 0.0;
        c.pv_phi_ok  = false;
        c.pv_W       = 0.0;
        c.pv_w_valid = true;
        c.pv_vpost   = 0.0;
        c.pv_step_d  = 0.0;
        // Card REV: the revision starts at zero; V at birth is the filtered variance of D
        // at the stash (the row's coefficients are the functional, sigma2 = 0).
        c.pv_rev       = 0.0;
        c.pv_sd        = 0.0;
        c.pv_vd0_birth = pbHoldCloneRowValueVar(c.pv_terms, 0.0, est.sharedCovariance());
        c.pv_vd0       = c.pv_vd0_birth;
        c.pv_pd        = c.pv_vd0_birth;
        c.pv_dw        = 0.0;
        c.block      = true;
        c.t0_s       = now_s;
        c.stamp_s    = now_s;
        c.alive      = true;
        c.death      = PbHoldCloneDeath::None;
        c.follow     = PbHoldFollow::None;
        c.died_ep    = -1;
        c.updated_ep = ep;
        blocks[key]  = c;
        bit          = bpending.erase(bit);
    }
}

void networkPbHoldBlockAtHold(NetworkRunContext& ctx, const std::string& key,
                              unsigned generation, bool pb_marked, double epoch_s,
                              const std::function<double(const std::string&)>& bar) {
    if (!ctx.knobs.emit_pb_hold_smoothed) return;
    const auto it = ctx.carry.smh_block.find(key);
    if (it == ctx.carry.smh_block.end()) return;
    PbHoldClone& c = it->second;
    if (c.generation != generation) return;
    const long long ep = static_cast<long long>(ctx.counters.n_loop_epochs);
    if (!c.alive) {
        // A re-reference the producer declared and the emitter did not mark: the hold is
        // what hides that step from the wire.
        if (c.died_ep == ep && c.death == PbHoldCloneDeath::Followed
            && c.follow == PbHoldFollow::Reref && !pb_marked)
            ++ctx.counters.smh_pv_reref_unmarked;
        return;
    }
    if (c.updated_ep != ep) return;
    if (pb_marked) {
        // Card SMD section 3: the emitter's mark on the phase channel is a declared step.
        c.follow = PbHoldFollow::Mark;
        ++ctx.counters.smh_pv_follow[static_cast<int>(PbHoldFollow::Mark)];
        killBlock(ctx, c, PbHoldCloneDeath::Followed, ep);
        queueBlockBirth(ctx, key, c, ep);
        return;
    }
    // A pair step above the emitter's jump bar with nothing declared: passed as motion.
    double b = 0.0;
    for (const std::string& sg : c.pv_pair.sig) {
        const double bj = bar ? bar(sg) : 0.0;
        if (bj > 0.0 && (b == 0.0 || bj < b)) b = bj;
    }
    if (b > 0.0 && std::isfinite(c.pv_step_d) && std::abs(c.pv_step_d) > b) {
        ++ctx.counters.smh_pv_steps_unmarked;
        ctx.counters.smh_pv_step_unmarked_max = std::max(ctx.counters.smh_pv_step_unmarked_max,
                                                         std::abs(c.pv_step_d));
        if (ctx.carry.smh_t_first_s >= 0.0)
            ++ctx.counters.smh_pv_steps_unmarked_hour[static_cast<int>(
                std::floor((epoch_s - ctx.carry.smh_t_first_s) / 3600.0))];
    }
    // The split, on the block's shared birth (card SMD section 4).
    if (c.pv_phi_ok && c.phi_ok) {
        ctx.counters.smh_pv_phi_abs.push_back(std::abs(c.pv_phi));
        ctx.counters.smh_pv_lvl_abs.push_back(std::abs(c.phi - c.pv_phi));
    }
    const double den = c.pv_W - c.pv_vpost;
    if (c.pv_phi_ok && c.pv_w_valid && den > 0.0 && ctx.carry.smh_t_first_s >= 0.0 && !c.sat.empty()) {
        const int hour = static_cast<int>(std::floor((epoch_s - ctx.carry.smh_t_first_s) / 3600.0));
        auto& acc = ctx.counters.smh_pv_r[{c.sat[0], hour}];
        acc.first  += c.pv_phi * c.pv_phi / den;
        acc.second += 1;
    }
}

}  // namespace GnssProcessor
