/**
 * @file NetworkClockClone.cpp
 * @brief The one-epoch clock clone of card SCL (@c network.carry_coupling_clock_clone);
 *        the mechanism, the index spaces and the contract of each step are in the header.
 */

#include "network/carry/NetworkClockClone.hpp"
#include "network/driver/NetworkEpoch.hpp"   // EpochEvents

#include "network/state/NetworkRunContext.hpp"
#include "common/GnssIdentity.hpp"            // letterOfSat, systemOfSat
#include "network/state/NetworkCarryDisposition.hpp"   // carryColumnPresent (card BCL-d)

#include <Eigen/Eigenvalues>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace GnssProcessor {

void networkClockCloneRegister(NetworkRunContext& ctx, NetworkEstimator& est,
                               const std::vector<std::string>& registered_clock_sats, const EpochEvents& events) {
    if (!ctx.knobs.carry_cpl_clone) return;
    ClockCloneEpoch& E = ctx.carry.ccl;
    E.clear();
    if (!ctx.carry.cxu_stash_valid) return;
    const SharedCarryCov& pcc = ctx.carry.prev_carry_cov;
    if (pcc.P.rows() == 0 || pcc.row.empty()) return;
    // A carried clock with a reinit mark folds decorrelated at the reinit variance: its
    // stashed cross-covariances are discarded, so it has no previous value to clone.
    std::set<std::string> carried, reinit;
    for (const CarryPrior& cp : ctx.carry.prev_carry) {
        if (cp.kind != SharedParamKind::Clock) continue;
        carried.insert(cp.sat);
        if (cp.inflate_q) reinit.insert(cp.sat);
    }
    for (const std::string& sat : registered_clock_sats) {
        if (!carried.count(sat) || reinit.count(sat)) continue;
        if (!pcc.row.count(sharedCarryKey(SharedParamKind::Clock, sat, ""))) continue;
        E.col[sat] = est.registerClockClone(sat);
    }
    // Card BCL: after card SCL's clocks.  BCL-a clones every phase and code bias with a
    // stashed row; BCL-d clones the keys that will (or may) be re-attributed: a clock card
    // SCL did not clone (reinit-marked, or its column absent) and, without BCL-a, a bias whose
    // column is absent, whose phase-bias frame is re-referenced or held, or which is parked.
    // The routes are final only at the decision, after this layout; a clone registered for a
    // key that then folds is idle and takes an isolated prior.
    if (ctx.knobs.carry_cpl_bclone || ctx.knobs.carry_cpl_dclone) {
        for (const CarryPrior& cp : ctx.carry.prev_carry) {
            if (cp.kind == SharedParamKind::VtecSh) continue;
            const std::string key = sharedCarryKey(cp.kind, cp.sat, cp.signal);
            if (!pcc.row.count(key) || E.xcol.count(key)) continue;
            bool want = false;
            if (cp.kind == SharedParamKind::Clock) {
                want = ctx.knobs.carry_cpl_dclone && !E.col.count(cp.sat);
            } else if (cp.kind == SharedParamKind::PhaseBias || cp.kind == SharedParamKind::CodeBias) {
                want = ctx.knobs.carry_cpl_bclone;
                if (!want && ctx.knobs.carry_cpl_dclone) {
                    const bool pb = cp.kind == SharedParamKind::PhaseBias;
                    want = !carryColumnPresent(est, cp) || cp.t_park_s > 0.0
                        || (pb && (events.phase_datum.eq34_reref.count({cp.sat, cp.signal})
                                   || ctx.carry.phase_anchor_reref.count({cp.sat, cp.signal})));
                }
            }
            if (!want) continue;
            E.xcol[key] = est.registerClone(cp.kind, cp.sat, cp.signal);
            E.xid[key]  = BorderCloneId{cp.kind, cp.sat, cp.signal};
        }
        if (!E.xcol.empty()) {
            ClockCloneCounters& C = ctx.counters.ccl;
            ++C.bcl_epochs;
            C.bcl_registered += static_cast<long long>(E.xcol.size());
            C.bcl_registered_max = std::max(C.bcl_registered_max, static_cast<long long>(E.xcol.size()));
        }
    }
    if (E.col.empty()) return;
    ClockCloneCounters& C = ctx.counters.ccl;
    ++C.epochs;
    C.registered += static_cast<long long>(E.col.size());
    C.registered_max = std::max(C.registered_max, static_cast<long long>(E.col.size()));
}

void networkClockClonePrepare(NetworkRunContext& ctx,
                              const std::vector<const CarryPrior*>& cp_of_p) {
    ClockCloneEpoch& E = ctx.carry.ccl;
    if (!ctx.knobs.carry_cpl_clone) return;
    E.rows_on = false;
    E.active.clear();
    E.xactive.clear();
    E.xdep.clear();
    E.rows_epoch = CouplingExactEpoch{};
    if (E.col.empty() && E.xcol.empty()) return;
    const CouplingExactEpoch& ep = ctx.carry.cxu_epoch;
    if (!ep.ok) return;
    ClockCloneCounters& C = ctx.counters.ccl;
    const Eigen::Index nS = ep.nS();
    CouplingExactEpoch ce = ep;
    ce.clone_s.assign(static_cast<std::size_t>(nS), 0);
    for (Eigen::Index s = 0; s < nS; ++s) {
        const int p = ep.p_of_s[static_cast<std::size_t>(s)];
        if (p < 0 || static_cast<std::size_t>(p) >= cp_of_p.size()) continue;
        if (!ep.clk_p[static_cast<std::size_t>(p)]) continue;
        const CarryPrior* cp = cp_of_p[static_cast<std::size_t>(p)];
        if (!cp || cp->kind != SharedParamKind::Clock) continue;
        const auto it = E.col.find(cp->sat);
        if (it == E.col.end()) continue;
        // A seam's level noise couples the clock to its phase biases inside Q_S (card CTU
        // G5); the clone carries no such term, so a seam satellite stays on its column.
        if (cp->inflate_q || cp->fold.has_seam) continue;
        const double q = ep.q_s(s);
        const double P = ep.P(p, p);
        if (!(q > 0.0) || !std::isfinite(q) || !(q > kClockCloneMinQRel * P)) continue;
        ce.col_of_s[static_cast<std::size_t>(s)] = it->second;
        ce.q_s(s) = 0.0;
        ce.clone_s[static_cast<std::size_t>(s)] = 1;
        ClockCloneActive a;
        a.p    = p;
        a.s    = static_cast<int>(s);
        a.col  = it->second;
        a.q    = q;
        a.shat = ep.shat_s(s);
        E.active[cp->sat] = a;
    }
    // Card BCL-a: each phase and code bias in S on its clone at Q = 0, by card SCL's rule.
    if (ctx.knobs.carry_cpl_bclone && !E.xcol.empty()) {
        for (Eigen::Index s = 0; s < nS; ++s) {
            const int p = ep.p_of_s[static_cast<std::size_t>(s)];
            if (p < 0 || static_cast<std::size_t>(p) >= cp_of_p.size()) continue;
            const CarryPrior* cp = cp_of_p[static_cast<std::size_t>(p)];
            if (!cp || (cp->kind != SharedParamKind::PhaseBias && cp->kind != SharedParamKind::CodeBias))
                continue;
            const std::string key = sharedCarryKey(cp->kind, cp->sat, cp->signal);
            const auto it = E.xcol.find(key);
            if (it == E.xcol.end()) continue;
            const double q = ep.q_s(s);
            const double P = ep.P(p, p);
            if (cp->inflate_q || cp->fold.has_seam || !(q > 0.0) || !std::isfinite(q)
                || !(q > kClockCloneMinQRel * P)) {
                ++C.bcl_a_screened;
                continue;
            }
            ce.col_of_s[static_cast<std::size_t>(s)] = it->second;
            ce.q_s(s) = 0.0;
            ce.clone_s[static_cast<std::size_t>(s)] = 1;
            BorderCloneActive a;
            a.id   = BorderCloneId{cp->kind, cp->sat, cp->signal};
            a.p    = p;
            a.s    = static_cast<int>(s);
            a.col  = it->second;
            a.q    = q;
            a.shat = ep.shat_s(s);
            a.route = static_cast<int>(CouplingExactDRoute::Present);
            E.xactive[key] = a;
        }
    }
    // Card BCL-d: each key card CTU re-attributes, with a carried entry and a registered
    // clone, joins S on its clone at Q = 0 (the rows keep its harvested coefficient, and the
    // fold gives the clone the stashed block's row and column with no walk).
    CouplingExactEpoch ced;
    std::map<std::string, BorderCloneActive> dep;
    if (ctx.knobs.carry_cpl_dclone) {
        ced = ce;
        std::vector<double> qd(ced.q_s.data(), ced.q_s.data() + ced.q_s.size());
        std::vector<double> sd(ced.shat_s.data(), ced.shat_s.data() + ced.shat_s.size());
        const int np = static_cast<int>(ep.s_of_p.size());
        for (int p = 0; p < np; ++p) {
            if (ep.s_of_p[static_cast<std::size_t>(p)] >= 0) continue;
            const int route = ep.route_p[static_cast<std::size_t>(p)];
            const int rt = (route >= 0 && route < kCouplingExactRoutes) ? route
                         : static_cast<int>(CouplingExactDRoute::Skipped);
            const CarryPrior* cp = (static_cast<std::size_t>(p) < cp_of_p.size())
                                       ? cp_of_p[static_cast<std::size_t>(p)] : nullptr;
            if (!cp) {
                ++C.bcl_d_nocarry;
                continue;
            }
            const std::string key = sharedCarryKey(cp->kind, cp->sat, cp->signal);
            Eigen::Index col = -1;
            if (cp->kind == SharedParamKind::Clock) {
                const auto ic = E.col.find(cp->sat);
                if (ic != E.col.end()) col = ic->second;
            }
            if (col < 0) {
                const auto ix = E.xcol.find(key);
                if (ix != E.xcol.end()) col = ix->second;
            }
            if (col < 0 || !std::isfinite(cp->value)) {
                ++C.bcl_d_noclone[rt];
                continue;
            }
            ced.s_of_p[static_cast<std::size_t>(p)] = static_cast<int>(ced.p_of_s.size());
            ced.p_of_s.push_back(p);
            ced.col_of_s.push_back(col);
            ced.clone_s.push_back(1);
            qd.push_back(0.0);
            sd.push_back(cp->value);
            BorderCloneActive a;
            a.id    = BorderCloneId{cp->kind, cp->sat, cp->signal};
            a.p     = p;
            a.s     = static_cast<int>(ced.p_of_s.size()) - 1;
            a.col   = col;
            a.shat  = cp->value;
            a.route = rt;
            dep[key] = a;
        }
        ced.q_s    = Eigen::Map<const Eigen::VectorXd>(qd.data(), static_cast<Eigen::Index>(qd.size()));
        ced.shat_s = Eigen::Map<const Eigen::VectorXd>(sd.data(), static_cast<Eigen::Index>(sd.size()));
    }
    if (E.active.empty() && E.xactive.empty() && dep.empty()) return;
    // The off-diagonal process noise is the seam's (a moved clock carries none), but a pair
    // touching a moved clock would put a walk on a clone; drop it.
    const auto dropPairs = [](CouplingExactEpoch& e) -> long long {
        if (e.q_off.empty()) return 0;
        const Eigen::Index n = e.nS();
        const auto before = e.q_off.size();
        e.q_off.erase(std::remove_if(e.q_off.begin(), e.q_off.end(),
                                     [&](const std::tuple<int, int, double>& t) {
                                         const int a = std::get<0>(t), b = std::get<1>(t);
                                         return (a >= 0 && a < n && e.clone_s[static_cast<std::size_t>(a)])
                                             || (b >= 0 && b < n && e.clone_s[static_cast<std::size_t>(b)]);
                                     }),
                      e.q_off.end());
        return static_cast<long long>(before - e.q_off.size());
    };
    if (!dep.empty()) {
        const long long dropped = dropPairs(ced);
        if (couplingExactFactor(ced)) {
            C.q_off_dropped += dropped;
            E.xdep = std::move(dep);
            for (const auto& [key, a] : E.xdep) {
                (void)key;
                ++C.bcl_d_active[a.route];
            }
            E.rows_epoch = std::move(ced);
            E.rows_on = true;
            if (!E.active.empty()) {
                ++C.rows_epochs;
                C.active += static_cast<long long>(E.active.size());
            }
            C.bcl_a_active += static_cast<long long>(E.xactive.size());
            return;
        }
        // The stashed block over S and the re-attributed keys did not factor: the rows take
        // the epoch without them (their terms re-attributed, as card CTU does).
        ++C.bcl_d_prepare_retry;
        for (const auto& [key, a] : dep) {
            (void)key;
            ++C.bcl_d_noclone[a.route];
        }
        if (E.active.empty() && E.xactive.empty()) return;
    }
    C.q_off_dropped += dropPairs(ce);
    if (!couplingExactFactor(ce)) {
        ++C.prepare_unfactored;
        E.active.clear();
        E.xactive.clear();
        return;
    }
    E.rows_epoch = std::move(ce);
    E.rows_on = true;
    if (!E.active.empty()) {
        ++C.rows_epochs;
        C.active += static_cast<long long>(E.active.size());
    }
    C.bcl_a_active += static_cast<long long>(E.xactive.size());
}

