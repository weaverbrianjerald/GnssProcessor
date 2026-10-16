/**
 * @file SharedHistory.cpp
 * @brief The shared history of card SCL2's build X2 (@c network.carry_coupling_shared_history);
 *        the mechanism, the index spaces and the contract of each step are in the header.
 */

#include "network/carry/SharedHistory.hpp"

#include "network/state/RunContext.hpp"

#include <Eigen/Eigenvalues>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace GnssProcessor {

namespace {

/// The entry a handle names, or null when it is gone.
LocalCarry* shxFindEntry(NetworkRunContext& ctx, const SharedHistoryHarvest::Handle& hd) {
    switch (hd.map) {
        case SharedHistoryMap::Amb:
            return net::getArc(ctx.entities.arcs, std::make_tuple(hd.sta, hd.sat, hd.sig),
                               &net::ArcState::amb);
        case SharedHistoryMap::Zwd:
            return net::get(ctx.entities.stations, StationId{hd.sta}, &net::StationState::zwd);
        case SharedHistoryMap::Iono:
            return net::getLos(ctx.entities.los, hd.sta, hd.sat, &net::LosState::iono_blk);
        case SharedHistoryMap::RxClk:
            return net::get(ctx.entities.stations, StationId{hd.sta}, &net::StationState::signals,
                            hd.sat, &net::StationSignalState::rx_clk);
        case SharedHistoryMap::RxCb:
            return net::get(ctx.entities.stations, StationId{hd.sta}, &net::StationState::signals,
                            hd.sat, &net::StationSignalState::rx_cb);
        case SharedHistoryMap::Pc:
            return net::getLos(ctx.entities.los, hd.sta, hd.sat, &net::LosState::pc);
    }
    return nullptr;
}

/// Visit every entry of the six carry maps that can hold history coefficients.
template <typename Fn>
void shxForEachEntry(const NetworkRunContext& ctx, Fn&& fn) {
    net::forEach(ctx.entities.arcs, &net::ArcState::amb,
                 [&](const ArcId&, const LocalCarry& lc) { fn(lc); });
    net::forEach(ctx.entities.stations, &net::StationState::zwd,
                 [&](const StationId&, const LocalCarry& lc) { fn(lc); });
    net::forEach(ctx.entities.los, &net::LosState::iono_blk,
                 [&](const LosId&, const LocalCarry& lc) { fn(lc); });
    net::forEach(ctx.entities.stations, &net::StationState::signals,
                 &net::StationSignalState::rx_clk,
                 [&](const StationId&, const std::string&, const LocalCarry& lc) { fn(lc); });
    net::forEach(ctx.entities.stations, &net::StationState::signals,
                 &net::StationSignalState::rx_cb,
                 [&](const StationId&, const std::string&, const LocalCarry& lc) { fn(lc); });
    net::forEach(ctx.entities.los, &net::LosState::pc,
                 [&](const LosId&, const LocalCarry& lc) { fn(lc); });
}

/// The p50 and the maximum of a sample (0 when empty).
std::pair<double, double> shxP50Max(std::vector<double> v) {
    if (v.empty()) return {0.0, 0.0};
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
    const double p50 = v[v.size() / 2];
    return {p50, *std::max_element(v.begin(), v.end())};
}

}  // namespace

