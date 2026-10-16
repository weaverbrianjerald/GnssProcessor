/**
 * @file PbHoldClone.cpp
 * @brief The smoothed phase-bias hold's clock clone (card SMH): the pure advance and
 *        re-harvest of a clone row, and the two seams that run them each epoch.
 *
 * Seam contract: free functions over the run context that own no state; every clone
 * lives in @c ctx.carry.smh_clone.
 */

#include "network/emit/PbHoldClone.hpp"

#include "network/state/RunContext.hpp"
#include "common/GnssIdentity.hpp"        // systemOfSat, letterOfSat, SignalId, nominalWavelengthM
#include "common/SignalBand.hpp"          // FrequencyBand (card SMD)
#include "filter/StateKeySchema.hpp"      // StateKeys::makeAmbiguityKey
#include "network/solve/ClockAnchor.hpp"  // clockAnchorPhaseClockWeights (card SMD)
#include "network/driver/RunTypes.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include "network/driver/Epoch.hpp"   // NetworkEpoch (card NOM3 step EP1)
#include "network/driver/Run.hpp"     // NetworkRun (card NOM3 step EP1)
#include "network/driver/SetupResult.hpp"
#include "rinex/MultiRinexSync.hpp"

namespace GnssProcessor {

using SG = net::SatSignalState;   // the clones live in the satellite-signal table (card NOM step N4d)

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
    net::emplaceSig(ctx.entities, key, &SG::smh_block_pending, q);
}