bool networkClockCloneGateView(NetworkRunContext& ctx, NetworkEstimator& est,
                               Eigen::MatrixXd& out) {
    if (!ctx.knobs.carry_cpl_clone) return false;
    ClockCloneEpoch& E = ctx.carry.ccl;
    // Card SCL2 X2: the shared-history columns follow the clones and are marginalized with
    // them (numBorderNuisance; 0 history columns off, so this is the clones as before).
    // Card BCL: its clones are border nuisance too (they precede the history).
    if (E.col.empty() && E.xcol.empty() && est.numHistoryColumns() == 0) return false;
    const Eigen::MatrixXd& N = est.reducedSharedNormals();
    const Eigen::Index n  = N.rows();
    const Eigen::Index nc = est.numBorderNuisance();
    const Eigen::Index n0 = n - nc;
    if (nc <= 0 || n0 < 0 || N.cols() != n) return false;
    std::vector<Eigen::Index> ref;
    long long idle = 0;
    for (Eigen::Index c = n0; c < n; ++c) {
        if (N.col(c).cwiseAbs().maxCoeff() > 0.0) ref.push_back(c);
        else ++idle;
    }
    Eigen::MatrixXd V = N.topLeftCorner(n0, n0);
    long long null_dirs = 0;
    if (!ref.empty()) {
        const Eigen::Index k = static_cast<Eigen::Index>(ref.size());
        Eigen::MatrixXd Ncc(k, k), Nsc(n0, k);
        for (Eigen::Index a = 0; a < k; ++a) {
            for (Eigen::Index b = 0; b < k; ++b)
                Ncc(a, b) = 0.5 * (N(ref[a], ref[b]) + N(ref[b], ref[a]));
            Nsc.col(a) = N.block(0, ref[a], n0, 1);
        }
        // Correlation-scaled pseudo-inverse of the clones' block: a direction with no
        // information (relative 1e-12) is dropped, which is the marginalization of a
        // variable the data do not see.
        Eigen::VectorXd dsc(k);
        for (Eigen::Index a = 0; a < k; ++a)
            dsc(a) = (Ncc(a, a) > 0.0) ? 1.0 / std::sqrt(Ncc(a, a)) : 0.0;
        const Eigen::MatrixXd Cn = dsc.asDiagonal() * Ncc * dsc.asDiagonal();
        const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(Cn);
        if (es.info() != Eigen::Success) {
            ++ctx.counters.ccl.gate_failed;
            return false;
        }
        const Eigen::VectorXd lam = es.eigenvalues();
        const double lmax = lam.cwiseAbs().maxCoeff();
        const Eigen::MatrixXd W = Nsc * dsc.asDiagonal() * es.eigenvectors();
        for (Eigen::Index i = 0; i < k; ++i) {
            if (!(lam(i) > 1e-12 * lmax)) {
                ++null_dirs;
                continue;
            }
            V.noalias() -= (W.col(i) / lam(i)) * W.col(i).transpose();
        }
        V = (0.5 * (V + V.transpose())).eval();
        if (!V.allFinite()) {
            ++ctx.counters.ccl.gate_failed;
            return false;
        }
    }
    if (!E.gate_counted) {
        E.gate_counted = true;
        ClockCloneCounters& C = ctx.counters.ccl;
        ++C.gate_epochs;
        C.gate_referenced += static_cast<long long>(ref.size());
        C.gate_idle       += idle;
        C.gate_null_dirs  += null_dirs;
    }
    out = std::move(V);
    return true;
}

Eigen::MatrixXd networkClockClonePadRows(const Eigen::MatrixXd& H, Eigen::Index n_shared) {
    Eigen::MatrixXd F = Eigen::MatrixXd::Zero(H.rows(), n_shared);
    const Eigen::Index w = std::min<Eigen::Index>(H.cols(), n_shared);
    if (H.rows() > 0 && w > 0) F.leftCols(w) = H.leftCols(w);
    return F;
}

void networkClockCloneFoldBegin(NetworkRunContext& ctx, NetworkEstimator& est) {
    if (!ctx.knobs.carry_cpl_clone) return;
    ClockCloneEpoch& E = ctx.carry.ccl;
    E.referenced.clear();
    E.standin.clear();
    E.xreferenced.clear();
    E.xstandin.clear();
    E.xdprior.clear();
    if (E.col.empty() && E.xcol.empty()) return;
    const Eigen::MatrixXd& N = est.reducedSharedNormals();
    for (const auto& [sat, col] : E.col)
        if (col >= 0 && col < N.cols() && N.col(col).cwiseAbs().maxCoeff() > 0.0)
            E.referenced.insert(sat);
    for (const auto& [key, col] : E.xcol)   // card BCL
        if (col >= 0 && col < N.cols() && N.col(col).cwiseAbs().maxCoeff() > 0.0)
            E.xreferenced.insert(key);
}