SharedHistoryCompression sharedHistoryCompress(const Eigen::MatrixXd& R,
                                               const Eigen::MatrixXd& Sigma, int cap,
                                               const std::vector<std::size_t>& group) {
    SharedHistoryCompression out;
    const Eigen::Index m  = R.rows();
    const Eigen::Index nY = R.cols();
    if (Sigma.rows() != nY || Sigma.cols() != nY || static_cast<Eigen::Index>(group.size()) != m)
        return out;
    if (!R.allFinite() || !Sigma.allFinite()) return out;
    // L L' = Sigma_Y|s.  A Cholesky failure (a semi-definite direction at rounding) takes the
    // eigen square root with the negative eigenvalues clamped to 0, counted by the caller.
    const Eigen::MatrixXd Ss = 0.5 * (Sigma + Sigma.transpose());
    Eigen::MatrixXd L;
    const Eigen::LLT<Eigen::MatrixXd> llt(Ss);
    if (llt.info() == Eigen::Success) {
        L = llt.matrixL();
    }
    if (L.size() == 0 || !L.allFinite()) {
        const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(Ss);
        if (es.info() != Eigen::Success) return out;
        L = es.eigenvectors() * es.eigenvalues().cwiseMax(0.0).cwiseSqrt().asDiagonal();
        out.chol_fallback = true;
    }
    const Eigen::MatrixXd F = R * L;   // m x nY: each row's shared factor
    // The leading right singular directions of F: eigenvectors of the Gram F'F, decreasing.
    const Eigen::MatrixXd G = F.transpose() * F;
    const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eg(0.5 * (G + G.transpose()));
    if (eg.info() != Eigen::Success) return out;
    const Eigen::VectorXd ev = eg.eigenvalues().reverse();
    const Eigen::MatrixXd V  = eg.eigenvectors().rowwise().reverse();
    const double emax = nY > 0 ? std::max(ev(0), 0.0) : 0.0;
    int rank = 0;
    for (Eigen::Index i = 0; i < nY; ++i)
        if (emax > 0.0 && ev(i) > kSharedHistoryRankTol * emax) ++rank;
    const int kt = std::max(0, std::min(cap, rank));
    out.rank = rank;
    out.kt   = kt;
    out.coef = F * V.leftCols(kt);
    out.rem  = F * V.rightCols(nY - kt);
    const Eigen::VectorXd evp = ev.cwiseMax(0.0);
    out.total      = evp.sum();
    out.kept       = evp.head(kt).sum();
    out.remainder  = evp.tail(nY - kt).sum();
    out.frob_total = evp.squaredNorm();
    // The remainder's cross-group blocks: ||Rem Rem'||_F^2 less each group's own block's,
    // both formed in the remainder's (nY - kt)-dimensional space (||A A'||_F = ||A' A||_F).
    if (nY - kt > 0 && m > 0) {
        const double whole = (out.rem.transpose() * out.rem).squaredNorm();
        std::map<std::size_t, Eigen::MatrixXd> per;
        for (Eigen::Index i = 0; i < m; ++i) {
            Eigen::MatrixXd& P = per[group[static_cast<std::size_t>(i)]];
            if (P.size() == 0) P = Eigen::MatrixXd::Zero(nY - kt, nY - kt);
            P.noalias() += out.rem.row(i).transpose() * out.rem.row(i);
        }
        double own = 0.0;
        for (const auto& kv : per) own += kv.second.squaredNorm();
        out.cross = std::max(0.0, whole - own);
    }
    out.ok = out.coef.allFinite() && out.rem.allFinite();
    if (out.ok) {   // amendment A1 projects card SMH's rows on the same factor and directions
        out.L = L;
        out.V = V;
    }
    return out;
}

SharedHistoryProjection sharedHistoryProject(const Eigen::VectorXd& r, const Eigen::MatrixXd& L,
                                             const Eigen::MatrixXd& V, int kt) {
    SharedHistoryProjection out;
    const Eigen::Index nY = r.size();
    if (L.rows() != nY || L.cols() != nY || V.rows() != nY || V.cols() != nY || kt < 0
        || kt > nY || !r.allFinite())
        return out;
    const Eigen::RowVectorXd F = r.transpose() * L;   // the row's shared factor F_T
    const Eigen::RowVectorXd G = F * V;               // its coordinates on the directions
    out.coef  = G.head(kt).transpose();
    out.rem   = G.tail(nY - kt).squaredNorm();
    out.total = F.squaredNorm();
    out.ok    = out.coef.allFinite() && std::isfinite(out.rem) && std::isfinite(out.total);
    return out;
}

bool sharedHistoryCoefValid(const LocalCarry& lc) {
    return !lc.shx_coef.empty()
        && std::abs(lc.shx_stash_s - lc.last_seen_s) <= kSharedHistoryStampTol;
}

double sharedHistoryMarginalVar(const LocalCarry& lc) {
    if (!sharedHistoryCoefValid(lc)) return lc.var;
    double s = 0.0;
    for (const double c : lc.shx_coef) s += c * c;
    return lc.var + s;
}

