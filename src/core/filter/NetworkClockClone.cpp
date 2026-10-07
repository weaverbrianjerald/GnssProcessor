/**
 * @file NetworkClockClone.cpp
 * @brief The one-epoch clock clone of card SCL (@c network.carry_coupling_clock_clone);
 *        the mechanism, the index spaces and the contract of each step are in the header.
 */

#include "filter/NetworkClockClone.hpp"

#include "app/NetworkRunContext.hpp"

#include <Eigen/Eigenvalues>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace GnssProcessor {

void networkClockCloneRegister(NetworkRunContext& ctx, NetworkEstimator& est,
                               const std::vector<std::string>& registered_clock_sats) {
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
    E.rows_epoch = CouplingExactEpoch{};
    if (E.col.empty()) return;
    const CouplingExactEpoch& ep = ctx.carry.cxu_epoch;
    if (!ep.ok) return;
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
    if (E.active.empty()) return;
    // The off-diagonal process noise is the seam's (a moved clock carries none), but a pair
    // touching a moved clock would put a walk on a clone; drop it.
    if (!ce.q_off.empty()) {
        const auto before = ce.q_off.size();
        ce.q_off.erase(std::remove_if(ce.q_off.begin(), ce.q_off.end(),
                                      [&](const std::tuple<int, int, double>& t) {
                                          const int a = std::get<0>(t), b = std::get<1>(t);
                                          return (a >= 0 && a < nS && ce.clone_s[static_cast<std::size_t>(a)])
                                              || (b >= 0 && b < nS && ce.clone_s[static_cast<std::size_t>(b)]);
                                      }),
                       ce.q_off.end());
        ctx.counters.ccl.q_off_dropped += static_cast<long long>(before - ce.q_off.size());
    }
    if (!couplingExactFactor(ce)) {
        ++ctx.counters.ccl.prepare_unfactored;
        E.active.clear();
        return;
    }
    E.rows_epoch = std::move(ce);
    E.rows_on = true;
    ++ctx.counters.ccl.rows_epochs;
    ctx.counters.ccl.active += static_cast<long long>(E.active.size());
}

bool networkClockCloneGateView(NetworkRunContext& ctx, NetworkEstimator& est,
                               Eigen::MatrixXd& out) {
    if (!ctx.knobs.carry_cpl_clone) return false;
    ClockCloneEpoch& E = ctx.carry.ccl;
    // Card SCL2 X2: the shared-history columns follow the clones and are marginalized with
    // them (numBorderNuisance; 0 history columns off, so this is the clones as before).
    if (E.col.empty() && est.numHistoryColumns() == 0) return false;
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
    if (E.col.empty()) return;
    const Eigen::MatrixXd& N = est.reducedSharedNormals();
    for (const auto& [sat, col] : E.col)
        if (col >= 0 && col < N.cols() && N.col(col).cwiseAbs().maxCoeff() > 0.0)
            E.referenced.insert(sat);
}