bool networkClockCloneDenseFold(NetworkRunContext& ctx, EpochWorkspace& ws,
                                NetworkEstimator& est,
                                const std::vector<NetworkEstimator::SharedPrior>& dkeys,
                                const std::vector<std::size_t>& drow,
                                const std::vector<double>& dvar_eff,
                                const std::vector<char>& dinflate,
                                const std::vector<char>& dreexp,
                                const Eigen::MatrixXd& cov_eff, bool within_epoch_armed,
                                std::map<std::string, double>* clk_prior_info, const EpochEvents& events) {
    if (!ctx.knobs.carry_cpl_clone) return false;
    ClockCloneEpoch& E = ctx.carry.ccl;
    if (!E.rows_on || (E.active.empty() && E.xactive.empty() && E.xdep.empty())) return false;
    ClockCloneCounters& C = ctx.counters.ccl;
    const SharedCarryCov& pcc = ctx.carry.prev_carry_cov;
    const std::size_t kd = dkeys.size();
    std::vector<NetworkEstimator::SharedPrior> keys = dkeys;
    Eigen::MatrixXd cov = cov_eff;
    std::vector<NetworkEstimator::CloneWalk> walks;
    std::set<std::string> met;
    std::map<std::string, double> q_of;
    // Card BCL-d: clones moved in the rows' epoch whose key does not fold as decided (retired
    // or screened at the fold, or folded decorrelated as a reinit); with BCL-d they take the
    // stashed prior with no walk instead of being left free.  A clone whose key folds on its
    // own column on a moved carry or a walk that is not positive stays free: the key's own
    // row would put c(t) and c(t-1) in one block with the walk as their only difference.
    const bool dclone = ctx.knobs.carry_cpl_dclone;
    std::vector<BorderCloneActive> fallen;
    const auto fallClock = [&](const std::string& sat, const ClockCloneActive& ac) {
        if (!dclone) return;
        BorderCloneActive f;
        f.id   = BorderCloneId{SharedParamKind::Clock, sat, ""};
        f.p    = ac.p;
        f.s    = ac.s;
        f.col  = ac.col;
        f.shat = ac.shat;
        f.route = -1;   // fell: no card CTU route of its own
        fallen.push_back(f);
    };

    // Card SCL B2: a constellation whose block the dense S-transform re-expressed this epoch
    // (network.handover_stransform_dense).  The rows hold their clocks' harvested
    // coefficients on the clones, which are time-(t-1) states in the outgoing frame; the
    // epoch's columns are in the incoming one.  The joint prior is the law of
    // (y_clones, S y + w): the stand-ins at the block before the transform (card CTU's copy,
    // taken before PASS B), the current keys at S P S^T (cov_eff), the cross block one-sided,
    // P_ab - u_b P_aq, and each stood-in clock's walk c_s(t) - c_s(t-1) + c_q(t-1) at its fold
    // variance.  The incoming pivot's clone stands in too (the other walks reference it); its
    // own column, pinned, keeps the fold's independent prior as an extra key.  It applies when
    // the kernel's self-check passed, the copy is the block before (its size), and the
    // incoming pivot's clone is active, folded on its row and unmoved; otherwise the
    // constellation's clocks fold on their own columns, as before (counted).
    const CouplingExactEpoch& xe = ctx.carry.cxu_epoch;
    struct DenseHo {
        const EpochEvents::Handover::StsDense* rec{nullptr};
        bool        ok{false};
        std::size_t a_q{0};   // the incoming pivot's dense key
    };
    std::map<char, DenseHo> dho;
    for (const auto& [cc, rec] : events.handover.sts_dense) {
        if (!events.handover.sts_consts.count(cc)) continue;
        DenseHo h;
        h.rec = &rec;
        const auto aq = E.active.find(rec.q);
        bool found = false;
        for (std::size_t a = 0; a < kd && !found; ++a)
            if (dkeys[a].kind == SharedParamKind::Clock && !dkeys[a].clone && dkeys[a].sat == rec.q) {
                h.a_q = a;
                found = true;
            }
        h.ok = rec.selfcheck_ok && xe.ok && static_cast<std::size_t>(xe.P.rows()) == rec.k_old
            && rec.iq >= 0 && static_cast<std::size_t>(rec.iq) < rec.k_old
            && aq != E.active.end() && aq->second.p == rec.iq && found
            && static_cast<int>(drow[h.a_q]) == rec.iq && !dinflate[h.a_q]
            && rec.clean.count(rec.q) > 0;
        if (h.ok) {   // the incoming pivot's clone passes the stand-in screen below
            const Eigen::Index iq = static_cast<Eigen::Index>(rec.iq);
            const double Pq = xe.P(iq, iq);
            const double Qq = dvar_eff[h.a_q] - pcc.P(iq, iq);
            h.ok = Pq > 0.0 && Qq > 0.0 && std::isfinite(Qq) && Qq > kClockCloneMinQRel * Pq;
        }
        if (!h.ok) ++C.fold_dense_noref;
        dho.emplace(cc, h);
    }
    const auto denseOf = [&](const std::string& sat) -> const DenseHo* {
        if (dho.empty() || sat.empty()) return nullptr;
        const auto it = dho.find(letterOfSat(sat));
        return it != dho.end() ? &it->second : nullptr;
    };
    std::vector<char> st_dense(kd, 0);   // stood in across a dense-transformed handover
    std::vector<NetworkEstimator::SharedPrior> extra;   // the incoming pivots' own columns
    std::vector<double> extra_var;
    std::map<std::string, std::size_t> extra_of;

    for (std::size_t a = 0; a < kd; ++a) {
        const NetworkEstimator::SharedPrior& k = dkeys[a];
        if (k.kind != SharedParamKind::Clock || k.clone) continue;
        const auto it = E.active.find(k.sat);
        if (it == E.active.end()) continue;
        met.insert(k.sat);
        const ClockCloneActive& ac = it->second;
        // A carry that moved after the rows were built (a reinit, a re-expression since the
        // decision, a handover transform of the constellation, another stash row) is not the
        // carry the rows conditioned on: the clock folds on its own column, the clone free.
        // Card SCL B2: a dense re-expression is the exception, when nothing else moved the
        // entry between the decision and the transform.
        const bool sts = !events.handover.sts_consts.empty() && !k.sat.empty()
                      && events.handover.sts_consts.count(letterOfSat(k.sat)) > 0;
        const DenseHo* dh = sts ? denseOf(k.sat) : nullptr;
        const bool dense_ok = dh && dh->ok && dh->rec->clean.count(k.sat) > 0;
        if (dinflate[a] || (dreexp[a] && !dense_ok) || (sts && !dense_ok)
            || static_cast<int>(drow[a]) != ac.p) {
            ++C.fold_reexpressed;
            if (dinflate[a]) fallClock(k.sat, ac);   // decorrelated: no shared row with its clone
            continue;
        }
        const Eigen::Index p = static_cast<Eigen::Index>(drow[a]);
        // The clone's variance is the block's before any handover transform (they are equal
        // off a handover); the walk is the fold's increment over the variance it folds on.
        const double P = dense_ok ? xe.P(p, p) : pcc.P(p, p);
        const double Q = dvar_eff[a] - pcc.P(p, p);   // the anchor's own decision, floors and seams in var_eff
        if (!(P > 0.0) || !(Q > 0.0) || !std::isfinite(Q) || !(Q > kClockCloneMinQRel * P)) {
            ++C.fold_q_nonpos;
            continue;
        }
        const Eigen::Index ia = static_cast<Eigen::Index>(a);
        keys[a].clone = true;
        keys[a].value = ac.shat;    // the rows' stashed value
        keys[a].info  = 1.0 / P;    // diagonal paths only
        cov(ia, ia)   = P;          // the clone at the stashed variance; off-diagonals unchanged
        if (dense_ok) {
            st_dense[a] = 1;
            if (k.sat == dh->rec->q) {
                // The incoming pivot: its clone stands in, and its column (pinned this epoch,
                // exactly 0 in the incoming frame) keeps the fold's independent prior.
                NetworkEstimator::SharedPrior own = k;
                extra_of[k.sat] = extra.size();
                extra.push_back(own);
                extra_var.push_back(dvar_eff[a]);
                continue;
            }
        }
        NetworkEstimator::CloneWalk w;
        w.sat  = k.sat;
        w.mean = k.value - ac.shat;
        w.q    = Q;
        if (dense_ok) {
            // c_s(t) - c_s(t-1) + c_q(t-1): the clone of q enters with its stashed value.
            const auto aq = E.active.find(dh->rec->q);
            w.ref  = dh->rec->q;
            w.mean = k.value - ac.shat + aq->second.shat;
        }
        if (w.mean != 0.0) ++C.fold_walk_moved;
        walks.push_back(w);
        q_of[k.sat] = Q;
    }
    for (const auto& [sat, ac] : E.active) {
        if (met.count(sat)) continue;
        ++C.fold_retired;
        fallClock(sat, ac);
    }
    // Card BCL-a: each bias key on its clone in the rows' epoch stands in for its column at
    // the stashed variance, with its walk b(t) - b(t-1) at the fold's own variance, by card
    // SCL's rule for a clock.  A clock-pivot handover's transform acts on the clocks only, so
    // a bias stands in across it; its covariance with a transformed constellation's
    // stand-ins comes from the block before the transform (card SCL B2's loop below reads the
    // clone flag set here).
    std::set<std::string> xmet;
    if (ctx.knobs.carry_cpl_bclone && !E.xactive.empty()) {
        for (std::size_t a = 0; a < kd; ++a) {
            const NetworkEstimator::SharedPrior& k = dkeys[a];
            if (k.clone || (k.kind != SharedParamKind::PhaseBias && k.kind != SharedParamKind::CodeBias))
                continue;
            const std::string key = sharedCarryKey(k.kind, k.sat, k.signal);
            const auto it = E.xactive.find(key);
            if (it == E.xactive.end()) continue;
            xmet.insert(key);
            const BorderCloneActive& xa = it->second;
            const Eigen::Index p = static_cast<Eigen::Index>(drow[a]);
            const double P = (p < pcc.P.rows()) ? pcc.P(p, p) : 0.0;
            const double Q = dvar_eff[a] - P;
            if (dinflate[a] || dreexp[a] || static_cast<int>(drow[a]) != xa.p || !(P > 0.0)
                || !(Q > 0.0) || !std::isfinite(Q) || !(Q > kClockCloneMinQRel * P)) {
                ++C.bcl_a_moved;
                if (dclone && dinflate[a]) {   // decorrelated: no shared row with its clone
                    fallen.push_back(xa);
                    fallen.back().route = -1;
                }
                continue;
            }
            const Eigen::Index ia = static_cast<Eigen::Index>(a);
            keys[a].clone = true;
            keys[a].value = xa.shat;
            keys[a].info  = 1.0 / P;
            cov(ia, ia)   = P;
            NetworkEstimator::CloneWalk w;
            w.sat    = k.sat;
            w.kind   = k.kind;
            w.signal = k.signal;
            w.mean   = k.value - xa.shat;
            w.q      = Q;
            if (w.mean != 0.0) ++C.fold_walk_moved;
            walks.push_back(w);
        }
        for (const auto& [key, xa] : E.xactive) {
            if (xmet.count(key)) continue;
            ++C.bcl_a_moved;   // its key did not fold (retired, screened)
            if (dclone) {
                fallen.push_back(xa);
                fallen.back().route = -1;
            }
        }
    }
    if (walks.empty() && E.xdep.empty() && fallen.empty()) return false;
    // Card SCL B2: the rows and columns of the stand-ins of a dense-transformed constellation,
    // in the outgoing frame, with the extra keys appended (independent).
    // u_b and the incoming pivot's row before the transform, for a current key b: a clock of
    // a constellation the dense kernel re-expressed.
    const auto uRow = [&](std::size_t b) -> Eigen::Index {
        const NetworkEstimator::SharedPrior& kb = dkeys[b];
        if (kb.kind != SharedParamKind::Clock || kb.clone || kb.sat.empty()) return -1;
        const char cons = letterOfSat(kb.sat);   // the dense kernel's key
        const auto it = events.handover.sts_dense.find(cons);
        if (it == events.handover.sts_dense.end() || !events.handover.sts_consts.count(cons))
            return -1;
        return static_cast<Eigen::Index>(it->second.iq);
    };
    bool any_dense = false;
    for (char x : st_dense) any_dense = any_dense || x != 0;
    if (any_dense || !extra.empty()) {
        const Eigen::Index n0 = static_cast<Eigen::Index>(kd);
        const Eigen::Index ne = static_cast<Eigen::Index>(extra.size());
        cov.conservativeResize(n0 + ne, n0 + ne);
        cov.rightCols(ne).setZero();
        cov.bottomRows(ne).setZero();
        for (Eigen::Index e = 0; e < ne; ++e) cov(n0 + e, n0 + e) = extra_var[static_cast<std::size_t>(e)];
        const Eigen::Index k_old = xe.P.rows();
        for (std::size_t a = 0; a < kd; ++a) {
            if (!st_dense[a]) continue;
            const Eigen::Index ia = static_cast<Eigen::Index>(a);
            const Eigen::Index ra = static_cast<Eigen::Index>(drow[a]);
            for (std::size_t b = 0; b < kd; ++b) {
                if (b == a) continue;
                const Eigen::Index ib = static_cast<Eigen::Index>(b);
                const Eigen::Index rb = static_cast<Eigen::Index>(drow[b]);
                double v = 0.0;
                if (keys[b].clone) {
                    v = (rb < k_old) ? xe.P(ra, rb) : 0.0;
                } else if (!dinflate[b]) {
                    v = (rb < k_old) ? xe.P(ra, rb) : 0.0;   // the appended outgoing pivot: exact before
                    const Eigen::Index iq_b = uRow(b);
                    if (iq_b >= 0 && iq_b < k_old) v -= xe.P(ra, iq_b);
                }
                cov(ia, ib) = v;
                cov(ib, ia) = v;
            }
        }
        for (std::size_t e = 0; e < extra.size(); ++e) keys.push_back(extra[e]);
    }
    // Card PFQ (network.clock_pivot_frame_q, docs/refound/PFQ_pivot_frame_process_noise_2026-
    // 10-08.md section 6): two clocks of a constellation share the pinned pivot's walk, which
    // the C1 fold put between their columns (cov_eff).  (1) A stand-in is a time t-1 state,
    // so that pair comes out of every covariance entry with a stood-in clone on either side;
    // between a stood-in clone and a clock folded on its own column the walks' covariance has
    // no place in this factorization and is lost (counted).  (2) The stood-in clocks' walks
    // carry it instead, as one joint factor whose covariance is checked before anything is
    // folded; a factor that does not factor folds per walk (counted).
    Eigen::MatrixXd Qw;   // empty: the per-walk form, byte-identical with the knob off
    if (ctx.knobs.pfq_on && !walks.empty()) {
        std::map<std::string, std::pair<double, double>> pf;   // sat -> (r_p, dt) of the term
        for (const CarryPrior& cp : ctx.carry.prev_carry)
            if (cp.kind == SharedParamKind::Clock && cp.fold.pfq_rate_m2ps > 0.0)
                pf[cp.sat] = {cp.fold.pfq_rate_m2ps, cp.fold.dt_applied_s};
        const auto pfOf = [&](const std::string& sat) -> const std::pair<double, double>* {
            const auto it = pf.find(sat);
            return it == pf.end() ? nullptr : &it->second;
        };
        for (std::size_t a = 0; a < kd; ++a) {
            if (dkeys[a].kind != SharedParamKind::Clock || dkeys[a].clone || st_dense[a]) continue;
            const auto* fa = pfOf(dkeys[a].sat);
            if (!fa) continue;
            for (std::size_t b = a + 1; b < kd; ++b) {
                if (dkeys[b].kind != SharedParamKind::Clock || dkeys[b].clone || st_dense[b]) continue;
                if (systemOfSat(dkeys[b].sat) != systemOfSat(dkeys[a].sat)) continue;
                if (!keys[a].clone && !keys[b].clone) continue;   // two current columns keep it
                const auto* fb = pfOf(dkeys[b].sat);
                if (!fb) continue;
                const double v = pivotFrameCross(fa->first, fa->second, fb->second);
                const Eigen::Index ia = static_cast<Eigen::Index>(a), ib = static_cast<Eigen::Index>(b);
                cov(ia, ib) -= v;
                cov(ib, ia) -= v;
                if (keys[a].clone != keys[b].clone) ++C.pfq_pairs_lost;
            }
        }
        const Eigen::Index nw = static_cast<Eigen::Index>(walks.size());
        Qw = Eigen::MatrixXd::Zero(nw, nw);
        long long pairs = 0;
        for (Eigen::Index a = 0; a < nw; ++a) {
            const auto& wa = walks[static_cast<std::size_t>(a)];
            Qw(a, a) = wa.q;
            // The pivot's walk is a clock's (card BCL's bias walks take none).
            const auto* fa = (wa.ref.empty() && wa.kind == SharedParamKind::Clock) ? pfOf(wa.sat) : nullptr;
            if (!fa) continue;
            for (Eigen::Index b = a + 1; b < nw; ++b) {
                const auto& wb = walks[static_cast<std::size_t>(b)];
                if (!wb.ref.empty() || wb.kind != SharedParamKind::Clock
                    || systemOfSat(wb.sat) != systemOfSat(wa.sat)) continue;
                const auto* fb = pfOf(wb.sat);
                if (!fb) continue;
                const double v = pivotFrameCross(fa->first, fa->second, fb->second);
                Qw(a, b) = v;
                Qw(b, a) = v;
                ++pairs;
            }
        }
        // Checked here so the joint factor below cannot throw after the block is folded.
        Eigen::VectorXd dq(nw);
        for (Eigen::Index a = 0; a < nw; ++a) dq(a) = 1.0 / std::sqrt(Qw(a, a));
        const Eigen::LDLT<Eigen::MatrixXd> fq(dq.asDiagonal() * Qw * dq.asDiagonal());
        if (pairs == 0) {
            Qw.resize(0, 0);
        } else if (fq.info() != Eigen::Success || !fq.isPositive()
                   || !(fq.vectorD().minCoeff() > 1e-12 * fq.vectorD().cwiseAbs().maxCoeff())) {
            Qw.resize(0, 0);
            ++C.pfq_walk_notpd;
        } else {
            ++C.pfq_walk_epochs;
            C.pfq_walk_pairs += pairs;
        }
    }
    // Card BCL-d: each re-attributed key on its clone, and each clone that fell above, as a
    // time t-1 state: the row and column of the block the rows were built on (card CTU's copy,
    // before any handover transform), against a stood-in clone the same, against a current key
    // the same less the incoming pivot's row for a clock of a dense-transformed constellation
    // (that key is S y + w), zero against a reinit key and an incoming pivot's own column, and
    // no walk factor.  Appended after every other key, so the indices above are unchanged.
    const std::size_t nbase = keys.size();
    std::vector<BorderCloneActive> dlist;
    if (dclone) {
        for (const auto& [key, a] : E.xdep) {
            (void)key;
            dlist.push_back(a);
        }
        for (const BorderCloneActive& f : fallen) dlist.push_back(f);
        const Eigen::Index k_old = xe.P.rows();
        dlist.erase(std::remove_if(dlist.begin(), dlist.end(),
                                   [&](const BorderCloneActive& d) {
                                       const bool bad = d.p < 0 || d.p >= k_old
                                                     || !(xe.P(d.p, d.p) > 0.0);
                                       if (bad) ++C.bcl_d_bad_row;
                                       return bad;
                                   }),
                    dlist.end());
    }
    Eigen::MatrixXd cov_nod;
    if (!dlist.empty()) {
        cov_nod = cov;
        const Eigen::Index n0 = static_cast<Eigen::Index>(nbase);
        const Eigen::Index nd = static_cast<Eigen::Index>(dlist.size());
        const Eigen::Index k_old = xe.P.rows();
        cov.conservativeResize(n0 + nd, n0 + nd);
        cov.rightCols(nd).setZero();
        cov.bottomRows(nd).setZero();
        for (Eigen::Index i = 0; i < nd; ++i) {
            const BorderCloneActive& d = dlist[static_cast<std::size_t>(i)];
            const Eigen::Index rd = static_cast<Eigen::Index>(d.p);
            for (std::size_t b = 0; b < kd; ++b) {
                const Eigen::Index rb = static_cast<Eigen::Index>(drow[b]);
                double v = 0.0;
                if (keys[b].clone) {
                    v = (rb < k_old) ? xe.P(rd, rb) : 0.0;
                } else if (!dinflate[b]) {
                    v = (rb < k_old) ? xe.P(rd, rb) : 0.0;
                    const Eigen::Index iq_b = uRow(b);
                    if (iq_b >= 0 && iq_b < k_old) v -= xe.P(rd, iq_b);
                }
                cov(n0 + i, static_cast<Eigen::Index>(b)) = v;
                cov(static_cast<Eigen::Index>(b), n0 + i) = v;
            }
            for (Eigen::Index j = 0; j < nd; ++j)
                cov(n0 + i, n0 + j) = xe.P(rd, static_cast<Eigen::Index>(dlist[static_cast<std::size_t>(j)].p));
            NetworkEstimator::SharedPrior pr;
            pr.kind   = d.id.kind;
            pr.sat    = d.id.sat;
            pr.signal = d.id.signal;
            pr.clone  = true;
            pr.value  = d.shat;
            pr.info   = 1.0 / xe.P(rd, rd);   // diagonal paths only
            keys.push_back(pr);
        }
    }
    Eigen::VectorXd lam;
    // Throws exactly when card CTU's dense fold would (non-finite, non-PD, information
    // budget), with nothing folded; the caller's diagonal fallback then applies.  Card BCL-d:
    // a block that does not factor with the time t-1 states is folded without them (they are
    // then left free), so the clones cannot add a fallback card SCL would not take.
    // Card NOM N2a: the stand-in block is the border carry in its dense form (each kind is
    // named before its call, the retry included: a fold takes the kind it was given).
    if (!dlist.empty()) {
        try {
            est.contributionKind(ContributionKind::BorderCarryDense);
            est.addSharedPriorDense(keys, cov, clk_prior_info ? &lam : nullptr,
                                    /*clock_gauge_free=*/false);
        } catch (const std::exception&) {
            ++C.bcl_d_fold_retry;
            keys.resize(nbase);
            cov = std::move(cov_nod);
            dlist.clear();
            if (walks.empty()) return false;   // nothing of card SCL's left to fold
            est.contributionKind(ContributionKind::BorderCarryDense);
            est.addSharedPriorDense(keys, cov, clk_prior_info ? &lam : nullptr,
                                    /*clock_gauge_free=*/false);
        }
    } else {
        est.contributionKind(ContributionKind::BorderCarryDense);
        est.addSharedPriorDense(keys, cov, clk_prior_info ? &lam : nullptr,
                                /*clock_gauge_free=*/false);
    }
    est.contributionKind(ContributionKind::CloneWalk);   // card NOM N2a
    est.addCloneWalkJoint(walks, Qw);
    long long n_clk_walks = 0;
    for (const auto& w : walks) {
        if (w.kind == SharedParamKind::Clock) {
            E.standin.insert(w.sat);
            ++n_clk_walks;
        } else {
            E.xstandin.insert(sharedCarryKey(w.kind, w.sat, w.signal));   // card BCL-a
            ++C.bcl_a_standin;
        }
    }
    for (const auto& [sat, e] : extra_of) {
        (void)e;
        E.standin.insert(sat);   // the incoming pivot's clone stood in (no walk of its own)
    }
    for (const BorderCloneActive& d : dlist) {   // card BCL-d
        E.xdprior.insert(sharedCarryKey(d.id.kind, d.id.sat, d.id.signal));
        ++C.bcl_d_prior;
        if (d.route < 0) ++C.bcl_d_fallen;
    }
    if (n_clk_walks > 0 || !extra_of.empty()) {
        ++C.fold_epochs;
        C.fold_standin += n_clk_walks;
    }
    if (any_dense) {
        ++C.fold_dense_epochs;
        for (char x : st_dense) C.fold_dense_standin += (x != 0) ? 1 : 0;
    }
    if (clk_prior_info) {
        for (std::size_t a = 0; a < kd; ++a) {
            if (dkeys[a].kind != SharedParamKind::Clock) continue;
            const auto qi = q_of.find(dkeys[a].sat);
            const auto ei = extra_of.find(dkeys[a].sat);
            // A cloned clock's column appears only in its walk factor: its folded
            // information is 1/Q; an incoming pivot's column only in its extra key.
            (*clk_prior_info)[dkeys[a].sat] =
                (qi != q_of.end())   ? 1.0 / qi->second
              : (ei != extra_of.end()) ? lam(static_cast<Eigen::Index>(kd + ei->second))
                                       : lam(static_cast<Eigen::Index>(a));
        }
    }
    if (within_epoch_armed) {
        ws.cap_ccl_on   = true;
        ws.cap_ccl_keys = std::move(keys);
        ws.cap_ccl_cov  = std::move(cov);
        ws.cap_ccl_walk = std::move(walks);
        ws.cap_ccl_walkQ = std::move(Qw);
    }
    return true;
}