void sharedHistoryRowTerms(const SharedHistoryState& shx, const LocalCarry& lc,
                           std::map<Eigen::Index, double>* emit, double& marg_var,
                           CouplingExactCounters& X) {
    if (!sharedHistoryCoefValid(lc)) return;
    // The registered columns are this compression's only when the stamps agree; any other
    // row (a stale station, a withheld epoch, a failed or absent compression) marginalizes.
    const bool gen = emit != nullptr && shx.on
                  && std::abs(shx.stash_s - lc.shx_stash_s) <= kSharedHistoryStampTol;
    long long nt = 0, nm = 0;
    double mv = 0.0;
    for (std::size_t j = 0; j < lc.shx_coef.size(); ++j) {
        const double c = lc.shx_coef[j];
        if (c == 0.0) continue;
        if (gen && j < shx.col.size() && shx.col[j] >= 0) {
            (*emit)[shx.col[j]] += c;
            ++nt;
        } else {
            mv += c * c;
            ++nm;
        }
    }
    if (nt > 0) {
        ++X.rows_hist;
        X.terms_hist += nt;
    }
    if (nm > 0) {
        marg_var += mv;
        ++X.rows_hist_marg;
        X.terms_hist_marg += nm;
        X.hist_marg_var += mv;
    }
}

void networkSharedHistoryRegister(NetworkRunContext& ctx, NetworkEstimator& est) {
    if (!ctx.knobs.carry_cpl_shx) return;
    SharedHistoryState& S = ctx.carry.shx;
    S.clearEpoch();
    if (!S.valid || S.kt <= 0) return;
    SharedHistoryCounters& C = ctx.counters.shx;
    // The rows emit history terms only on card CTU's exact route, i.e. when they were
    // harvested at the stash the rows' epoch belongs to; a compression of any other harvest
    // is not that generation (a withheld epoch, a run without a valid stash).
    if (!ctx.carry.cxu_stash_valid
        || std::abs(S.stash_s - ctx.carry.cxu_stash_s) > kSharedHistoryStampTol) {
        ++C.gen_mismatch;
        return;
    }
    std::vector<char> ref(static_cast<std::size_t>(S.kt), 0);
    shxForEachEntry(ctx, [&](const LocalCarry& lc) {
        if (!sharedHistoryCoefValid(lc)
            || std::abs(lc.shx_stash_s - S.stash_s) > kSharedHistoryStampTol) return;
        const std::size_t n = std::min(lc.shx_coef.size(), ref.size());
        for (std::size_t j = 0; j < n; ++j)
            if (lc.shx_coef[j] != 0.0) ref[j] = 1;
    });
    // Amendment A1: card SMH's live clone rows reference the directions too (empty unless the
    // smoothed hold is armed).
    const auto holdRef = [&](const PbHoldCloneShx& sh) {
        if (sh.coef.empty() || std::abs(sh.stash_s - S.stash_s) > kSharedHistoryStampTol) return;
        const std::size_t n = std::min(sh.coef.size(), ref.size());
        for (std::size_t j = 0; j < n; ++j)
            if (sh.coef[j] != 0.0) ref[j] = 1;
    };
    // The hold rows live in the satellite-signal table (card NOM step N4d), in key order.
    net::forEach(ctx.entities.sat_signals, &net::SatSignalState::smh_clone,
                 [&](const SatSignalId&, const PbHoldClone& c) { if (c.alive) holdRef(c.shx); });
    net::forEach(ctx.entities.sat_signals, &net::SatSignalState::smh_block,
                 [&](const SatSignalId&, const PbHoldClone& c) {
                     if (c.alive) {
                         holdRef(c.shx);
                         holdRef(c.pv_shx);
                     }
                 });
    S.col.assign(static_cast<std::size_t>(S.kt), Eigen::Index{-1});
    long long n = 0;
    for (int j = 0; j < S.kt; ++j) {
        if (!ref[static_cast<std::size_t>(j)]) {
            ++C.unreferenced;
            continue;
        }
        S.col[static_cast<std::size_t>(j)] = est.registerHistoryColumn(j);
        ++n;
    }
    if (n == 0) return;
    S.on = true;
    ++C.epochs;
    C.registered += n;
    C.registered_max = std::max(C.registered_max, n);
}

void networkSharedHistoryFold(NetworkRunContext& ctx, EpochWorkspace& ws, NetworkEstimator& est,
                              bool within_epoch_armed) {
    if (!ctx.knobs.carry_cpl_shx) return;
    const SharedHistoryState& S = ctx.carry.shx;
    if (!S.on) return;
    std::vector<NetworkEstimator::SharedPrior> pri;
    for (std::size_t j = 0; j < S.col.size(); ++j) {
        if (S.col[j] < 0) continue;
        NetworkEstimator::SharedPrior p;
        p.kind     = SharedParamKind::Clock;
        p.hist     = true;
        p.hist_idx = static_cast<int>(j);
        p.value    = 0.0;
        p.info     = 1.0;   // N(0, 1): the compression's unit-variance directions
        pri.push_back(p);
    }
    if (pri.empty()) return;
    est.contributionKind(ContributionKind::SharedHistory);   // card NOM N2a
    est.addSharedPrior(pri);
    ctx.counters.shx.fold_priors += static_cast<long long>(pri.size());
    if (within_epoch_armed) ws.cap_shx = std::move(pri);
}