/// Card SMH section 9's engagement test for a clone of either map: @c Withdrawn when card
/// Q3's guard decoupled the parent clock this epoch, @c Disengaged when its fold was not the
/// anchored one (the constellation's pinned clock exempt), otherwise @c None.
PbHoldCloneDeath engagementDeath(const NetworkRunContext& ctx, const PbHoldClone& c) {
    const ClockAnchorState& an = ctx.carry.clk_anchor;
    if (an.tripped.count(c.sat)) return PbHoldCloneDeath::Withdrawn;
    const std::pair<std::string, double>* pv =
        c.sat.empty() ? nullptr
                      : net::get(ctx.entities.constellations, systemOfSat(c.sat),
                                 &net::ConstellationState::cxu_pivot);
    const bool pinned = pv != nullptr && pv->first == c.sat;
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
        const char   cc     = tm.sat.empty() ? '\0' : letterOfSat(tm.sat);
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
    // of distinct S columns only; its diagonal is in q_s.  Card SCL: on the clone epoch a
    // cloned clock's walk is the walk factor's, not a Q of the rows' epoch, so Q is read
    // from card CTU's epoch over the same S index space (W stays the modelled walk since
    // birth).
    const CouplingExactEpoch* xw =
        (in.xe_walk && in.xe_walk->nS() == xe->nS()) ? in.xe_walk : xe;
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
            dW += own[i].g * own[i].g * xw->q_s(own_s[i]);
        if (own.size() > 1) {
            for (const auto& [ia, ib, v] : xw->q_off) {
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
    std::map<FrequencyBand, std::pair<std::string, double>> best;   // band -> (signal, stashed variance)
    const System sys = systemOfSat(sat);
    for (const auto& [sig, v] : pb_var) {
        if (!(v > 0.0) || !std::isfinite(v)) continue;
        const std::optional<SignalId> signal = SignalId::fromObsCode(sys, sig);
        const FrequencyBand band = signal ? signal->band() : FrequencyBand::Unknown;
        if (!(nominalWavelengthM(band) > 0.0)) continue;
        const auto it = best.find(band);
        if (it == best.end() || v < it->second.second) best[band] = {sig, v};
    }
    if (best.size() < 2) return out;
    std::vector<double> alpha;
    for (const auto& [band, sv] : best) {
        out.sig.push_back(sv.first);
        const double lam = nominalWavelengthM(band);
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

CouplingExactEpoch pbHoldCloneOnClones(const CouplingExactEpoch& xe,
                                       const std::map<int, Eigen::Index>& clone_of_p) {
    CouplingExactEpoch out = xe;
    if (!xe.ok || clone_of_p.empty()) return out;
    const Eigen::Index nS = xe.nS();
    out.clone_s.assign(static_cast<std::size_t>(nS), 0);
    bool any = false;
    for (Eigen::Index s = 0; s < nS; ++s) {
        const auto it = clone_of_p.find(xe.p_of_s[static_cast<std::size_t>(s)]);
        if (it == clone_of_p.end() || it->second < 0) continue;
        out.col_of_s[static_cast<std::size_t>(s)] = it->second;
        out.q_s(s) = 0.0;
        out.clone_s[static_cast<std::size_t>(s)] = 1;
        any = true;
    }
    if (!any) {
        out.clone_s.clear();
        return out;
    }
    // A pair touching a moved key would put a walk on a clone (the seam's level noise; a
    // seam satellite is never cloned, so this is a guard).
    out.q_off.erase(std::remove_if(out.q_off.begin(), out.q_off.end(),
                                   [&](const std::tuple<int, int, double>& t) {
                                       const int a = std::get<0>(t), b = std::get<1>(t);
                                       return (a >= 0 && a < nS && out.clone_s[static_cast<std::size_t>(a)])
                                           || (b >= 0 && b < nS && out.clone_s[static_cast<std::size_t>(b)]);
                                   }),
                    out.q_off.end());
    couplingExactFactor(out);
    return out;
}

PbHoldCloneMarg pbHoldCloneMarginalizeClones(
    std::vector<PbHoldCloneTerm>& terms, double& var, const Eigen::MatrixXd& K,
    const Eigen::MatrixXd& B, Eigen::Index n0,
    const std::function<bool(Eigen::Index, PbHoldCloneTerm&)>& term_of_col,
    const std::map<char, PbHoldCloneTerm>& pin, Eigen::VectorXd* defer_rc) {
    PbHoldCloneMarg out;
    const Eigen::Index nc = B.rows();
    const auto is_clone = [&](Eigen::Index col) { return nc > 0 && col >= n0 && col < n0 + nc; };
    bool has_clone = false;
    for (const PbHoldCloneTerm& t : terms)
        if (is_clone(t.col)) { has_clone = true; break; }
    if (!has_clone) return out;
    if (n0 < 0 || B.cols() != nc || K.rows() != nc || K.cols() != n0 + nc || !term_of_col) {
        out.ok = false;
        return out;
    }
    // The row's clone coefficients, and the clock-family sum they hold per constellation.
    Eigen::VectorXd rc = Eigen::VectorXd::Zero(nc);
    std::map<char, double> deficit;
    std::vector<PbHoldCloneTerm> rest;
    rest.reserve(terms.size());
    for (const PbHoldCloneTerm& t : terms) {
        if (is_clone(t.col)) {
            rc(t.col - n0) += t.coeff;
            if (t.kind == SharedParamKind::Clock && !t.sat.empty())
                deficit[letterOfSat(t.sat)] += t.coeff;
            ++out.n_clone;
            continue;
        }
        rest.push_back(t);
    }
    // E[c | s'] = c_hat + K (s' - s_hat'): the clone terms re-attributed onto the border.
    const Eigen::VectorXd moved = K.transpose() * rc;   // zero on the clone and pinned columns
    std::map<std::string, std::size_t> at;              // a free term's key -> its index in rest
    for (std::size_t i = 0; i < rest.size(); ++i)
        if (rest[i].p_prev > 0.0 && rest[i].col >= 0) at[termKey(rest[i])] = i;
    for (Eigen::Index j = 0; j < n0; ++j) {
        const double m = moved(j);
        if (m == 0.0) continue;
        PbHoldCloneTerm t;
        if (!term_of_col(j, t)) {
            ++out.n_dropped;
            out.mass_dropped += std::abs(m);
            continue;
        }
        if (t.kind == SharedParamKind::Clock && !t.sat.empty())
            deficit[letterOfSat(t.sat)] -= m;
        const std::string key = termKey(t);
        const auto it = at.find(key);
        if (it != at.end() && rest[it->second].col == j) {
            rest[it->second].coeff += m;
            continue;
        }
        t.coeff = m;
        t.col   = j;
        at[key] = rest.size();
        rest.push_back(t);
        ++out.n_moved;
    }
    // The clock-family deficit onto the constellation's pinned clock (exactly known: no
    // revision, no variance), the harvest's gauge restore.
    for (const auto& [cc, d] : deficit) {
        if (d == 0.0) continue;
        const auto pv = pin.find(cc);
        if (pv == pin.end()) {
            ++out.n_orphan;
            continue;
        }
        addPinnedClock(rest, pv->second.sat, d, pv->second.s_prev, pv->second.col);
        ++out.n_held;
    }
    const double add = rc.dot(B * rc);
    if (defer_rc) {
        // Card SCL2 X2b: the shared term stays with the row's Y coefficients, projected onto
        // the history at the harvest's end; the per-row term is only reported.
        *defer_rc = rc;
        if (std::isfinite(add) && add > 0.0) out.var_add = add;
    } else if (std::isfinite(add) && add > 0.0) {
        var += add;
        out.var_add = add;
    }
    terms = std::move(rest);
    return out;
}

PbHoldCloneHistAttach pbHoldCloneAttachHistory(const SharedHistoryState& shx, PbHoldCloneShx& sh,
                                               const Eigen::VectorXd& x_s,
                                               std::vector<PbHoldCloneTerm>& terms,
                                               double& value, double& var) {
    PbHoldCloneHistAttach out;
    if (sh.coef.empty()) return out;
    const Eigen::Index nx = x_s.size();
    // The registered columns are this compression's only when the stamps agree (the
    // coupling rows' test, sharedHistoryRowTerms); any other coefficient is marginalized.
    const bool gen = shx.on && std::abs(shx.stash_s - sh.stash_s) <= kSharedHistoryStampTol;
    for (std::size_t j = 0; j < sh.coef.size(); ++j) {
        const double h = sh.coef[j];
        if (h == 0.0) continue;
        const Eigen::Index col = (gen && j < shx.col.size()) ? shx.col[j] : Eigen::Index{-1};
        if (col >= 0 && col < nx && std::isfinite(x_s(col))) {
            // A constant of the epoch with the prior N(0, 1): no time update; the revision is
            // its posterior against the prior mean 0.
            PbHoldCloneTerm t;
            t.kind   = SharedParamKind::Clock;   // empty satellite: no clock family (no deficit)
            t.coeff  = h;
            t.s_prev = 0.0;
            t.p_prev = 1.0;
            t.col    = col;
            terms.push_back(t);
            value -= h * x_s(col);
            ++out.n_terms;
        } else {
            var += h * h;
            out.marg_var += h * h;
            ++out.n_marg;
        }
    }
    sh.coef.clear();
    return out;
}

void networkPbHoldCloneUpdate(NetworkRun& run, const NetworkEpoch& nep) {
    // ---- From the run and the epoch (card NOM3 step EP2; generated by
    //      scripts/nom_ep2_adapters.py: the explicit form's parameters, each bound to the
    //      argument the retired adapter forwarded). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    NetworkRunContext& ctx = run.ctx;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    const NetworkEstimator& est = run.engines.est;
    const Eigen::VectorXd& x_s = nep.solve.x_s;
    const EpochWorkspace& ws = nep.ws;
    const NetworkEstimator::Datum& datum = nep.solve.plan.shared_datum;
    const std::set<std::pair<std::string, std::string>>& eq34_reref_ev = nep.events.phase_datum.eq34_reref;
    // ---- End of the bindings ----
    (void)log;
    if (!ctx.knobs.emit_pb_hold_smoothed) return;
    net::SatSignalTable& sigs = ctx.entities.sat_signals;   // the clones (card NOM step N4d)
    const long long ep = static_cast<long long>(ctx.counters.n_loop_epochs);
    // Card SCL2 X2b (amendment A1): a row still pending from an earlier epoch never reached
    // its compression; it takes the per-row term before it advances (no-op off).
    const bool shx_on = ctx.knobs.carry_cpl_shx;
    if (shx_on) networkSharedHistoryFinalizeHoldRows(ctx, ep - 1);

    // The clone lives while its parent's anchor is engaged (section 9).  A clock card Q3's
    // guard decoupled this epoch (a trip, a gap, a retired frame), or one whose fold this
    // epoch was not the anchored one (LOOSE, or no clock prior folded), kills the clone
    // before the exact route reads the epoch: the LOOSE fold's q is a datum device, and
    // the smoother would pass it as motion.  The key bridges on the phase-clock transfer
    // and the birth is queued again for the first stash at which the anchor holds.  The
    // constellation's pinned clock has no anchor fold and passes nothing: exempt from the
    // second test.
    // Card SMD's block clones: SatSignalState::smh_block.
    if (ctx.knobs.cta_on) {
        net::forEach(sigs, &SG::smh_clone, [&](const SatSignalId& id, PbHoldClone& c) {
            const std::string key = net::holdKey(id);
            if (!c.alive) return;
            const PbHoldCloneDeath why = engagementDeath(ctx, c);
            if (why == PbHoldCloneDeath::None) return;
            killClone(ctx, c, why);
            PbHoldClone q;
            q.sat        = c.sat;
            q.signal     = c.signal;
            q.generation = c.generation;
            q.queued_ep  = ep;
            q.rebirth    = true;
            net::emplaceSig(ctx.entities, key, &SG::smh_pending, q);   // a newer generation's entry wins
        });
        net::forEach(sigs, &SG::smh_block, [&](const SatSignalId& id, PbHoldClone& c) {
            const std::string key = net::holdKey(id);
            if (!c.alive) return;
            const PbHoldCloneDeath why = engagementDeath(ctx, c);
            if (why == PbHoldCloneDeath::None) return;
            killBlock(ctx, c, why, ep);
            queueBlockBirth(ctx, key, c, ep);
        });
    }

    // Card SMD section 3: a block clone follows the phase datum's declared steps of its pair
    // by retiring at them, before the exact route reads the epoch.  A re-reference withholds
    // the key's prior (no frame-free step exists); a new anchor tenure without one is a
    // continuous handover, whose pin at the successor's ledger value steps the family by an
    // undeclared amount.  The key bridges on the phase-clock transfer and the birth is queued
    // for this epoch's stash, on the new frame.  (The emitter's marks are read at the hold.)
    {
        const auto& pa_reref   = ctx.carry.phase_anchor_reref;
        const auto& eq34_reref = eq34_reref_ev;
        net::forEach(sigs, &SG::smh_block, [&](const SatSignalId& id, PbHoldClone& c) {
            const std::string key = net::holdKey(id);
            if (!c.alive) return;
            PbHoldFollow f = PbHoldFollow::None;
            for (const std::string& sg : c.pv_pair.sig) {
                if (pa_reref.count({c.sat, sg}) || eq34_reref.count({c.sat, sg})) {
                    f = PbHoldFollow::Reref;
                    break;
                }
            }
            if (f == PbHoldFollow::None) {
                for (const std::string& sg : c.pv_pair.sig) {
                    const PhaseBasisEntry* bit = net::getSig(
                        sigs, StateKeys::makeAmbiguityKey(c.sat, sg), &SG::phase_basis);
                    if (bit && bit->elected_ep == ep) {
                        f = PbHoldFollow::Tenure;
                        break;
                    }
                }
            }
            if (f == PbHoldFollow::None) return;
            c.follow = f;
            ++ctx.counters.smh_pv_follow[static_cast<int>(f)];
            killBlock(ctx, c, PbHoldCloneDeath::Followed, ep);
            queueBlockBirth(ctx, key, c, ep);
        });
    }

    bool any = false;
    net::forEach(sigs, &SG::smh_clone, [&](const SatSignalId&, const PbHoldClone& c) { any = any || c.alive; });
    net::forEach(sigs, &SG::smh_block, [&](const SatSignalId&, const PbHoldClone& c) { any = any || c.alive; });
    if (!any) return;

    const CouplingExactEpoch& xe0 = ctx.carry.cxu_epoch;
    const Eigen::Index nx = x_s.size();
    // The final solve's priors: the dense fold's capture (trimmed by card Q3's in-epoch
    // refloat) and the diagonal ones.  Their means are the prior means the solve used,
    // after any in-place frame transform of this epoch.
    std::map<std::string, double> folded, diag;
    // A clock clone of card SCL is not its clock: skipped (the clone fold keeps its joint
    // prior in a capture of its own, so none reaches these two; the test is a guard).
    for (const auto& sp : ws.cap_dense_keys)
        if (sp.kind != SharedParamKind::VtecSh && !sp.clone) folded[sharedCarryKey(sp.kind, sp.sat, sp.signal)] = sp.value;
    for (const auto& sp : ws.cap_priors)
        if (sp.kind != SharedParamKind::VtecSh && !sp.clone) diag[sharedCarryKey(sp.kind, sp.sat, sp.signal)] = sp.value;

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
    // Card SCL: the clocks whose clones stood in at this epoch's fold (P row -> clone
    // column).  Empty off, and on an epoch with no stand-in (no clone epoch, a fold
    // fallback), which leaves every step below as before.
    std::map<int, Eigen::Index> clone_of_p;
    if (ctx.knobs.carry_cpl_clone && xe0.ok) {
        const ClockCloneEpoch& E = ctx.carry.ccl;
        for (const std::string& sat : E.standin) {
            const auto a = E.active.find(sat);
            if (a == E.active.end() || a->second.p < 0 || a->second.col < 0) continue;
            clone_of_p[a->second.p] = a->second.col;
        }
        // Card BCL-a: the bias clones that stood in with a walk, the same way (empty off).
        for (const std::string& key : E.xstandin) {
            const auto a = E.xactive.find(key);
            if (a == E.xactive.end() || a->second.p < 0 || a->second.col < 0) continue;
            clone_of_p[a->second.p] = a->second.col;
        }
    }
    // An S key the solve did not fold took no carried prior: the Reinit route for the
    // clones (a withdrawn clock, a constellation skipped at an un-transformed handover).
    // Card SCL: a clock whose clone stood in keeps it (card Q3's refloat withdraws the walk
    // factor and keeps the clone with its stand-in prior), so its rows' terms stay on it.
    std::vector<int> unfolded;
    long long kept_unfolded = 0;
    for (Eigen::Index s = 0; s < xe0.nS(); ++s) {
        const int p = xe0.p_of_s[static_cast<std::size_t>(s)];
        if (p < 0 || static_cast<std::size_t>(p) >= in.desc_of_p.size()) continue;
        if (!folded.count(termKey(in.desc_of_p[static_cast<std::size_t>(p)]))) {
            if (clone_of_p.count(p)) {
                ++kept_unfolded;
                continue;
            }
            unfolded.push_back(p);
        }
    }
    CouplingExactEpoch xe_r;
    const CouplingExactEpoch* xe = &xe0;
    if (xe0.ok && !unfolded.empty()) {
        xe_r = pbHoldCloneRestrictEpoch(xe0, unfolded);
        xe   = &xe_r;
        ++ctx.counters.smh_epochs_restricted;
        ctx.counters.smh_keys_unfolded += static_cast<long long>(unfolded.size());
    }
    // Card SCL: the rows advance on the clone epoch (the stood-in clocks on their clones at
    // Q = 0), and the walk share reads the epoch they were moved from.
    CouplingExactEpoch xe_c;
    if (!clone_of_p.empty()) {
        ClockCloneCounters& CC = ctx.counters.ccl;
        xe_c = pbHoldCloneOnClones(*xe, clone_of_p);
        in.xe_walk = xe;
        xe = &xe_c;
        ++CC.smh_epochs;
        CC.smh_moved += static_cast<long long>(clone_of_p.size());
        if (clone_of_p.size() < ctx.carry.ccl.active.size()) ++CC.smh_partial;
        CC.smh_kept_unfolded += kept_unfolded;
        if (!xe_c.ok) ++CC.smh_unfactored;
    }
    in.xe = xe;
    const Eigen::Index nS = xe->nS();
    in.post_s.setZero(nS);
    in.prior_s.setZero(nS);
    for (Eigen::Index s = 0; s < nS; ++s) {
        const Eigen::Index col = xe->col_of_s[static_cast<std::size_t>(s)];
        in.post_s(s)  = (col >= 0 && col < nx) ? x_s(col) : xe->shat_s(s);
        in.prior_s(s) = xe->shat_s(s);
        // Card SCL: a clone's prior mean is the stashed value the fold centred it on (the
        // rows' shat), not its clock's fold-time mean, which the walk factor carries.
        if (static_cast<std::size_t>(s) < xe->clone_s.size() && xe->clone_s[static_cast<std::size_t>(s)])
            continue;
        const int p = xe->p_of_s[static_cast<std::size_t>(s)];
        if (p >= 0 && static_cast<std::size_t>(p) < in.desc_of_p.size()) {
            const auto it = folded.find(termKey(in.desc_of_p[static_cast<std::size_t>(p)]));
            if (it != folded.end()) in.prior_s(s) = it->second;
        }
    }
    net::forEach(ctx.entities.constellations, &net::ConstellationState::cxu_pivot,
                 [&](System sys, const std::pair<std::string, double>& pv) {
                     in.pivot_sat[rinexLetter(sys)] = pv.first;
                 });
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

    // Card SCL: a clone lives one epoch, so an advanced row's terms on the clones are
    // marginalized through the solve's posterior regression of the clones on the rest of
    // the border (the coupling rows' harvest regression, formed here on the final solve),
    // with the clock-family deficit onto the datum-pinned clock of the constellation.
    Eigen::MatrixXd ccl_K, ccl_B;
    bool ccl_reg = false;
    std::map<char, PbHoldCloneTerm> ccl_pin;
    // Card SCL2 X2: the regression is over Y = the clones and the shared history (equal to the
    // clones off); these rows hold no history term, and r_C' B' r_C over Y's clone block is
    // Var(c | s') with the history marginalized, the correct per-row marginal.
    const Eigen::Index ccl_n0 = est.numShared() - est.numBorderNuisance();
    // Card SCL2 X2b: with history columns registered this epoch the rows carry history terms
    // (pbHoldCloneAttachHistory), which the same regression re-expresses, so it is formed
    // whenever either kind of Y term can be present (the clones alone off).
    const bool ccl_need = !clone_of_p.empty() || (shx_on && ctx.carry.shx.on);
    if (ccl_need) {
        ccl_reg = est.cloneConditional(datum, ccl_K, ccl_B)
               && ccl_K.cols() == est.numShared() && ccl_B.rows() == est.numBorderNuisance();
        ClockCloneEpoch& E = ctx.carry.ccl;
        if (ccl_reg) {
            E.smh_reg = true;
            E.smh_K   = ccl_K;
            E.smh_B   = ccl_B;
        } else {
            ++ctx.counters.ccl.smh_noreg;
        }
        for (const NetworkEstimator::Pin& pn : datum) {
            PbHoldCloneTerm t;
            if (!est.sharedColumnIdentity(pn.column, t.kind, t.sat, t.signal)
                || t.kind != SharedParamKind::Clock || t.sat.empty()) continue;
            t.col    = pn.column;
            t.s_prev = (pn.column >= 0 && pn.column < nx) ? x_s(pn.column) : pn.value;
            t.p_prev = 0.0;
            ccl_pin[letterOfSat(t.sat)] = t;
        }
    }
    const auto ccl_term_of_col = [&](Eigen::Index j, PbHoldCloneTerm& t) {
        if (!est.sharedColumnIdentity(j, t.kind, t.sat, t.signal)) return false;
        t.s_prev = (j >= 0 && j < nx) ? x_s(j) : 0.0;
        const double v = (j >= 0 && j < ns) ? Sig(j, j) : 0.0;
        t.p_prev = v > 0.0 ? v : 1e-12;
        return true;
    };
    // Re-express one advanced row; false (the row is then killed RowFailed) when it has clone
    // terms and the regression is unavailable.  A no-op off (clone_of_p empty).  Card SCL2 X2b:
    // the row's Y coefficients (clones and history) are kept pending in @p sh, projected onto
    // the history at the harvest's end, instead of adding r' B' r to the row now.
    const auto ccl_marg = [&](PbHoldCloneStep& st, PbHoldCloneShx& sh) {
        if (!ccl_need) return true;
        bool has_clone = false;
        for (const PbHoldCloneTerm& t : st.terms)
            if (t.col >= ccl_n0 && t.col < est.numShared()) { has_clone = true; break; }
        if (!has_clone) return true;
        if (!ccl_reg) return false;
        Eigen::VectorXd rcY;
        const PbHoldCloneMarg mg = pbHoldCloneMarginalizeClones(st.terms, st.var, ccl_K, ccl_B,
                                                                ccl_n0, ccl_term_of_col, ccl_pin,
                                                                shx_on ? &rcY : nullptr);
        if (!mg.ok) return false;
        ClockCloneCounters& CC = ctx.counters.ccl;
        ++CC.smh_rows;
        CC.smh_clone_terms  += mg.n_clone;
        CC.smh_moved_terms  += mg.n_moved;
        CC.smh_dropped      += mg.n_dropped;
        CC.smh_mass_dropped += mg.mass_dropped;
        CC.smh_gauge_held   += mg.n_held;
        CC.smh_gauge_orphan += mg.n_orphan;
        if (shx_on) {
            sh.rc     = rcY;
            sh.rc_var = mg.var_add;
            sh.rc_ep  = ep;
            ++ctx.counters.shx.hold_pending;
        } else {
            CC.smh_var_add += mg.var_add;
        }
        return true;
    };
    // Card SCL2 X2b: the row's history terms of the previous compression (no-op off).
    const auto shx_attach = [&](PbHoldCloneStep& st, PbHoldCloneShx& sh) {
        if (!shx_on) return;
        const PbHoldCloneHistAttach a =
            pbHoldCloneAttachHistory(ctx.carry.shx, sh, x_s, st.terms, st.value, st.var);
        SharedHistoryCounters& HC = ctx.counters.shx;
        if (a.n_terms > 0) {
            ++HC.hold_rows_hist;
            HC.hold_terms_hist += a.n_terms;
        }
        if (a.n_marg > 0) {
            ++HC.hold_rows_stale;
            HC.hold_terms_stale += a.n_marg;
            HC.hold_stale_var += a.marg_var;
        }
    };

    net::forEach(sigs, &SG::smh_clone, [&](const SatSignalId&, PbHoldClone& c) {
        if (!c.alive) return;
        PbHoldCloneStep st = pbHoldCloneAdvance(in, c);
        if (!st.ok) {
            killClone(ctx, c, st.death);
            return;
        }
        shx_attach(st, c.shx);
        if (!ccl_marg(st, c.shx)) {
            killClone(ctx, c, PbHoldCloneDeath::RowFailed);
            return;
        }
        // A row that had dense terms and lost every one of them carries nothing: the
        // clone is unlinked (a clone born on the pinned clock has no dense term at all).
        if (st.n_s + st.n_d > 0) {
            double mx = 0.0;
            for (const PbHoldCloneTerm& t : st.terms)
                if (t.p_prev > 0.0) mx = std::max(mx, std::abs(t.coeff));
            if (!(mx > 1e-3)) {
                killClone(ctx, c, PbHoldCloneDeath::Unlinked);
                return;
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
        const auto pv_it = in.pivot_sat.find(c.sat.empty() ? '\0' : letterOfSat(c.sat));
        const bool pinned_parent = pv_it != in.pivot_sat.end() && pv_it->second == c.sat;
        double step_p = 0.0;
        if (!pinned_parent) {
            if (!(pcol >= 0 && pcol < nx) || !std::isfinite(prior_p)) {
                killClone(ctx, c, PbHoldCloneDeath::ParentOutside);
                return;
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
        c.vpost = c.var + c.shx.pendingVar(ep) + q;   // X2b: the pending share is the row's
    });

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
    net::forEach(sigs, &SG::smh_block, [&](const SatSignalId& id, PbHoldClone& c) {
        const std::string key = net::holdKey(id);
        if (!c.alive) return;
        PbHoldCloneStep sc = pbHoldCloneAdvance(in, c);
        if (!sc.ok) {
            killBlock(ctx, c, sc.death, ep);
            return;
        }
        const std::vector<PbHoldCloneOwn> own = pbHoldCloneOwnPv(c.pv_pair);
        PbHoldCloneStep sd = pbHoldCloneAdvanceRow(in, c.sat, own, c.pv_terms, c.pv_value,
                                                   c.pv_var, c.stamp_s);
        if (!sd.ok) {
            if (sd.death == PbHoldCloneDeath::ParentOutside && sd.outside_own > 0) {
                killBlock(ctx, c, PbHoldCloneDeath::PairLost, ep);
                queueBlockBirth(ctx, key, c, ep);
            } else {
                killBlock(ctx, c, sd.death, ep);
            }
            return;
        }
        // Card SCL: both rows re-expressed off the clones (no-op off); card SCL2 X2b: each
        // row's history terms first.
        shx_attach(sc, c.shx);
        shx_attach(sd, c.pv_shx);
        if (!ccl_marg(sc, c.shx) || !ccl_marg(sd, c.pv_shx)) {
            killBlock(ctx, c, PbHoldCloneDeath::RowFailed, ep);
            return;
        }
        if (unlinked(sc) || unlinked(sd)) {
            killBlock(ctx, c, PbHoldCloneDeath::Unlinked, ep);
            return;
        }
        // The frame-free steps (card SMH section 10): the clock's, and each pair phase bias's
        // against the prior mean the solve folded.
        const Eigen::Index pcol = in.col_of(PbHoldCloneTerm{SharedParamKind::Clock, c.sat, "", 0.0, 0.0, 0.0, -1});
        const double prior_c = prior_of_key(sharedCarryKey(SharedParamKind::Clock, c.sat, ""));
        const auto pv_it = in.pivot_sat.find(c.sat.empty() ? '\0' : letterOfSat(c.sat));
        const bool pinned_parent = pv_it != in.pivot_sat.end() && pv_it->second == c.sat;
        double step_c = 0.0;
        if (!pinned_parent) {
            if (!(pcol >= 0 && pcol < nx) || !std::isfinite(prior_c)) {
                killBlock(ctx, c, PbHoldCloneDeath::ParentOutside, ep);
                return;
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
            return;
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
        c.vpost    = c.var + c.shx.pendingVar(ep) + quad(gc);
        c.pv_vpost = c.pv_var + c.pv_shx.pendingVar(ep) + quad(gd);
        // Card REV: the row's stated variance of D(t0), the filtered variance of D now
        // (the functional alone) and the walk this epoch added along D.
        c.pv_vd0 = pbHoldCloneRowValueVar(c.pv_terms, c.pv_var + c.pv_shx.pendingVar(ep), Sig);
        std::map<Eigen::Index, double> gown;
        if (pcol >= 0 && pcol < nx) gown[pcol] += 1.0;
        for (std::size_t j = 0; j < np; ++j) gown[pb_col[j]] -= c.pv_pair.w[j];
        c.pv_pd = quad(gown);
        c.pv_dw = sd.parent_in_s ? sd.dW : 0.0;
    });
}

void networkPbHoldCloneHarvest(NetworkRun& run, const NetworkEpoch& nep) {
    // ---- From the run and the epoch (card NOM3 step EP2; generated by
    //      scripts/nom_ep2_adapters.py: the explicit form's parameters, each bound to the
    //      argument the retired adapter forwarded). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    NetworkRunContext& ctx = run.ctx;
    const NetworkEstimator& est = run.engines.est;
    const Eigen::VectorXd& x_s = nep.solve.x_s;
    double now_s = TimeUtils::continuousGpsSeconds(nep.se.time);
    // ---- End of the bindings ----
    if (!ctx.knobs.emit_pb_hold_smoothed) return;
    // Card SCL2 X2b (amendment A1): the harvest's end, earlier in the epoch, projected every
    // row the update kept pending; a row it did not reach takes the per-row term here, before
    // the re-harvest (no-op off and when every row was projected).
    networkSharedHistoryFinalizeHoldRows(ctx, static_cast<long long>(ctx.counters.n_loop_epochs));
    net::SatSignalTable& sigs = ctx.entities.sat_signals;   // the clones and births (N4d)
    if (net::present(sigs, &SG::smh_clone) == 0 && net::present(sigs, &SG::smh_pending) == 0
        && net::present(sigs, &SG::smh_block) == 0
        && net::present(sigs, &SG::smh_block_pending) == 0) return;

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
    net::forEach(ctx.entities.constellations, &net::ConstellationState::cxu_pivot,
                 [&](System, const std::pair<std::string, double>& pv) {
                     pinned[sharedCarryKey(SharedParamKind::Clock, pv.first, "")] = pv.second;
                 });
    const Eigen::MatrixXd& Sig = est.sharedCovariance();
    const Eigen::Index nx = x_s.size();
    const auto fallback = [&](Eigen::Index col) -> std::pair<double, double> {
        const double v   = (col >= 0 && col < nx) ? x_s(col) : 0.0;
        const double var = (col >= 0 && col < Sig.rows()) ? Sig(col, col) : 0.0;
        return {v, var};
    };
    const long long ep = static_cast<long long>(ctx.counters.n_loop_epochs);

    net::forEach(sigs, &SG::smh_clone, [&](const SatSignalId&, PbHoldClone& c) {
        if (!c.alive) return;
        if (!stash_now) {
            killClone(ctx, c, PbHoldCloneDeath::NoStash);
            return;
        }
        if (c.updated_ep != ep) {
            killClone(ctx, c, PbHoldCloneDeath::Missed);
            return;
        }
        c.terms   = pbHoldCloneRebase(c.terms, stash, pinned, fallback);
        c.stamp_s = now_s;
    });

    net::eraseIf(sigs, &SG::smh_pending, [&](const SatSignalId& id, const PbHoldClone& pend) {
        const std::string key = net::holdKey(id);
        PbHoldClone c = pend;
        // A generation that has ended takes its pending birth with it.
        const net::PbHoldKey* hit = net::getSig(sigs, key, &SG::pb_hold);
        if (!hit || hit->generation != c.generation) {
            ++ctx.counters.smh_pending_dropped;
            return true;
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
            return false;
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
        net::put(sigs, id, &SG::smh_clone) = c;
        return true;
    });

    // Card SMD: the block clones, re-harvested row by row, and born under card SMH's rule
    // when the stash also holds the satellite's phase biases on two bands (the pair, fixed
    // for the clone's life).
    net::forEach(sigs, &SG::smh_block, [&](const SatSignalId& id, PbHoldClone& c) {
        if (!c.alive) return;
        if (!stash_now) {
            killBlock(ctx, c, PbHoldCloneDeath::NoStash, ep);
            return;
        }
        if (c.updated_ep != ep) {
            killBlock(ctx, c, PbHoldCloneDeath::Missed, ep);
            return;
        }
        c.terms    = pbHoldCloneRebase(c.terms, stash, pinned, fallback);
        c.pv_terms = pbHoldCloneRebase(c.pv_terms, stash, pinned, fallback);
        c.stamp_s  = now_s;
    });
    if (net::present(sigs, &SG::smh_block_pending) == 0) return;
    std::map<std::string, std::map<std::string, double>> pb_var_of;   // sat -> signal -> stashed variance
    if (stash_now) {
        for (const CarryPrior& cp : ctx.carry.prev_carry) {
            if (cp.kind != SharedParamKind::PhaseBias) continue;
            const auto sit = stash.find(sharedCarryKey(cp.kind, cp.sat, cp.signal));
            if (sit != stash.end()) pb_var_of[cp.sat][cp.signal] = sit->second.second;
        }
    }
    net::eraseIf(sigs, &SG::smh_block_pending, [&](const SatSignalId& id, const PbHoldClone& pend) {
        const std::string key = net::holdKey(id);
        PbHoldClone c = pend;
        const net::PbHoldKey* hit = net::getSig(sigs, key, &SG::pb_hold);
        if (!hit || hit->generation != c.generation) {
            ++ctx.counters.smh_pv_pending_dropped;
            return true;
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
            return false;
        }
        PbHoldPair pair;
        if (const auto pv = pb_var_of.find(c.sat); pv != pb_var_of.end())
            pair = pbHoldClonePickPair(c.sat, pv->second);
        if (pair.sig.empty()) {
            ++ctx.counters.smh_pv_defer_nopair;
            return false;
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
        net::put(sigs, id, &SG::smh_block) = c;
        return true;
    });
}

void networkPbHoldBlockAtHold(NetworkRunContext& ctx, const std::string& key,
                              unsigned generation, bool pb_marked, double epoch_s,
                              const std::function<double(const std::string&)>& bar) {
    if (!ctx.knobs.emit_pb_hold_smoothed) return;
    PbHoldClone* const cp = net::getSig(ctx.entities.sat_signals, key, &SG::smh_block);
    if (!cp) return;
    PbHoldClone& c = *cp;
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
        auto& acc = ctx.counters.smh_pv_r[{letterOfSat(c.sat), hour}];
        acc.first  += c.pv_phi * c.pv_phi / den;
        acc.second += 1;
    }
}

}  // namespace GnssProcessor