void networkClockCloneNoteFallback(NetworkRunContext& ctx) {
    if (ctx.knobs.carry_cpl_clone && ctx.carry.ccl.rows_on) ++ctx.counters.ccl.fold_fallback;
}

void networkClockCloneFoldEnd(NetworkRunContext& ctx, EpochWorkspace& ws,
                              NetworkEstimator& est, bool within_epoch_armed) {
    if (!ctx.knobs.carry_cpl_clone) return;
    ClockCloneEpoch& E = ctx.carry.ccl;
    if (E.col.empty() && E.xcol.empty()) return;
    ClockCloneCounters& C = ctx.counters.ccl;
    const SharedCarryCov& pcc = ctx.carry.prev_carry_cov;
    std::vector<NetworkEstimator::SharedPrior> iso;
    for (const auto& [sat, col] : E.col) {
        (void)col;
        if (E.standin.count(sat)) continue;
        // Card BCL-d: a clone that fell from the stand-ins took the stashed prior.
        if (!E.xdprior.empty() && E.xdprior.count(sharedCarryKey(SharedParamKind::Clock, sat, "")))
            continue;
        if (E.referenced.count(sat)) {
            ++C.fold_free;
            continue;
        }
        double value = 0.0, var = 0.0;
        const auto rit = pcc.row.find(sharedCarryKey(SharedParamKind::Clock, sat, ""));
        if (rit != pcc.row.end() && static_cast<Eigen::Index>(rit->second) < pcc.P.rows())
            var = pcc.P(static_cast<Eigen::Index>(rit->second), static_cast<Eigen::Index>(rit->second));
        for (const CarryPrior& cp : ctx.carry.prev_carry)
            if (cp.kind == SharedParamKind::Clock && cp.sat == sat) {
                value = cp.value;
                if (!(var > 0.0)) var = cp.var;
                break;
            }
        if (!(var > 0.0) || !std::isfinite(var)) var = 1.0;
        if (!std::isfinite(value)) value = 0.0;
        NetworkEstimator::SharedPrior p;
        p.kind  = SharedParamKind::Clock;
        p.sat   = sat;
        p.clone = true;
        p.value = value;
        p.info  = 1.0 / var;
        iso.push_back(p);
    }
    const std::size_t n_b1_iso = iso.size();
    // Card BCL: the same for its clones, by key (a referenced clone without a prior is free,
    // an idle one takes the isolated prior at its stashed value and variance).
    for (const auto& [key, col] : E.xcol) {
        (void)col;
        if (E.xstandin.count(key) || E.xdprior.count(key)) continue;
        if (E.xreferenced.count(key)) {
            ++C.bcl_free;
            continue;
        }
        const auto idit = E.xid.find(key);
        if (idit == E.xid.end()) continue;
        const BorderCloneId& id = idit->second;
        double value = 0.0, var = 0.0;
        const auto rit = pcc.row.find(key);
        if (rit != pcc.row.end() && static_cast<Eigen::Index>(rit->second) < pcc.P.rows())
            var = pcc.P(static_cast<Eigen::Index>(rit->second), static_cast<Eigen::Index>(rit->second));
        for (const CarryPrior& cp : ctx.carry.prev_carry)
            if (cp.kind == id.kind && cp.sat == id.sat && cp.signal == id.signal) {
                value = cp.value;
                if (!(var > 0.0)) var = cp.var;
                break;
            }
        if (!(var > 0.0) || !std::isfinite(var)) var = 1.0;
        if (!std::isfinite(value)) value = 0.0;
        NetworkEstimator::SharedPrior p;
        p.kind   = id.kind;
        p.sat    = id.sat;
        p.signal = id.signal;
        p.clone  = true;
        p.value  = value;
        p.info   = 1.0 / var;
        iso.push_back(p);
        ++C.bcl_isolated;
    }
    if (iso.empty()) return;
    est.contributionKind(ContributionKind::ClonePrior);   // card NOM N2a
    est.addSharedPrior(iso);
    C.fold_isolated += static_cast<long long>(n_b1_iso);
    if (within_epoch_armed) ws.cap_ccl_idle = std::move(iso);
}