void networkSharedHistoryReplayFold(const NetworkRunContext& ctx, const EpochWorkspace& ws,
                                    NetworkEstimator& est) {
    if (!ctx.knobs.carry_cpl_shx || ws.cap_shx.empty()) return;
    est.contributionKind(ContributionKind::SharedHistory);   // card NOM N2b: the replay names its kind
    est.addSharedPrior(ws.cap_shx);
}

SharedHistoryHarvest networkSharedHistoryHarvestBegin(NetworkRunContext& ctx,
                                                      const ClockCloneHarvest& h) {
    SharedHistoryHarvest H;
    if (!ctx.knobs.carry_cpl_shx) return H;
    if (h.noreg && ctx.carry.shx.on) ++ctx.counters.shx.noreg_epochs;
    H.on = h.on;
    return H;
}

void networkSharedHistoryNoteEntry(SharedHistoryHarvest& H, std::size_t est_idx, Eigen::Index col,
                                   SharedHistoryMap map, int sta, const std::string& sat,
                                   const std::string& sig) {
    if (!H.on) return;
    SharedHistoryHarvest::Handle hd;
    hd.map     = map;
    hd.sta     = sta;
    hd.sat     = sat;
    hd.sig     = sig;
    hd.est_idx = est_idx;
    hd.col     = col;
    H.handles.push_back(std::move(hd));
}