bool networkClockCloneDenseFold(NetworkRunContext& ctx, EpochWorkspace& ws,
                                NetworkEstimator& est,
                                const std::vector<NetworkEstimator::SharedPrior>& dkeys,
                                const std::vector<std::size_t>& drow,
                                const std::vector<double>& dvar_eff,
                                const std::vector<char>& dinflate,
                                const std::vector<char>& dreexp,
                                const Eigen::MatrixXd& cov_eff, bool within_epoch_armed,
                                std::map<std::string, double>* clk_prior_info) {
    if (!ctx.knobs.carry_cpl_clone) return false;
    ClockCloneEpoch& E = ctx.carry.ccl;
    if (!E.rows_on || E.active.empty()) return false;
    ClockCloneCounters& C = ctx.counters.ccl;
    const SharedCarryCov& pcc = ctx.carry.prev_carry_cov;
    const std::size_t kd = dkeys.size();
    std::vector<NetworkEstimator::SharedPrior> keys = dkeys;
    Eigen::MatrixXd cov = cov_eff;
    std::vector<NetworkEstimator::CloneWalk> walks;
    std::set<std::string> met;
    std::map<std::string, double> q_of;

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
        const NetworkCarryState::StsDense* rec{nullptr};
        bool        ok{false};
        std::size_t a_q{0};   // the incoming pivot's dense key
    };
    std::map<char, DenseHo> dho;
    for (const auto& [cc, rec] : ctx.carry.sts_dense) {
        if (!ctx.carry.sts_consts.count(cc)) continue;
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
        const auto it = dho.find(sat.front());
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
        const bool sts = !ctx.carry.sts_consts.empty() && !k.sat.empty()
                      && ctx.carry.sts_consts.count(k.sat.front()) > 0;
        const DenseHo* dh = sts ? denseOf(k.sat) : nullptr;
        const bool dense_ok = dh && dh->ok && dh->rec->clean.count(k.sat) > 0;
        if (dinflate[a] || (dreexp[a] && !dense_ok) || (sts && !dense_ok)
            || static_cast<int>(drow[a]) != ac.p) {
            ++C.fold_reexpressed;
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
        (void)ac;
        if (!met.count(sat)) ++C.fold_retired;
    }
    if (walks.empty()) return false;
    // Card SCL B2: the rows and columns of the stand-ins of a dense-transformed constellation,
    // in the outgoing frame, with the extra keys appended (independent).
    bool any_dense = false;
    for (char x : st_dense) any_dense = any_dense || x != 0;
    if (any_dense || !extra.empty()) {
        const Eigen::Index n0 = static_cast<Eigen::Index>(kd);
        const Eigen::Index ne = static_cast<Eigen::Index>(extra.size());
        cov.conservativeResize(n0 + ne, n0 + ne);
        cov.rightCols(ne).setZero();
        cov.bottomRows(ne).setZero();
        for (Eigen::Index e = 0; e < ne; ++e) cov(n0 + e, n0 + e) = extra_var[static_cast<std::size_t>(e)];
        // u_b and the incoming pivot's row before the transform, for a current key b: a clock
        // of a constellation the dense kernel re-expressed.
        const auto uRow = [&](std::size_t b) -> Eigen::Index {
            const NetworkEstimator::SharedPrior& kb = dkeys[b];
            if (kb.kind != SharedParamKind::Clock || kb.clone || kb.sat.empty()) return -1;
            const auto it = ctx.carry.sts_dense.find(kb.sat.front());
            if (it == ctx.carry.sts_dense.end() || !ctx.carry.sts_consts.count(kb.sat.front()))
                return -1;
            return static_cast<Eigen::Index>(it->second.iq);
        };
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
    Eigen::VectorXd lam;
    // Throws exactly when card CTU's dense fold would (non-finite, non-PD, information
    // budget), with nothing folded; the caller's diagonal fallback then applies.
    est.addSharedPriorDense(keys, cov, clk_prior_info ? &lam : nullptr,
                            /*clock_gauge_free=*/false, /*require_closure=*/false);
    est.attribTag("ccl_walk");
    est.addCloneWalk(walks);
    for (const auto& w : walks) E.standin.insert(w.sat);
    for (const auto& [sat, e] : extra_of) {
        (void)e;
        E.standin.insert(sat);   // the incoming pivot's clone stood in (no walk of its own)
    }
    ++C.fold_epochs;
    C.fold_standin += static_cast<long long>(walks.size());
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
    if (E.col.empty()) return;
    ClockCloneCounters& C = ctx.counters.ccl;
    const SharedCarryCov& pcc = ctx.carry.prev_carry_cov;
    std::vector<NetworkEstimator::SharedPrior> iso;
    for (const auto& [sat, col] : E.col) {
        (void)col;
        if (E.standin.count(sat)) continue;
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
    if (iso.empty()) return;
    est.attribTag("ccl_idle");
    est.addSharedPrior(iso);
    C.fold_isolated += static_cast<long long>(iso.size());
    if (within_epoch_armed) ws.cap_ccl_idle = std::move(iso);
}

bool networkClockCloneReplayFold(const NetworkRunContext& ctx, const EpochWorkspace& ws,
                                 NetworkEstimator& est) {
    if (!ctx.knobs.carry_cpl_clone) return false;
    if (!ws.cap_ccl_idle.empty()) est.addSharedPrior(ws.cap_ccl_idle);
    if (!ws.cap_ccl_on) return false;
    est.addSharedPriorDense(ws.cap_ccl_keys, ws.cap_ccl_cov, nullptr,
                            /*clock_gauge_free=*/false, /*require_closure=*/false);
    est.addCloneWalk(ws.cap_ccl_walk);
    return true;
}

bool networkClockCloneRefloat(NetworkRunContext& ctx, EpochWorkspace& ws, NetworkEstimator& est,
                              const std::set<std::string>& drop_sats, bool restore) {
    if (!ctx.knobs.carry_cpl_clone || !ws.cap_ccl_on) return false;
    std::vector<NetworkEstimator::CloneWalk> wd;
    std::set<std::string> walked;
    for (const auto& w : ws.cap_ccl_walk)
        if (drop_sats.count(w.sat)) {
            wd.push_back(w);
            walked.insert(w.sat);
        }
    std::set<std::string> block_drop;
    for (const std::string& s : drop_sats)
        if (!walked.count(s)) block_drop.insert(s);
    // The block refold runs first: it builds both of its blocks before folding anything and
    // throws before any fold, so a throw leaves the shared system untouched (the caller then
    // has nothing to restore).
    if (!block_drop.empty())
        est.refoldSharedPriorDenseDroppingClocks(ws.cap_ccl_keys, ws.cap_ccl_cov, block_drop,
                                                 restore, /*clock_gauge_free=*/false,
                                                 /*require_closure=*/false);
    if (!wd.empty()) {
        est.addCloneWalk(wd, restore ? 1.0 : -1.0);
        if (!restore) ctx.counters.ccl.refloat_walks += static_cast<long long>(wd.size());
    }
    return true;
}

void networkClockCloneRefloatTrim(EpochWorkspace& ws, const std::set<std::string>& drop_sats) {
    if (!ws.cap_ccl_on || drop_sats.empty()) return;
    auto& wk = ws.cap_ccl_walk;
    wk.erase(std::remove_if(wk.begin(), wk.end(),
                            [&](const NetworkEstimator::CloneWalk& w) { return drop_sats.count(w.sat) > 0; }),
             wk.end());
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
            h.col_cc[static_cast<std::size_t>(col)] = d.sat[0];
    h.clone_cc.assign(static_cast<std::size_t>(h.n_shared - h.n0), '\0');
    const ClockCloneEpoch& E = ctx.carry.ccl;
    for (const auto& [sat, col] : E.col) {
        const Eigen::Index i = col - h.n0;
        if (i < 0 || i >= static_cast<Eigen::Index>(h.clone_cc.size()) || sat.empty()) continue;
        h.clone_cc[static_cast<std::size_t>(i)] = sat[0];
        if (E.standin.count(sat) && h.B(i, i) >= 0.0) C.post_sd.push_back(std::sqrt(h.B(i, i)));
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

void networkClockCloneTeardown(const NetworkRunContext& ctx,
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