bool networkClockCloneReplayFold(const NetworkRunContext& ctx, const EpochWorkspace& ws,
                                 NetworkEstimator& est) {
    if (!ctx.knobs.carry_cpl_clone) return false;
    // Card NOM N2b: each re-fold names its kind, as the primary folds did.
    if (!ws.cap_ccl_idle.empty()) {
        est.contributionKind(ContributionKind::ClonePrior);
        est.addSharedPrior(ws.cap_ccl_idle);
    }
    if (!ws.cap_ccl_on) return false;
    est.contributionKind(ContributionKind::BorderCarryDense);
    est.addSharedPriorDense(ws.cap_ccl_keys, ws.cap_ccl_cov, nullptr,
                            /*clock_gauge_free=*/false);
    est.contributionKind(ContributionKind::CloneWalk);
    est.addCloneWalkJoint(ws.cap_ccl_walk, ws.cap_ccl_walkQ);
    return true;
}

bool networkClockCloneRefloat(NetworkRunContext& ctx, EpochWorkspace& ws, NetworkEstimator& est,
                              const std::set<std::string>& drop_sats, bool restore) {
    if (!ctx.knobs.carry_cpl_clone || !ws.cap_ccl_on) return false;
    // The refloat frees clocks: a walk is dropped when it is a dropped satellite's clock walk
    // (card BCL-a's bias walks of the same satellite stay).
    const auto dropped = [&](const NetworkEstimator::CloneWalk& w) {
        return w.kind == SharedParamKind::Clock && drop_sats.count(w.sat) > 0;
    };
    std::vector<NetworkEstimator::CloneWalk> wd;
    std::set<std::string> walked;
    for (const auto& w : ws.cap_ccl_walk)
        if (dropped(w)) {
            wd.push_back(w);
            walked.insert(w.sat);
        }
    std::set<std::string> block_drop;
    for (const std::string& s : drop_sats)
        if (!walked.count(s)) block_drop.insert(s);
    // The block refold runs first: it builds both of its blocks before folding anything and
    // throws before any fold, so a throw leaves the shared system untouched (the caller then
    // has nothing to restore).
    // Card NOM N2a: every call here is the clock-anchor guard's (Q3): a withdrawal, or with
    // `restore` set the restore after a failed re-solve.
    const ContributionKind q3_kind =
        restore ? ContributionKind::Q3Restore : ContributionKind::Q3Withdraw;
    if (!block_drop.empty()) {
        est.contributionKind(q3_kind);
        est.refoldSharedPriorDenseDroppingClocks(ws.cap_ccl_keys, ws.cap_ccl_cov, block_drop,
                                                 restore, /*clock_gauge_free=*/false);
    }
    if (!wd.empty()) {
        if (ws.cap_ccl_walkQ.size() == 0) {
            est.contributionKind(q3_kind);
            est.addCloneWalk(wd, restore ? 1.0 : -1.0);
        } else {
            // Card PFQ: the walks are one joint factor, so a walk cannot be withdrawn on its
            // own.  The whole factor is withdrawn and the factor over the kept walks (the
            // marginal of the walk vector, a sub-block of its covariance) folded in its place;
            // a restore does the reverse.  Both factors were checked when the fold formed them.
            std::vector<NetworkEstimator::CloneWalk> kept;
            std::vector<Eigen::Index> idx;
            for (std::size_t i = 0; i < ws.cap_ccl_walk.size(); ++i)
                if (!dropped(ws.cap_ccl_walk[i])) {
                    kept.push_back(ws.cap_ccl_walk[i]);
                    idx.push_back(static_cast<Eigen::Index>(i));
                }
            Eigen::MatrixXd Qk(static_cast<Eigen::Index>(idx.size()), static_cast<Eigen::Index>(idx.size()));
            for (std::size_t a = 0; a < idx.size(); ++a)
                for (std::size_t b = 0; b < idx.size(); ++b)
                    Qk(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(b)) =
                        ws.cap_ccl_walkQ(idx[a], idx[b]);
            if (!restore) {
                est.contributionKind(q3_kind);
                est.addCloneWalkJoint(ws.cap_ccl_walk, ws.cap_ccl_walkQ, -1.0);
                if (!kept.empty()) {
                    est.contributionKind(q3_kind);
                    est.addCloneWalkJoint(kept, Qk, 1.0);
                }
            } else {
                if (!kept.empty()) {
                    est.contributionKind(q3_kind);
                    est.addCloneWalkJoint(kept, Qk, -1.0);
                }
                est.contributionKind(q3_kind);
                est.addCloneWalkJoint(ws.cap_ccl_walk, ws.cap_ccl_walkQ, 1.0);
            }
        }
        if (!restore) ctx.counters.ccl.refloat_walks += static_cast<long long>(wd.size());
    }
    return true;
}