void networkSharedHistoryHarvestEnd(SharedHistoryHarvest& H, const ClockCloneHarvest& h,
                                    NetworkRunContext& ctx, double now_s) {
    if (!ctx.knobs.carry_cpl_shx) return;
    SharedHistoryState& S = ctx.carry.shx;
    // No regression this epoch (no clone or history registered, or B1 could not form it):
    // the rows harvested now hold no Y term, so no compression is held for the next epoch.
    if (!H.on) {
        S.valid = false;
        S.kt = 0;
        networkSharedHistoryProjectHoldRows(ctx, nullptr, now_s);   // amendment A1: per-row term
        return;
    }
    SharedHistoryCounters& C = ctx.counters.shx;
    struct RowRef {
        LocalCarry* lc{nullptr};
        const Eigen::VectorXd* rc{nullptr};
        std::size_t est_idx{0};
        Eigen::Index col{-1};
    };
    std::vector<RowRef> rows;
    std::set<std::pair<std::size_t, Eigen::Index>> noted, used;
    for (const SharedHistoryHarvest::Handle& hd : H.handles) {
        const std::pair<std::size_t, Eigen::Index> key{hd.est_idx, hd.col};
        noted.insert(key);
        const auto it = h.rc.find(key);
        if (it == h.rc.end()) continue;    // the row had no Y term: nothing shared to keep
        LocalCarry* lc = shxFindEntry(ctx, hd);
        if (!lc || std::abs(lc->last_seen_s - now_s) > kSharedHistoryStampTol) {
            ++C.rows_lost;
            continue;
        }
        if (!used.insert(key).second) continue;
        rows.push_back(RowRef{lc, &it->second, hd.est_idx, hd.col});
    }
    for (const auto& kv : h.rc)
        if (!noted.count(kv.first)) ++C.rows_unnoted;
    S.valid   = true;
    S.stash_s = now_s;
    S.kt      = 0;
    const Eigen::Index nY = h.B.rows();
    const Eigen::Index m  = static_cast<Eigen::Index>(rows.size());
    if (m == 0 || nY == 0) {
        networkSharedHistoryProjectHoldRows(ctx, nullptr, now_s);   // amendment A1: per-row term
        return;
    }
    Eigen::MatrixXd R(m, nY);
    std::vector<std::size_t> group(static_cast<std::size_t>(m));
    std::map<std::pair<std::size_t, Eigen::Index>, Eigen::Index> row_of;
    for (Eigen::Index i = 0; i < m; ++i) {
        const RowRef& rr = rows[static_cast<std::size_t>(i)];
        R.row(i) = rr.rc->transpose();
        group[static_cast<std::size_t>(i)] = rr.est_idx;
        row_of[{rr.est_idx, rr.col}] = i;
    }
    const int ncl = static_cast<int>(ctx.carry.ccl.col.size());
    const int cap = ctx.knobs.shx_rank * ncl;
    const SharedHistoryCompression X = sharedHistoryCompress(R, h.B, cap, group);
    ++C.compress_epochs;
    C.ny_sum += static_cast<long long>(nY);
    // The within-station pairs of each F3 block B1 registered: pair term(a, b) added to
    // C(a, b) and C(b, a); the diagonal is the entry's own variance.
    const auto addPairs = [&](const auto& term) {
        long long pairs = 0;
        for (const ClockCloneHarvest::Blk& b : h.blocks) {
            Eigen::MatrixXd& Cb = b.blk->C;
            const Eigen::Index n = static_cast<Eigen::Index>(b.cols.size());
            if (Cb.rows() != n || Cb.cols() != n) continue;
            std::vector<Eigen::Index> ri(static_cast<std::size_t>(n), Eigen::Index{-1});
            for (Eigen::Index a = 0; a < n; ++a) {
                const auto it = row_of.find({b.est_idx, b.cols[static_cast<std::size_t>(a)]});
                if (it != row_of.end()) ri[static_cast<std::size_t>(a)] = it->second;
            }
            for (Eigen::Index a = 0; a < n; ++a) {
                const Eigen::Index ia = ri[static_cast<std::size_t>(a)];
                if (ia < 0) continue;
                for (Eigen::Index c = a + 1; c < n; ++c) {
                    const Eigen::Index ic = ri[static_cast<std::size_t>(c)];
                    if (ic < 0) continue;
                    const double v = term(ia, ic);
                    if (!std::isfinite(v)) continue;
                    bool any = false;
                    if (std::isfinite(Cb(a, c))) { Cb(a, c) += v; any = true; }
                    if (std::isfinite(Cb(c, a))) { Cb(c, a) += v; any = true; }
                    if (any) ++pairs;
                }
            }
        }
        return pairs;
    };
    if (!X.ok) {
        // B1's per-station form: each row takes r_Y' Sigma r_Y, each F3 pair r_a' Sigma r_b,
        // and no history is held.
        ++C.compress_failed;
        const Eigen::MatrixXd RB = R * h.B;
        for (Eigen::Index i = 0; i < m; ++i) {
            const double add = RB.row(i).dot(R.row(i));
            if (std::isfinite(add) && add > 0.0) rows[static_cast<std::size_t>(i)].lc->var += add;
            rows[static_cast<std::size_t>(i)].lc->shx_coef.clear();
            ++C.rows_fallback;
        }
        C.f3_pairs += addPairs([&](Eigen::Index a, Eigen::Index b) { return RB.row(a).dot(R.row(b)); });
        S.valid = false;
        networkSharedHistoryProjectHoldRows(ctx, nullptr, now_s);   // amendment A1: per-row term
        return;
    }
    if (X.chol_fallback) ++C.chol_fallback;
    S.kt = X.kt;
    for (Eigen::Index i = 0; i < m; ++i) {
        LocalCarry& lc = *rows[static_cast<std::size_t>(i)].lc;
        if (X.kt > 0) {
            lc.shx_coef.resize(static_cast<std::size_t>(X.kt));
            for (int j = 0; j < X.kt; ++j) lc.shx_coef[static_cast<std::size_t>(j)] = X.coef(i, j);
            lc.shx_stash_s = now_s;
        } else {
            lc.shx_coef.clear();
        }
        const double add = X.rem.cols() > 0 ? X.rem.row(i).squaredNorm() : 0.0;
        if (std::isfinite(add) && add > 0.0) {
            lc.var += add;
            C.rem_var_sum += add;
        }
    }
    if (X.rem.cols() > 0)
        C.f3_pairs += addPairs([&](Eigen::Index a, Eigen::Index b) { return X.rem.row(a).dot(X.rem.row(b)); });
    C.rows_compressed += static_cast<long long>(m);
    C.kt_sum   += X.kt;
    C.kt_max    = std::max<long long>(C.kt_max, X.kt);
    C.rank_sum += X.rank;
    C.rank_max  = std::max<long long>(C.rank_max, X.rank);
    if (X.rank > cap) ++C.kt_capped;
    if (X.total > 0.0) {
        C.kept_share.push_back(X.kept / X.total);
        C.rem_share.push_back(X.remainder / X.total);
    }
    if (X.frob_total > 0.0) C.cross_share.push_back(X.cross / X.frob_total);
    // Amendment A1: card SMH's clone rows, pending since this epoch's update, projected onto
    // the kept directions of the compression just formed.
    networkSharedHistoryProjectHoldRows(ctx, &X, now_s);
}