void networkClockCloneRefloatTrim(EpochWorkspace& ws, const std::set<std::string>& drop_sats) {
    if (!ws.cap_ccl_on || drop_sats.empty()) return;
    auto& wk = ws.cap_ccl_walk;
    // A dropped satellite's clock walk (card BCL-a's bias walks stay).
    const auto dropped = [&](const NetworkEstimator::CloneWalk& w) {
        return w.kind == SharedParamKind::Clock && drop_sats.count(w.sat) > 0;
    };
    if (ws.cap_ccl_walkQ.size() != 0) {
        // Card PFQ: the joint factor's covariance follows the kept walks.
        std::vector<Eigen::Index> idx;
        for (std::size_t i = 0; i < wk.size(); ++i)
            if (!dropped(wk[i])) idx.push_back(static_cast<Eigen::Index>(i));
        Eigen::MatrixXd Qk(static_cast<Eigen::Index>(idx.size()), static_cast<Eigen::Index>(idx.size()));
        for (std::size_t a = 0; a < idx.size(); ++a)
            for (std::size_t b = 0; b < idx.size(); ++b)
                Qk(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(b)) = ws.cap_ccl_walkQ(idx[a], idx[b]);
        ws.cap_ccl_walkQ = std::move(Qk);
    }
    wk.erase(std::remove_if(wk.begin(), wk.end(), dropped), wk.end());
    std::vector<Eigen::Index> keep;
    keep.reserve(ws.cap_ccl_keys.size());
    for (std::size_t i = 0; i < ws.cap_ccl_keys.size(); ++i) {
        const auto& k = ws.cap_ccl_keys[i];
        if (k.kind == SharedParamKind::Clock && !k.clone && drop_sats.count(k.sat)) continue;
        keep.push_back(static_cast<Eigen::Index>(i));
    }
    if (keep.size() == ws.cap_ccl_keys.size()) return;
    std::vector<NetworkEstimator::SharedPrior> nk;
    Eigen::MatrixXd nc(static_cast<Eigen::Index>(keep.size()), static_cast<Eigen::Index>(keep.size()));
    nk.reserve(keep.size());
    for (std::size_t a = 0; a < keep.size(); ++a) {
        nk.push_back(ws.cap_ccl_keys[static_cast<std::size_t>(keep[a])]);
        for (std::size_t b = 0; b < keep.size(); ++b)
            nc(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(b)) =
                ws.cap_ccl_cov(keep[a], keep[b]);
    }
    ws.cap_ccl_keys = std::move(nk);
    ws.cap_ccl_cov  = std::move(nc);
}