namespace {

/// Every live hold row's shared-history record (card SMH's clock rows; card SMD's two rows).
template <typename Fn>
void shxForEachHoldRow(NetworkRunContext& ctx, Fn&& fn) {
    net::forEach(ctx.entities.sat_signals, &net::SatSignalState::smh_clone,   // N4d
                 [&](const SatSignalId&, PbHoldClone& c) { if (c.alive) fn(c.shx, c.var); });
    net::forEach(ctx.entities.sat_signals, &net::SatSignalState::smh_block,
                 [&](const SatSignalId&, PbHoldClone& c) {
                     if (c.alive) {
                         fn(c.shx, c.var);
                         fn(c.pv_shx, c.pv_var);
                     }
                 });
}

/// A pending hold row's per-row term (B1's form): its whole shared term on its own variance,
/// counted in @p n (the cause).
void shxHoldFallback(SharedHistoryCounters& C, PbHoldCloneShx& sh, double& var, long long& n) {
    if (std::isfinite(sh.rc_var) && sh.rc_var > 0.0) {
        var += sh.rc_var;
        C.hold_fallback_var += sh.rc_var;
    }
    sh.coef.clear();
    ++n;
}

/// Clear a row's pending record.
void shxHoldClearPending(PbHoldCloneShx& sh) {
    sh.rc.resize(0);
    sh.rc_var = 0.0;
    sh.rc_ep  = -1;
}

}  // namespace

void networkSharedHistoryProjectHoldRows(NetworkRunContext& ctx, const SharedHistoryCompression* X,
                                         double now_s) {
    if (!ctx.knobs.carry_cpl_shx || !ctx.knobs.emit_pb_hold_smoothed) return;
    SharedHistoryCounters& C = ctx.counters.shx;
    const long long ep = static_cast<long long>(ctx.counters.n_loop_epochs);
    shxForEachHoldRow(ctx, [&](PbHoldCloneShx& sh, double& var) {
        if (sh.rc_ep != ep) return;   // nothing pending from this epoch's update
        const bool formed = X && X->ok;
        SharedHistoryProjection P;
        if (formed) P = sharedHistoryProject(sh.rc, X->L, X->V, X->kt);
        if (!P.ok) {
            shxHoldFallback(C, sh, var, formed ? C.hold_proj_failed : C.hold_nocomp_end);
            shxHoldClearPending(sh);
            return;
        }
        sh.coef.assign(P.coef.data(), P.coef.data() + P.coef.size());
        sh.stash_s = now_s;
        if (std::isfinite(P.rem) && P.rem > 0.0) {
            var += P.rem;
            C.hold_rem_var += P.rem;
        }
        ++C.hold_projected;
        if (P.total > 0.0) {
            C.hold_kept_share.push_back(P.coef.squaredNorm() / P.total);
            C.hold_rem_share.push_back(P.rem / P.total);
        }
        shxHoldClearPending(sh);
    });
}

void networkSharedHistoryFinalizeHoldRows(NetworkRunContext& ctx, long long ep) {
    if (!ctx.knobs.carry_cpl_shx || !ctx.knobs.emit_pb_hold_smoothed) return;
    SharedHistoryCounters& C = ctx.counters.shx;
    shxForEachHoldRow(ctx, [&](PbHoldCloneShx& sh, double& var) {
        if (sh.rc_ep < 0 || sh.rc_ep > ep) return;
        shxHoldFallback(C, sh, var, C.hold_nocomp_unreached);
        shxHoldClearPending(sh);
    });
}