ClockCloneHarvest networkClockCloneHarvestBegin(NetworkRunContext& ctx,
                                                const NetworkEstimator& est,
                                                const NetworkEstimator::Datum& datum,
                                                const std::map<Eigen::Index, CoupledShared>& col2desc) {
    ClockCloneHarvest h;
    // Card SCL2 X2: Y = the clones and the shared-history columns (numBorderNuisance equals
    // numClockClones off), and the rows' conditional variance is deferred to the history's
    // compression (filter/NetworkSharedHistory).
    if (!ctx.knobs.carry_cpl_clone || est.numBorderNuisance() == 0) return h;
    h.shx = ctx.knobs.carry_cpl_shx;
    ClockCloneCounters& C = ctx.counters.ccl;
    ++C.harvest_epochs;
    h.n_shared = est.numShared();
    h.n0       = h.n_shared - est.numBorderNuisance();
    if (!est.cloneConditional(datum, h.K, h.B) || h.K.cols() != h.n_shared
        || h.B.rows() != est.numBorderNuisance()) {
        ++C.harvest_noreg;
        h.noreg = true;
        return h;
    }
    h.on = true;
    // Card SMH's re-expression formed this regression at the smoothed hold's update, which
    // precedes this harvest: the two must be one regression of one solve (counted).
    if (ctx.carry.ccl.smh_reg) {
        const ClockCloneEpoch& E0 = ctx.carry.ccl;
        ++C.smh_reg_checked;
        bool same = E0.smh_K.rows() == h.K.rows() && E0.smh_K.cols() == h.K.cols()
                 && E0.smh_B.rows() == h.B.rows() && E0.smh_B.cols() == h.B.cols();
        if (same) {
            const double d = std::max((E0.smh_K - h.K).cwiseAbs().maxCoeff(),
                                      (E0.smh_B - h.B).cwiseAbs().maxCoeff());
            C.smh_reg_maxdiff = std::max(C.smh_reg_maxdiff, d);
            same = (d == 0.0);
        }
        if (!same) ++C.smh_reg_mismatch;
    }
    // The clock constellations of the gauge restore: each satellite clock column, each
    // clone, and each constellation's datum-pinned clock.
    h.col_cc.assign(static_cast<std::size_t>(h.n0), '\0');
    for (const auto& [col, d] : col2desc)
        if (d.kind == SharedParamKind::Clock && !d.sat.empty() && col >= 0 && col < h.n0)
            h.col_cc[static_cast<std::size_t>(col)] = letterOfSat(d.sat);
    h.clone_cc.assign(static_cast<std::size_t>(h.n_shared - h.n0), '\0');
    const ClockCloneEpoch& E = ctx.carry.ccl;
    for (const auto& [sat, col] : E.col) {
        const Eigen::Index i = col - h.n0;
        if (i < 0 || i >= static_cast<Eigen::Index>(h.clone_cc.size()) || sat.empty()) continue;
        h.clone_cc[static_cast<std::size_t>(i)] = letterOfSat(sat);
        if (E.standin.count(sat) && h.B(i, i) >= 0.0) C.post_sd.push_back(std::sqrt(h.B(i, i)));
    }
    // Card BCL: a clock clone card SCL did not register is a member of the clock gauge like
    // card SCL's; a bias clone is not ('\0', as a history column).
    for (const auto& [key, col] : E.xcol) {
        const auto idit = E.xid.find(key);
        if (idit == E.xid.end() || idit->second.kind != SharedParamKind::Clock
            || idit->second.sat.empty()) continue;
        const Eigen::Index i = col - h.n0;
        if (i < 0 || i >= static_cast<Eigen::Index>(h.clone_cc.size())) continue;
        h.clone_cc[static_cast<std::size_t>(i)] = letterOfSat(idit->second.sat);
    }
    for (const auto& p : datum)
        if (p.column >= 0 && p.column < h.n0 && h.col_cc[static_cast<std::size_t>(p.column)])
            h.pin_col[h.col_cc[static_cast<std::size_t>(p.column)]] = p.column;
    return h;
}

void networkClockCloneHarvestRow(ClockCloneHarvest& h, NetworkRunContext& ctx,
                                 std::size_t est_idx, Eigen::Index local_col,
                                 Eigen::VectorXd& row, double& cond_var) {
    if (!h.on && !h.noreg) return;
    if (row.size() != h.n_shared || h.n0 < 0 || h.n0 > h.n_shared) return;
    const Eigen::Index nc = h.n_shared - h.n0;
    if (nc <= 0) return;
    const Eigen::VectorXd rc = row.segment(h.n0, nc);
    if (!(rc.cwiseAbs().maxCoeff() > 0.0)) return;
    ClockCloneCounters& C = ctx.counters.ccl;
    if (!h.on) {
        ++C.harvest_rows_dropped;
        row.segment(h.n0, nc).setZero();
        return;
    }
    const Eigen::VectorXd moved = h.K.transpose() * rc;   // zero on the clone and pinned columns
    row += moved;
    row.segment(h.n0, nc).setZero();
    // The clock-family deficit per constellation onto its pinned clock: the clones'
    // coefficients minus what the re-attribution put on the constellation's clocks.
    std::map<char, double> deficit;
    for (Eigen::Index i = 0; i < nc; ++i) {
        const char cc = (static_cast<std::size_t>(i) < h.clone_cc.size())
                            ? h.clone_cc[static_cast<std::size_t>(i)] : '\0';
        if (cc) deficit[cc] += rc(i);
    }
    for (Eigen::Index j = 0; j < h.n0 && static_cast<std::size_t>(j) < h.col_cc.size(); ++j) {
        const char cc = h.col_cc[static_cast<std::size_t>(j)];
        if (cc && moved(j) != 0.0) deficit[cc] -= moved(j);
    }
    for (const auto& [cc, d] : deficit) {
        if (d == 0.0) continue;
        const auto pv = h.pin_col.find(cc);
        if (pv == h.pin_col.end()) {
            ++C.harvest_gauge_orphan;
            continue;
        }
        row(pv->second) += d;
        ++C.harvest_gauge_held;
        C.harvest_gauge_abs += std::abs(d);
    }
    ++C.harvest_rows;
    C.harvest_reg_mass += moved.cwiseAbs().sum();
    // Card SCL2 X2: the shared conditional term is not added per row; the compression at the
    // harvest's end keeps it in the history and adds each row's remainder.
    if (!h.shx) {
        const double add = rc.dot(h.B * rc);
        if (std::isfinite(add) && add > 0.0) cond_var += add;
        C.harvest_var_mass += std::max(0.0, add);
    }
    h.rc[{est_idx, local_col}] = rc;
}

void networkClockCloneHarvestBlock(ClockCloneHarvest& h, std::size_t est_idx,
                                   const std::shared_ptr<PlHarvestBlock>& blk,
                                   const std::vector<Eigen::Index>& cols) {
    if (!h.on || !blk) return;
    ClockCloneHarvest::Blk b;
    b.est_idx = est_idx;
    b.blk     = blk;
    b.cols    = cols;
    h.blocks.push_back(std::move(b));
}