void networkSharedHistoryRunEnd(const NetworkRunContext& ctx,
                                  const std::shared_ptr<spdlog::logger>& log) {
    if (!ctx.knobs.carry_cpl_shx || !log) return;
    const SharedHistoryCounters& C = ctx.counters.shx;
    const CouplingExactCounters& X = ctx.counters.cxu;
    const auto ks = shxP50Max(C.kept_share);
    const auto rs = shxP50Max(C.rem_share);
    const auto cs = shxP50Max(C.cross_share);
    const double per = C.epochs > 0 ? 1.0 / static_cast<double>(C.epochs) : 0.0;
    const double pc  = C.compress_epochs > 0 ? 1.0 / static_cast<double>(C.compress_epochs) : 0.0;
    // Amendment A1 (build X2b): card SMH's clone rows in the history, form p, appended to the
    // line when the smoothed hold is armed.
    std::string hold;
    if (ctx.knobs.emit_pb_hold_smoothed) {
        const auto hk = shxP50Max(C.hold_kept_share);
        const auto hr = shxP50Max(C.hold_rem_share);
        hold = fmt::format(
            "  Hold rows (X2b, amendment A1): {} kept pending at the update, {} projected onto the "
            "kept directions (kept share p50 / max {:.3e} / {:.3e}, remainder share p50 / max "
            "{:.3e} / {:.3e}, remainder variance added {:.4e} m^2), {} projection failure(s) "
            "with a compression formed, {} at a harvest's end without a compression, {} in an "
            "epoch without a carry harvest (per-row term on those {:.4e} m^2), {} died pending "
            "(the row killed after its update); at the next update {} row(s) with history terms "
            "({} term(s)), {} marginalized on a stale generation or an unregistered column ({} "
            "term(s), {:.4e} m^2).",
            C.hold_pending, C.hold_projected, hk.first, hk.second, hr.first, hr.second,
            C.hold_rem_var, C.hold_proj_failed, C.hold_nocomp_end, C.hold_nocomp_unreached,
            C.hold_fallback_var,
            C.hold_pending - C.hold_projected - C.hold_proj_failed - C.hold_nocomp_end
                - C.hold_nocomp_unreached,
            C.hold_rows_hist, C.hold_terms_hist, C.hold_rows_stale, C.hold_terms_stale,
            C.hold_stale_var);
    }
    log->info("Network (card SCL2 / shared history X2, k = {}): {} epoch(s) with history columns, "
              "registered {:.1f} mean / {} max per epoch ({} unreferenced direction(s) not "
              "registered, {} generation mismatch(es), {} prior(s) folded); rows: {} with "
              "history terms ({} term(s)), {} marginalized ({} term(s), {:.4e} m^2); "
              "compression: {} epoch(s), {} failure(s), {} Cholesky fallback(s), {} epoch(s) "
              "without the regression; Y width {:.1f} mean; kt {:.1f} mean / {} max ({} "
              "epoch(s) rank-capped), rank {:.1f} mean / {} max; {} row(s) compressed, {} lost, "
              "{} unnoted, {} on B1's fallback; remainder variance added {:.4e} m^2, {} F3 "
              "pair(s); mass shares p50 / max: kept {:.3e} / {:.3e}, remainder {:.3e} / {:.3e}, "
              "cross-station dropped {:.3e} / {:.3e}.{}",
              ctx.knobs.shx_rank, C.epochs, static_cast<double>(C.registered) * per,
              C.registered_max, C.unreferenced, C.gen_mismatch, C.fold_priors, X.rows_hist,
              X.terms_hist, X.rows_hist_marg, X.terms_hist_marg, X.hist_marg_var,
              C.compress_epochs, C.compress_failed, C.chol_fallback, C.noreg_epochs,
              static_cast<double>(C.ny_sum) * pc, static_cast<double>(C.kt_sum) * pc, C.kt_max,
              C.kt_capped, static_cast<double>(C.rank_sum) * pc, C.rank_max, C.rows_compressed,
              C.rows_lost, C.rows_unnoted, C.rows_fallback, C.rem_var_sum, C.f3_pairs, ks.first,
              ks.second, rs.first, rs.second, cs.first, cs.second, hold);
    if (ctx.knobs.emit_pb_hold_smoothed) {
        if (C.hold_pending > 0 && C.hold_projected == 0)
            log->warn("Network (card SCL2 X2b): the smoothed hold's rows were kept pending but "
                      "none was projected: amendment A1 is INERT for this run (every row took "
                      "the per-row term).");
    }
    if (C.registered == 0 || X.terms_hist == 0)
        log->warn("Network (card SCL2): network.carry_coupling_shared_history is ARMED but {}: "
                  "the build is INERT for this run.",
                  C.registered == 0 ? "no history column was registered"
                                    : "no row emitted a history term");
}

}  // namespace GnssProcessor