void networkClockCloneHarvestEnd(ClockCloneHarvest& h, NetworkRunContext& ctx) {
    if (!h.on || h.rc.empty()) return;
    if (h.shx) return;   // card SCL2 X2: the history's compression adds the within-station term
    long long pairs = 0;
    for (const ClockCloneHarvest::Blk& b : h.blocks) {
        Eigen::MatrixXd& Cb = b.blk->C;
        const Eigen::Index n = static_cast<Eigen::Index>(b.cols.size());
        if (Cb.rows() != n || Cb.cols() != n) continue;
        std::vector<const Eigen::VectorXd*> r(static_cast<std::size_t>(n), nullptr);
        for (Eigen::Index a = 0; a < n; ++a) {
            const auto it = h.rc.find({b.est_idx, b.cols[static_cast<std::size_t>(a)]});
            if (it != h.rc.end()) r[static_cast<std::size_t>(a)] = &it->second;
        }
        for (Eigen::Index a = 0; a < n; ++a) {
            const Eigen::VectorXd* ra = r[static_cast<std::size_t>(a)];
            if (!ra) continue;
            const Eigen::VectorXd Bra = h.B * (*ra);
            for (Eigen::Index c = a + 1; c < n; ++c) {
                const Eigen::VectorXd* rb = r[static_cast<std::size_t>(c)];
                if (!rb) continue;
                const double v = rb->dot(Bra);
                if (!std::isfinite(v)) continue;
                bool any = false;
                if (std::isfinite(Cb(a, c))) { Cb(a, c) += v; any = true; }
                if (std::isfinite(Cb(c, a))) { Cb(c, a) += v; any = true; }
                if (any) ++pairs;
            }
        }
    }
    ctx.counters.ccl.harvest_f3_pairs += pairs;
}

void networkClockCloneRunEnd(const NetworkRunContext& ctx,
                               const std::shared_ptr<spdlog::logger>& log) {
    if (!ctx.knobs.carry_cpl_clone) return;
    const ClockCloneCounters& C = ctx.counters.ccl;
    const CouplingExactCounters& X = ctx.counters.cxu;
    std::vector<double> sd = C.post_sd;
    double p50 = 0.0, mx = 0.0;
    if (!sd.empty()) {
        std::nth_element(sd.begin(), sd.begin() + static_cast<std::ptrdiff_t>(sd.size() / 2), sd.end());
        p50 = sd[sd.size() / 2];
        mx  = *std::max_element(sd.begin(), sd.end());
    }
    const double per = C.epochs > 0 ? 1.0 / static_cast<double>(C.epochs) : 0.0;
    log->info("Network (card SCL / clock clone): {} epoch(s) with clones, registered {:.1f} mean "
              "/ {} max per epoch; rows on the clone epoch {} epoch(s) ({:.1f} clones moved per "
              "epoch, {} unfactored, {} seam pair(s) dropped); gate: {:.1f} referenced / {:.1f} "
              "idle per epoch, {} empty clone direction(s), {} view failure(s); fold: {} epoch(s), "
              "{} stand-in(s), {} free, {} isolated, {} Q<=0, {} retired, {} re-expressed, {} "
              "moved walk mean(s), {} fallback(s); rows {} with clone terms, {} clone term(s); "
              "harvest: {} epoch(s), {} without the regression, {} row(s) marginalized ({} "
              "dropped), regression mass {:.4e}, conditional variance added {:.4e} m^2, {} clock "
              "deficit(s) onto the pinned clock ({} orphan, |d| {:.4e}), {} F3 "
              "pair(s); clone posterior sd p50 {:.4f} m, max {:.4f} m ({} sample(s)); {} walk(s) "
              "withdrawn by card Q3.",
              C.epochs, static_cast<double>(C.registered) * per, C.registered_max, C.rows_epochs,
              C.rows_epochs > 0 ? static_cast<double>(C.active) / static_cast<double>(C.rows_epochs) : 0.0,
              C.prepare_unfactored, C.q_off_dropped,
              C.gate_epochs > 0 ? static_cast<double>(C.gate_referenced) / static_cast<double>(C.gate_epochs) : 0.0,
              C.gate_epochs > 0 ? static_cast<double>(C.gate_idle) / static_cast<double>(C.gate_epochs) : 0.0,
              C.gate_null_dirs, C.gate_failed, C.fold_epochs, C.fold_standin, C.fold_free,
              C.fold_isolated, C.fold_q_nonpos, C.fold_retired, C.fold_reexpressed,
              C.fold_walk_moved, C.fold_fallback, X.rows_clone, X.terms_clone, C.harvest_epochs,
              C.harvest_noreg, C.harvest_rows, C.harvest_rows_dropped, C.harvest_reg_mass,
              C.harvest_var_mass, C.harvest_gauge_held, C.harvest_gauge_orphan, C.harvest_gauge_abs,
              C.harvest_f3_pairs, p50, mx, sd.size(), C.refloat_walks);
    if (ctx.knobs.pfq_on) {
        // Card PFQ on the clone: the pivot's walk enters once, in the joint walk factor.
        log->info("Network (card PFQ / on the clock clone): {} joint fold(s) with the pivot's "
                  "walk shared between the stood-in clocks' walks ({} pair(s)); {} joint "
                  "covariance(s) that did not factor (folded per walk); {} pair(s) between a "
                  "stood-in clone and a clock on its own column, lost.",
                  C.pfq_walk_epochs, C.pfq_walk_pairs, C.pfq_walk_notpd, C.pfq_pairs_lost);
        if (C.fold_epochs > 0 && C.pfq_walk_epochs == 0)
            log->warn("Network (card PFQ): network.clock_pivot_frame_q is ARMED with the clone "
                      "but no joint walk factor formed: the term is INERT on the clone.");
    }
    if (ctx.knobs.carry_cpl_bclone || ctx.knobs.carry_cpl_dclone) {
        // Card BCL: the rest of the stashed border on clones.
        const auto routes = [](const long long (&r)[kCouplingExactRoutes]) {
            return fmt::format("{} departed / {} reinit / {} reref / {} skipped",
                               r[static_cast<int>(CouplingExactDRoute::Departed)],
                               r[static_cast<int>(CouplingExactDRoute::Reinit)],
                               r[static_cast<int>(CouplingExactDRoute::Reref)],
                               r[static_cast<int>(CouplingExactDRoute::Skipped)]);
        };
        const double perb = C.bcl_epochs > 0 ? 1.0 / static_cast<double>(C.bcl_epochs) : 0.0;
        log->info("Network (card BCL / the previous border on clones): {} epoch(s) with clones, "
                  "{:.1f} registered mean / {} max per epoch; BCL-a {}: {} bias key(s) on their "
                  "clones in the rows, {} screened, {} stood in with a walk, {} moved or "
                  "unfolded; BCL-d {}: in the rows {}, without a clone {}, {} row(s) without a "
                  "carried entry, {} rows' epoch(s) rebuilt without them; at the fold {} stashed "
                  "prior(s) ({} for clones that fell), {} outside the block, {} fold(s) retried "
                  "without them; {} referenced clone(s) left free, {} idle isolated.",
                  C.bcl_epochs, static_cast<double>(C.bcl_registered) * perb, C.bcl_registered_max,
                  ctx.knobs.carry_cpl_bclone ? "on" : "off", C.bcl_a_active, C.bcl_a_screened,
                  C.bcl_a_standin, C.bcl_a_moved, ctx.knobs.carry_cpl_dclone ? "on" : "off",
                  routes(C.bcl_d_active), routes(C.bcl_d_noclone), C.bcl_d_nocarry,
                  C.bcl_d_prepare_retry, C.bcl_d_prior, C.bcl_d_fallen, C.bcl_d_bad_row,
                  C.bcl_d_fold_retry, C.bcl_free, C.bcl_isolated);
        if (ctx.knobs.carry_cpl_bclone && C.bcl_a_standin == 0)
            log->warn("Network (card BCL-a): network.carry_coupling_bias_clone is ARMED but no "
                      "bias clone stood in: the card is INERT for this run.");
        long long d_active = 0;
        for (int r = 0; r < kCouplingExactRoutes; ++r) d_active += C.bcl_d_active[r];
        if (ctx.knobs.carry_cpl_dclone && d_active == 0 && C.bcl_d_prior == 0)
            log->warn("Network (card BCL-d): network.carry_coupling_departed_clone is ARMED but no "
                      "re-attributed key was put on a clone: the card is INERT for this run.");
    }
    if (C.fold_dense_epochs > 0 || C.fold_dense_noref > 0)
        log->info("Network (card SCL / clock clone, B2): {} joint fold(s) across a "
                  "dense-transformed handover ({} clone(s) stood in in the outgoing frame, the "
                  "walks referencing the incoming pivot's clone); {} dense-transformed "
                  "constellation-epoch(s) whose clones could not stand in.",
                  C.fold_dense_epochs, C.fold_dense_standin, C.fold_dense_noref);
    if (X.terms_clone == 0)
        log->warn("Network (card SCL): network.carry_coupling_clock_clone is ARMED but no row "
                  "referenced a clone: the card is INERT for this run.");
}

}  // namespace GnssProcessor
