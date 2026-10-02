/**
 * @file test_coupling_exact_update.cpp
 * @brief Unit gate of card CTU (docs/refound/CTU_coupling_time_update_2026-09-28.md,
 *        section 9.2): the exact conditional time update of the carried coupling rows.
 *
 * The card 1d fixture (test_network_estimator.cpp, ConditionalCarryTimeUpdateRecursiveEqualsBatch)
 * carries clocks only, on one band, with no ionosphere and no phase-bias column, which is
 * why the scalar kappa shrink passed it.  This fixture has what the lock needs: three
 * satellites, each with a clock and two phase-bias columns; three stations with two-band
 * phase and code, a free slant ionosphere per line of sight and epoch, and carried float
 * ambiguities; a held datum (station 0's ambiguities pinned); a pinned clock pivot
 * (satellite 0); the anchor's random walk on the other clocks (2.9e-8 m^2/s over 30 s);
 * sixty epochs.
 *
 * Two recursions run over the same data.  The reference carries every station's
 * ambiguities as one joint local block, so the one rule plus the rows' full time-update
 * covariance make it an exact Kalman recursion: it must equal the batch (every epoch's
 * rows plus the walk pseudo-observations) to rounding.  The production form carries one
 * block per station and drops the cross-station time-update covariance (card section 4):
 * its level is reported against the batch.  A third recursion applies the card 1d scalar
 * kappa at the legacy clock rate, and must be seen to lock the level.
 */

#include <gtest/gtest.h>

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkClockAnchor.hpp"
#include "filter/NetworkCouplingExact.hpp"
#include "logging/LogManager.hpp"

#include <Eigen/Dense>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

// ---------------------------------------------------------------------------
// Small helpers for the pure-function cases.
// ---------------------------------------------------------------------------

/// A random symmetric positive-definite matrix with a spread of scales and strong
/// correlations (the shape of a border posterior: stiff phase directions, loose level).
Eigen::MatrixXd randomSpd(int n, std::mt19937& rng) {
    std::normal_distribution<double> nd(0.0, 1.0);
    Eigen::MatrixXd A(n, n);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) A(i, j) = nd(rng);
    Eigen::MatrixXd P = A * A.transpose() + 1e-3 * Eigen::MatrixXd::Identity(n, n);
    Eigen::VectorXd sc(n);
    for (int i = 0; i < n; ++i) sc(i) = std::pow(10.0, -1.5 * (i % 3));
    return sc.asDiagonal() * P * sc.asDiagonal();
}

/// An epoch over P rows 0..n-1: rows in `d` are D (with `route`), the rest S with the
/// given inflation, all present at columns 100+p.  Constellation 'G'; `clk` marks clocks.
CouplingExactEpoch makeEpoch(const Eigen::MatrixXd& P, const std::vector<double>& q,
                             const std::vector<int>& d, const std::vector<char>& clk,
                             CouplingExactDRoute route = CouplingExactDRoute::Departed) {
    CouplingExactEpoch ep;
    const int n = static_cast<int>(P.rows());
    ep.P = P;
    ep.s_of_p.assign(n, -1);
    ep.route_p.assign(n, static_cast<int>(CouplingExactDRoute::Present));
    ep.clk_p = clk;
    ep.cc_p.assign(n, 'G');
    std::vector<double> qs, sh;
    for (int p = 0; p < n; ++p) {
        if (std::find(d.begin(), d.end(), p) != d.end()) {
            ep.route_p[p] = static_cast<int>(route);
            continue;
        }
        ep.s_of_p[p] = static_cast<int>(ep.p_of_s.size());
        ep.p_of_s.push_back(p);
        ep.col_of_s.push_back(100 + p);
        qs.push_back(q[static_cast<std::size_t>(p)]);
        sh.push_back(0.1 * p);
    }
    ep.q_s = Eigen::Map<Eigen::VectorXd>(qs.data(), static_cast<Eigen::Index>(qs.size()));
    ep.shat_s = Eigen::Map<Eigen::VectorXd>(sh.data(), static_cast<Eigen::Index>(sh.size()));
    EXPECT_TRUE(couplingExactFactor(ep));
    return ep;
}

/// Brute-force B = P_KK - P_KS (P_SS + Q_S)^-1 P_SK over every P row.
Eigen::MatrixXd bruteB(const CouplingExactEpoch& ep) {
    const Eigen::Index n = ep.P.rows(), nS = ep.nS();
    Eigen::MatrixXd PKS(n, nS), PSS(nS, nS);
    for (Eigen::Index a = 0; a < n; ++a)
        for (Eigen::Index b = 0; b < nS; ++b) PKS(a, b) = ep.P(a, ep.p_of_s[b]);
    for (Eigen::Index a = 0; a < nS; ++a)
        for (Eigen::Index b = 0; b < nS; ++b) PSS(a, b) = ep.P(ep.p_of_s[a], ep.p_of_s[b]);
    const Eigen::MatrixXd F = (PSS + Eigen::MatrixXd(ep.q_s.asDiagonal())).inverse();
    return ep.P - PKS * F * PKS.transpose();
}

// ---------------------------------------------------------------------------
// The network fixture: truth, observations, batch and recursions.
// ---------------------------------------------------------------------------

constexpr int    kS      = 3;          // satellites; satellite 0's clock is the pinned pivot
constexpr int    kR      = 3;          // stations; station 0 holds the datum (ambiguities pinned)
constexpr double kDt     = 30.0;
constexpr double kQClk   = 2.9e-8;     // the anchor's clock walk (m^2/s)
constexpr double kSigPh  = 0.003;
constexpr double kSigCd  = 0.30;
const double     kAlpha[2] = {1.0, 1.6469444444444444};   // (f1/f2)^2 for GPS L1/L2

struct Scenario {
    int  K = 60;
    int  decouple_sat = -1, decouple_k = -1;     // no walk tie into this epoch (1-based)
    int  depart_sat = -1, depart_k0 = -1, depart_k1 = -1;   // absent over [k0, k1]
    unsigned seed = 7u;
};

/// Identity of one border column: a clock or a phase bias, with the segment it belongs
/// to (a clock's segment advances at a decoupling or a return, a phase bias's at a return).
struct BKey {
    bool clk{true};
    int  s{0}, j{0}, seg{0};
    bool operator<(const BKey& o) const {
        return std::tie(clk, s, j, seg) < std::tie(o.clk, o.s, o.j, o.seg);
    }
    bool operator==(const BKey& o) const {
        return clk == o.clk && s == o.s && j == o.j && seg == o.seg;
    }
};

struct Net {
    Scenario sc;
    // truth
    std::vector<std::array<double, kS>> c;                    // c[k][s], k = 1..K (index k)
    double p[kS][2]{};
    double N[kR][kS][2]{};
    std::vector<std::array<double, kR>> rx;
    std::vector<std::array<std::array<double, kS>, kR>> ion;
    // observations
    struct Obs { double ph[2]{}, cd[2]{}; };
    std::vector<std::array<std::array<Obs, kS>, kR>> obs;     // obs[k][r][s]

    bool present(int k, int s) const {
        return !(s == sc.depart_sat && k >= sc.depart_k0 && k <= sc.depart_k1);
    }
    int clkSeg(int k, int s) const {
        int seg = 0;
        if (s == sc.decouple_sat && k >= sc.decouple_k) ++seg;
        if (s == sc.depart_sat && k > sc.depart_k1) ++seg;
        return seg;
    }
    int pbSeg(int k, int s) const { return (s == sc.depart_sat && k > sc.depart_k1) ? 1 : 0; }
    /// The border columns present at epoch k (the pivot clock is not a column).
    std::vector<BKey> border(int k) const {
        std::vector<BKey> b;
        for (int s = 0; s < kS; ++s) {
            if (!present(k, s)) continue;
            if (s != 0) b.push_back({true, s, 0, clkSeg(k, s)});
            for (int j = 0; j < 2; ++j) b.push_back({false, s, j, pbSeg(k, s)});
        }
        return b;
    }
    /// Carried ambiguities: every (station 1.., satellite, signal), in a fixed order.
    static int nIdx(int r, int s, int j) { return ((r - 1) * kS + s) * 2 + j; }
    static constexpr int kN = (kR - 1) * kS * 2;
};

Net makeNet(const Scenario& sc) {
    Net n;
    n.sc = sc;
    std::mt19937 rng(sc.seed);
    std::normal_distribution<double> nd(0.0, 1.0);
    n.c.resize(static_cast<std::size_t>(sc.K + 1));
    n.rx.resize(static_cast<std::size_t>(sc.K + 1));
    n.ion.resize(static_cast<std::size_t>(sc.K + 1));
    n.obs.resize(static_cast<std::size_t>(sc.K + 1));
    for (int s = 0; s < kS; ++s) {
        n.c[1][s] = 0.8 * nd(rng);
        for (int j = 0; j < 2; ++j) n.p[s][j] = nd(rng);
        for (int r = 0; r < kR; ++r)
            for (int j = 0; j < 2; ++j) n.N[r][s][j] = 3.0 * nd(rng);
    }
    for (int k = 2; k <= sc.K; ++k)
        for (int s = 0; s < kS; ++s)
            n.c[k][s] = n.c[k - 1][s] + std::sqrt(kQClk * kDt) * nd(rng);
    for (int k = 1; k <= sc.K; ++k)
        for (int r = 0; r < kR; ++r) {
            n.rx[k][r] = 10.0 * nd(rng);
            for (int s = 0; s < kS; ++s) {
                n.ion[k][r][s] = 5.0 + nd(rng);
                auto& o = n.obs[k][r][s];
                for (int j = 0; j < 2; ++j) {
                    o.ph[j] = n.rx[k][r] - n.c[k][s] + n.p[s][j] + n.N[r][s][j]
                            - kAlpha[j] * n.ion[k][r][s] + kSigPh * nd(rng);
                    o.cd[j] = n.rx[k][r] - n.c[k][s] + kAlpha[j] * n.ion[k][r][s]
                            + kSigCd * nd(rng);
                }
            }
        }
    return n;
}

/// One epoch's observation rows over an unknown vector.  `idx(kind, ...)` maps a state
/// to its column (or -1 when it is known: the pivot clock, station 0's ambiguities).
/// Known terms are moved to the right-hand side with their true values.
template <class ColOf>
void addEpochObs(const Net& n, int k, const ColOf& col, Eigen::MatrixXd& L, Eigen::VectorXd& e) {
    for (int r = 0; r < kR; ++r)
        for (int s = 0; s < kS; ++s) {
            if (!n.present(k, s)) continue;
            const auto& o = n.obs[k][r][s];
            for (int type = 0; type < 2; ++type)          // 0 phase, 1 code
                for (int j = 0; j < 2; ++j) {
                    std::vector<std::pair<int, double>> a;
                    double y = (type == 0) ? o.ph[j] : o.cd[j];
                    a.emplace_back(col('r', r, s, j), 1.0);                       // receiver clock
                    if (s == 0) y += n.c[k][0];                                    // the pinned pivot
                    else a.emplace_back(col('c', r, s, j), -1.0);
                    a.emplace_back(col('i', r, s, j), (type == 0 ? -1.0 : 1.0) * kAlpha[j]);
                    if (type == 0) {
                        a.emplace_back(col('p', r, s, j), 1.0);
                        if (r == 0) y -= n.N[0][s][j];                             // the held datum
                        else a.emplace_back(col('n', r, s, j), 1.0);
                    }
                    const double w = 1.0 / std::pow(type == 0 ? kSigPh : kSigCd, 2);
                    for (const auto& [ci, ai] : a)
                        for (const auto& [cj, aj] : a) L(ci, cj) += w * ai * aj;
                    for (const auto& [ci, ai] : a) e(ci) += w * ai * y;
                }
        }
}

/// The batch: every epoch's observations and the walk ties, all at once.
struct BatchOut {
    std::map<BKey, int> col_of;       // final-epoch border columns in the batch
    Eigen::VectorXd x;
    Eigen::MatrixXd cov;
    std::vector<int> ncol;            // carried ambiguity -> batch column
};

/// Inverse of a symmetric positive-definite matrix through LDLT (more accurate than LU
/// on the stiff-and-loose information matrices of this fixture).
Eigen::MatrixXd spdInverse(const Eigen::MatrixXd& A) {
    const Eigen::LDLT<Eigen::MatrixXd> f(0.5 * (A + A.transpose()));
    return f.solve(Eigen::MatrixXd::Identity(A.rows(), A.cols()));
}

BatchOut runBatch(const Net& n) {
    const int K = n.sc.K;
    // Global columns: every clock epoch-state, every phase-bias segment, every ambiguity.
    // Each epoch's fresh locals (receiver clocks and slant ionosphere) are eliminated
    // epoch by epoch, which is exact because nothing else involves them.
    std::map<std::pair<int, int>, int> ccol;      // (k, s) -> column
    std::map<BKey, int> pcol;                     // phase-bias segment -> column
    int nc = 0;
    for (int k = 1; k <= K; ++k)
        for (int s = 1; s < kS; ++s)
            if (n.present(k, s)) ccol[{k, s}] = nc++;
    for (int k = 1; k <= K; ++k)
        for (int s = 0; s < kS; ++s)
            if (n.present(k, s))
                for (int j = 0; j < 2; ++j) {
                    const BKey key{false, s, j, n.pbSeg(k, s)};
                    if (!pcol.count(key)) pcol[key] = nc++;
                }
    const int n0 = nc;
    nc += Net::kN;
    const int nl = kR * (1 + kS);
    Eigen::MatrixXd L = Eigen::MatrixXd::Zero(nc, nc);
    Eigen::VectorXd e = Eigen::VectorXd::Zero(nc);
    for (int k = 1; k <= K; ++k) {
        const auto col = [&](char kind, int r, int s, int j) -> int {
            switch (kind) {
                case 'r': return nc + r * (1 + kS);
                case 'i': return nc + r * (1 + kS) + 1 + s;
                case 'c': return ccol.at({k, s});
                case 'p': return pcol.at(BKey{false, s, j, n.pbSeg(k, s)});
                case 'n': return n0 + Net::nIdx(r, s, j);
            }
            return -1;
        };
        Eigen::MatrixXd Lk = Eigen::MatrixXd::Zero(nc + nl, nc + nl);
        Eigen::VectorXd ek = Eigen::VectorXd::Zero(nc + nl);
        addEpochObs(n, k, col, Lk, ek);
        const Eigen::MatrixXd Lll_inv = spdInverse(Lk.bottomRightCorner(nl, nl));
        const Eigen::MatrixXd Lgl = Lk.topRightCorner(nc, nl);
        L += Lk.topLeftCorner(nc, nc) - Lgl * Lll_inv * Lgl.transpose();
        e += ek.head(nc) - Lgl * (Lll_inv * ek.tail(nl));
    }
    for (int k = 2; k <= K; ++k)
        for (int s = 1; s < kS; ++s) {
            if (!n.present(k, s) || !n.present(k - 1, s)) continue;
            if (n.clkSeg(k, s) != n.clkSeg(k - 1, s)) continue;   // decoupled: no tie
            const int a = ccol.at({k, s}), b = ccol.at({k - 1, s});
            const double w = 1.0 / (kQClk * kDt);
            L(a, a) += w; L(b, b) += w; L(a, b) -= w; L(b, a) -= w;
        }
    BatchOut out;
    out.cov = spdInverse(L);
    out.x = out.cov * e;
    for (const BKey& b : n.border(K))
        out.col_of[b] = b.clk ? ccol.at({K, b.s}) : pcol.at(b);
    for (int i = 0; i < Net::kN; ++i) out.ncol.push_back(n0 + i);
    return out;
}

/// The factorized recursion.  mode 0: the exact rule with one joint local block (the
/// reference); mode 1: the exact rule with one block per station, cross-station
/// time-update covariance dropped and the within-station one kept (the producer's form
/// with network.carry_coupling_exact_cross, card WSX); mode 2: card 1d's scalar kappa at the
/// legacy clock rate q = 1e-3 m^2/s (the lock); mode 3: one block per station with the
/// time-update addend on the diagonal only (the shipped form, the knob off).
struct RecOut {
    std::vector<BKey> keys;
    Eigen::VectorXd shat;
    Eigen::MatrixXd P;
    Eigen::VectorXd nhat;
    Eigen::MatrixXd ncov;
    double min_var_add{0.0};
};

RecOut runRecursion(const Net& n, int mode) {
    const int K = n.sc.K;
    const int nN = Net::kN;
    // Carried state after the previous epoch.
    std::vector<BKey> bprev;
    Eigen::VectorXd sprev;
    Eigen::MatrixXd Pprev;
    bool have = false;
    Eigen::VectorXd nhat;            // ambiguity posterior means (row values)
    Eigen::MatrixXd G;               // regression of the ambiguities on the border: N = nhat + G (s - shat) + e
    Eigen::MatrixXd C;               // conditional covariance
    RecOut out;
    out.min_var_add = 1e300;
    for (int k = 1; k <= K; ++k) {
        const std::vector<BKey> b = n.border(k);
        const int nb = static_cast<int>(b.size());
        std::map<BKey, int> bcol;
        for (int i = 0; i < nb; ++i) bcol[b[i]] = i;
        const int n0 = nb, loc0 = nb + nN, nx = loc0 + kR * (1 + kS);
        Eigen::MatrixXd L = Eigen::MatrixXd::Zero(nx, nx);
        Eigen::VectorXd e = Eigen::VectorXd::Zero(nx);
        if (have) {
            // The epoch: S = previous columns carried into this epoch (same identity), with
            // the walk's inflation on clocks; D = the rest (departed or decoupled).
            CouplingExactEpoch ep;
            const int np = static_cast<int>(bprev.size());
            ep.P = Pprev;
            ep.s_of_p.assign(np, -1);
            ep.route_p.assign(np, static_cast<int>(CouplingExactDRoute::Departed));
            ep.clk_p.assign(np, 0);
            ep.cc_p.assign(np, 'G');
            std::vector<double> qs, sh;
            std::vector<int> col_s;
            for (int p = 0; p < np; ++p) {
                ep.clk_p[p] = bprev[p].clk ? 1 : 0;
                const auto it = bcol.find(bprev[p]);
                if (it == bcol.end()) continue;
                ep.route_p[p] = static_cast<int>(CouplingExactDRoute::Present);
                ep.s_of_p[p] = static_cast<int>(ep.p_of_s.size());
                ep.p_of_s.push_back(p);
                ep.col_of_s.push_back(it->second);
                col_s.push_back(it->second);
                qs.push_back(bprev[p].clk ? kQClk * kDt : 0.0);
                sh.push_back(sprev(p));
            }
            const Eigen::Index nS = static_cast<Eigen::Index>(qs.size());
            ep.q_s = Eigen::Map<Eigen::VectorXd>(qs.data(), nS);
            ep.shat_s = Eigen::Map<Eigen::VectorXd>(sh.data(), nS);
            EXPECT_TRUE(couplingExactFactor(ep)) << "epoch " << k;
            // The border prior N(shat_S, P_SS + Q_S): the fold.
            if (nS > 0) {
                Eigen::MatrixXd PS(nS, nS);
                for (Eigen::Index a = 0; a < nS; ++a)
                    for (Eigen::Index c = 0; c < nS; ++c)
                        PS(a, c) = Pprev(ep.p_of_s[a], ep.p_of_s[c]);
                PS += Eigen::MatrixXd(ep.q_s.asDiagonal());
                const Eigen::MatrixXd Lam = spdInverse(PS);
                for (Eigen::Index a = 0; a < nS; ++a) {
                    for (Eigen::Index c = 0; c < nS; ++c) L(col_s[a], col_s[c]) += Lam(a, c);
                    e(col_s[a]) += Lam.row(a).dot(ep.shat_s);
                }
            }
            // The carried rows.  Production convention: N_i + M_i's = nhat_i + M_i'shat
            // with M = -G.
            Eigen::MatrixXd A = Eigen::MatrixXd::Zero(nN, nx);
            Eigen::VectorXd y(nN);
            Eigen::MatrixXd Sig = C;
            if (mode == 2) {
                for (int i = 0; i < nN; ++i) {
                    A(i, n0 + i) = 1.0;
                    y(i) = nhat(i);
                    for (int p = 0; p < np; ++p) {
                        const double m = -G(i, p);
                        if (m == 0.0) continue;
                        const int s = ep.s_of_p[p];
                        if (s < 0) { Sig(i, i) += m * m * Pprev(p, p); continue; }   // departed
                        const double q = bprev[p].clk ? 1e-3 * kDt : 0.0;          // legacy rate
                        const double kap = (q > 0.0) ? Pprev(p, p) / (Pprev(p, p) + q) : 1.0;
                        Sig(i, i) += m * m * kap * q;
                        A(i, col_s[s]) += kap * m;
                        y(i) += kap * m * sprev(p);
                    }
                }
            } else {
                std::vector<CouplingExactRow> xr(static_cast<std::size_t>(nN));
                for (int i = 0; i < nN; ++i) {
                    std::vector<std::pair<int, double>> terms;
                    for (int p = 0; p < np; ++p)
                        if (G(i, p) != 0.0) terms.emplace_back(p, -G(i, p));
                    xr[i] = couplingExactRow(ep, terms);
                    EXPECT_TRUE(xr[i].ok);
                    out.min_var_add = std::min(out.min_var_add, xr[i].var_raw);
                    A(i, n0 + i) = 1.0;
                    y(i) = nhat(i);
                    for (Eigen::Index s = 0; s < nS; ++s) {
                        A(i, col_s[s]) += xr[i].m_new(s);
                        y(i) += xr[i].m_new(s) * ep.shat_s(s);
                    }
                }
                for (int i = 0; i < nN; ++i)
                    for (int j = 0; j < nN; ++j) {
                        const bool same_sta = (i / (kS * 2)) == (j / (kS * 2));
                        if ((mode == 1 || mode == 3) && !same_sta) continue;
                        if (mode == 3 && i != j) continue;   // the shipped form: the diagonal only
                        Sig(i, j) += couplingExactCross(ep, xr[i], xr[j]);
                    }
            }
            const Eigen::MatrixXd W = spdInverse(Sig);
            L += A.transpose() * W * A;
            e += A.transpose() * W * y;
        }
        const auto col = [&](char kind, int r, int s, int j) -> int {
            switch (kind) {
                case 'r': return loc0 + r * (1 + kS);
                case 'i': return loc0 + r * (1 + kS) + 1 + s;
                case 'c': return bcol.at(BKey{true, s, 0, n.clkSeg(k, s)});
                case 'p': return bcol.at(BKey{false, s, j, n.pbSeg(k, s)});
                case 'n': return n0 + Net::nIdx(r, s, j);
            }
            return -1;
        };
        addEpochObs(n, k, col, L, e);
        // An ambiguity never observed yet (epoch 1) has no row: it is fixed by its phase.
        const Eigen::MatrixXd Sx = spdInverse(L);
        const Eigen::VectorXd x = Sx * e;
        // Harvest: the border marginal and the ambiguities' conditional on it.
        bprev = b;
        sprev = x.head(nb);
        Pprev = Sx.topLeftCorner(nb, nb);
        const Eigen::MatrixXd Snb = Sx.block(n0, 0, nN, nb);
        const Eigen::MatrixXd Pinv = spdInverse(Pprev);
        G = Snb * Pinv;
        C = Sx.block(n0, n0, nN, nN) - Snb * Pinv * Snb.transpose();
        if (mode == 1 || mode == 3)   // one block per station: conditionally independent given the border
            for (int i = 0; i < nN; ++i)
                for (int j = 0; j < nN; ++j)
                    if ((i / (kS * 2)) != (j / (kS * 2))) C(i, j) = 0.0;
        nhat = x.segment(n0, nN);
        have = true;
        if (k == K) {
            out.keys = b;
            out.shat = sprev;
            out.P = Pprev;
            out.nhat = nhat;
            out.ncov = Sx.block(n0, n0, nN, nN);
        }
    }
    return out;
}

/// The level of satellite s at the final epoch: its clock against the pivot.  Returns
/// (value error of rec against the batch, rec variance / batch variance).
std::pair<double, double> levelGap(const Net& n, const BatchOut& bt, const RecOut& rc, int s) {
    const BKey key{true, s, 0, n.clkSeg(n.sc.K, s)};
    int ri = -1;
    for (int i = 0; i < static_cast<int>(rc.keys.size()); ++i)
        if (rc.keys[i] == key) ri = i;
    const int bi = bt.col_of.at(key);
    return {rc.shat(ri) - bt.x(bi), rc.P(ri, ri) / bt.cov(bi, bi)};
}

void expectEqualsBatch(const Net& n, const BatchOut& bt, const RecOut& rc) {
    ASSERT_EQ(rc.keys.size(), bt.col_of.size());
    for (int a = 0; a < static_cast<int>(rc.keys.size()); ++a) {
        const int ba = bt.col_of.at(rc.keys[a]);
        EXPECT_NEAR(rc.shat(a), bt.x(ba), 1e-8) << "border value " << a;
        for (int c = 0; c < static_cast<int>(rc.keys.size()); ++c) {
            const int bc = bt.col_of.at(rc.keys[c]);
            EXPECT_NEAR(rc.P(a, c), bt.cov(ba, bc), 1e-8 * std::abs(bt.cov(ba, ba)) + 1e-14)
                << "border covariance " << a << "," << c;
        }
    }
    // The ambiguities are held to 1e-7 m rather than 1e-8: measured, every L2 ambiguity of
    // a station differs from the batch by the same ~1.1e-8 m, a common per-station L2
    // offset (a code-determined direction, where sixty epochs of inversions accumulate
    // rounding at cond x eps); the border and every variance meet 1e-8.
    for (int i = 0; i < Net::kN; ++i) {
        EXPECT_NEAR(rc.nhat(i), bt.x(bt.ncol[i]), 1e-7) << "ambiguity " << i;
        EXPECT_NEAR(rc.ncov(i, i), bt.cov(bt.ncol[i], bt.ncol[i]),
                    1e-8 * bt.cov(bt.ncol[i], bt.ncol[i]))
            << "ambiguity variance " << i;
    }
    (void)n;
}

// Config helper for the fold-mirror case (the anchor armed as on the goldens).
struct Loaded {
    LogManager log;
    GnssConfig cfg;
    fs::path   path;
    explicit Loaded(const std::string& tag) {
        const std::string body =
            "{\n"
            "  \"processing\": { \"mode\": \"ppp-rtk-network\", \"active_systems\": [ \"GPS\" ],\n"
            "                    \"signal_priorities\": { \"GPS\": \"WC\" } },\n"
            "  \"estimation\": { \"estimate_ifb\": true },\n"
            "  \"filter\": { \"process_noise\": { \"sat_clock_q\": 1e-3 } },\n"
            "  \"network\": {\n"
            "    \"clock_datum_station\": \"AAAA\",\n"
            "    \"clock_temporal_anchor\": true,\n"
            "    \"clock_anchor_q\": 2.9e-8,\n"
            "    \"clock_anchor_var_floor_m2\": 1e-5,\n"
            "    \"clock_anchor_max_gap_s\": 120.0,\n"
            "    \"stations\": [\n"
            "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
            "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
            "    ]\n"
            "  }\n"
            "}\n";
        path = fs::temp_directory_path() / ("ctu_cfg_" + tag + ".json");
        std::ofstream f(path);
        f << body;
        f.close();
        EXPECT_TRUE(cfg.loadFromJson(path, log));
    }
    ~Loaded() { std::error_code ec; fs::remove(path, ec); }
};

}  // namespace

// ---------------------------------------------------------------------------
// The one rule on its own.
// ---------------------------------------------------------------------------

// q = 0 and nothing decoupled: the moved border is the old one, and the row must come back
// unchanged, with no inflation.
TEST(CouplingExactUpdateTest, ZeroNoiseReproducesTheRawRow) {
    std::mt19937 rng(11u);
    const Eigen::MatrixXd P = randomSpd(9, rng);
    const std::vector<double> q(9, 0.0);
    const std::vector<char> clk{1, 0, 0, 1, 0, 0, 1, 0, 0};
    const CouplingExactEpoch ep = makeEpoch(P, q, {}, clk);
    const std::vector<std::pair<int, double>> terms{{0, -1.7}, {1, 1.0}, {3, 0.35}, {6, 0.35},
                                                    {8, -0.2}};
    const CouplingExactRow r = couplingExactRow(ep, terms);
    ASSERT_TRUE(r.ok);
    for (const auto& [p, c] : terms)
        EXPECT_NEAR(r.m_new(ep.s_of_p[p]), c, 1e-13 * std::abs(c) + 1e-13);
    for (Eigen::Index s = 0; s < ep.nS(); ++s) {
        bool own = false;
        for (const auto& t : terms) own = own || ep.s_of_p[t.first] == s;
        if (!own) EXPECT_NEAR(r.m_new(s), 0.0, 1e-12);
    }
    EXPECT_NEAR(r.var_raw, 0.0, 1e-13);
}

// A diagonal P with nothing decoupled is card 1d's kappa, column by column.
TEST(CouplingExactUpdateTest, DiagonalBorderIsTheKappaShrink) {
    Eigen::MatrixXd P = Eigen::MatrixXd::Zero(4, 4);
    P.diagonal() << 1e-5, 2e-3, 4e-6, 1e-9;
    const std::vector<double> q{8.7e-7, 3e-2, 0.0, 3e-11};
    const CouplingExactEpoch ep = makeEpoch(P, q, {}, {1, 1, 0, 0});
    const std::vector<std::pair<int, double>> terms{{0, -1.7}, {1, 0.4}, {2, 1.0}, {3, -0.3}};
    const CouplingExactRow r = couplingExactRow(ep, terms);
    ASSERT_TRUE(r.ok);
    double var = 0.0;
    for (const auto& [p, c] : terms) {
        const double kap = P(p, p) / (P(p, p) + q[p]);
        EXPECT_NEAR(r.m_new(ep.s_of_p[p]), kap * c, 1e-12 * std::abs(c));
        var += c * c * kap * q[p];
    }
    EXPECT_NEAR(r.var_raw, var, 1e-12 * var);
}

// The per-row inflation is the exact marginal of the row (never below it), and the
// within-station cross terms are the off-diagonals of the same M'BM.
TEST(CouplingExactUpdateTest, InflationAndCrossTermsAreTheExactCovariance) {
    std::mt19937 rng(23u);
    const int n = 12;
    const Eigen::MatrixXd P = randomSpd(n, rng);
    std::vector<double> q(n, 0.0);
    std::vector<char> clk(n, 0);
    for (int p = 0; p < n; p += 3) { q[p] = 8.7e-7 * (1 + p); clk[p] = 1; }
    for (const auto route : {CouplingExactDRoute::Departed, CouplingExactDRoute::Reinit}) {
        const CouplingExactEpoch ep = makeEpoch(P, q, {3, 7}, clk, route);
        const Eigen::MatrixXd B = bruteB(ep);
        std::uniform_real_distribution<double> ud(-2.0, 2.0);
        std::vector<Eigen::VectorXd> M;
        std::vector<CouplingExactRow> rows;
        for (int k = 0; k < 5; ++k) {
            Eigen::VectorXd m = Eigen::VectorXd::Zero(n);
            std::vector<std::pair<int, double>> terms;
            for (int p = 0; p < n; ++p)
                if ((p + k) % 2 == 0 || p == 3 || p == 7) {
                    m(p) = ud(rng);
                    terms.emplace_back(p, m(p));
                }
            M.push_back(m);
            rows.push_back(couplingExactRow(ep, terms));
            ASSERT_TRUE(rows.back().ok);
            EXPECT_EQ(rows.back().n_d_route[static_cast<int>(route)], 2);
        }
        for (int a = 0; a < 5; ++a) {
            const double ex = M[a].dot(B * M[a]);
            EXPECT_NEAR(rows[a].var_raw, ex, 1e-10 * std::abs(ex) + 1e-18);
            EXPECT_GE(rows[a].var_add, ex - 1e-10 * std::abs(ex) - 1e-18);
            for (int b = 0; b < 5; ++b) {
                const double eb = M[a].dot(B * M[b]);
                EXPECT_NEAR(couplingExactCross(ep, rows[a], rows[b]), eb,
                            1e-10 * std::sqrt(std::abs(ex) * std::abs(M[b].dot(B * M[b])))
                                + 1e-18);
            }
        }
    }
}

// The pivot takes the clock-family deficit: the harvested clock coefficients over the
// stashed terms minus the new ones over S.  Added to the pinned pivot, the row's clock
// sum (its identity along the declared clock gauge) is exactly the harvest's.
TEST(CouplingExactUpdateTest, ClockDeficitKeepsTheGaugeSum) {
    std::mt19937 rng(5u);
    const Eigen::MatrixXd P = randomSpd(9, rng);
    const std::vector<double> q{8.7e-7, 0.0, 0.0, 8.7e-7, 0.0, 0.0, 3e-2, 0.0, 0.0};
    const std::vector<char> clk{1, 0, 0, 1, 0, 0, 1, 0, 0};
    const CouplingExactEpoch ep = makeEpoch(P, q, {6}, clk, CouplingExactDRoute::Departed);
    // A row whose non-pivot clocks sum to -g (the pivot, pinned outside P, carries +g).
    const double pivot = 0.25;
    const std::vector<std::pair<int, double>> terms{{0, -1.9}, {3, 0.9}, {6, 0.75}, {1, 1.0},
                                                    {5, -0.4}};
    const CouplingExactRow r = couplingExactRow(ep, terms);
    ASSERT_TRUE(r.ok);
    double sum_new = pivot + r.clk_deficit.at('G');
    for (Eigen::Index s = 0; s < ep.nS(); ++s)
        if (clk[ep.p_of_s[s]]) sum_new += r.m_new(s);
    EXPECT_NEAR(sum_new, pivot - 1.9 + 0.9 + 0.75, 1e-13);
}

// ---------------------------------------------------------------------------
// The network fixture.
// ---------------------------------------------------------------------------

// The reference recursion (the one rule, every station's rows as one block) is a Kalman
// recursion and equals the batch: the level, every border column, every ambiguity.
TEST(CouplingExactUpdateTest, ExactRecursionEqualsTheBatch) {
    const Net n = makeNet(Scenario{});
    const BatchOut bt = runBatch(n);
    const RecOut rc = runRecursion(n, 0);
    expectEqualsBatch(n, bt, rc);
    EXPECT_GE(rc.min_var_add, -1e-15) << "a row's inflation below zero beyond rounding";
}

// A clock decoupled at epoch 30 (no walk tie; the fold carries nothing on it) and a
// satellite departed for ten epochs (its columns leave; they return fresh): the rows'
// terms on those columns are re-attributed through P, and the recursion still equals
// the batch.
TEST(CouplingExactUpdateTest, DecoupledAndDepartedColumnsEqualTheBatch) {
    Scenario dec;
    dec.decouple_sat = 2;
    dec.decouple_k = 30;
    const Net nd = makeNet(dec);
    expectEqualsBatch(nd, runBatch(nd), runRecursion(nd, 0));
    Scenario dep;
    dep.depart_sat = 1;
    dep.depart_k0 = 40;
    dep.depart_k1 = 49;
    const Net np = makeNet(dep);
    expectEqualsBatch(np, runBatch(np), runRecursion(np, 0));
}

// The card 1d scalar kappa at the legacy clock rate states the level at phase precision:
// its claimed level variance collapses far below the batch's, and its level sits many of
// its own claimed sigmas from the batch's (one realization: the distance in batch sigmas
// is a draw of the first epochs' code, so the robust signatures are the claimed variance
// and the error against it).  The exact rule in a per-station form (mode 1, with the
// within-station cross terms of card WSX's knob; the shipped form, mode 3, is read in
// WithinStationCrossTermsAgainstTheBatch) stays with the batch: card section 4's claim that
// the dropped cross-station term leaves the level untouched, measured.
TEST(CouplingExactUpdateTest, ShrunkFormLocksTheLevelAndTheExactFormDoesNot) {
    const Net n = makeNet(Scenario{});
    const BatchOut bt = runBatch(n);
    const RecOut shrunk = runRecursion(n, 2);
    const RecOut perst  = runRecursion(n, 1);
    double worst_ratio = 1e300, worst_err_own = 0.0;
    for (int s = 1; s < kS; ++s) {
        const auto [err, ratio] = levelGap(n, bt, shrunk, s);
        const int bi = bt.col_of.at(BKey{true, s, 0, 0});
        const double own = std::sqrt(ratio * bt.cov(bi, bi));   // the shrunk form's claimed sigma
        worst_ratio   = std::min(worst_ratio, ratio);
        worst_err_own = std::max(worst_err_own, std::abs(err) / own);
        std::printf("[CTU] sat %d  shrunk level: err %.3e m against the batch (batch sigma "
                    "%.3e m, claimed %.3e m), variance ratio %.3e, truth error %.3e m vs the "
                    "batch's %.3e m\n", s, err, std::sqrt(bt.cov(bi, bi)), own, ratio,
                    bt.x(bi) + err - n.c[n.sc.K][s], bt.x(bi) - n.c[n.sc.K][s]);
    }
    EXPECT_LT(worst_ratio, 0.05) << "the shrunk form should claim the level far too tightly";
    EXPECT_GT(worst_err_own, 3.0) << "the shrunk form's level should sit many of its own "
                                     "claimed sigmas from the batch's";
    for (int s = 1; s < kS; ++s) {
        const auto [err, ratio] = levelGap(n, bt, perst, s);
        const int bi = bt.col_of.at(BKey{true, s, 0, 0});
        const double sig = std::sqrt(bt.cov(bi, bi));
        // Dropping the cross-station walk covariance counts the walk about n times: the
        // per-station form may be slightly tight, never by an order of magnitude, and its
        // level stays within a fraction of the batch sigma.
        EXPECT_GT(ratio, 0.5) << "sat " << s;
        EXPECT_LT(std::abs(err), 0.5 * sig) << "sat " << s;
        std::printf("[CTU] sat %d  per-station level: err %.3e m (batch sigma %.3e m), "
                    "variance ratio %.4f\n", s, err, sig, ratio);
    }
}

// Card WSX: the shipped form (mode 3, the time-update addend on the diagonal only) beside
// the form with the within-station cross terms (mode 1), both against the exact batch.  Per
// satellite the level's variance ratio and error; over the whole border the extreme
// generalized eigenvalues of the recursion's covariance against the batch's (below 1: a
// direction the recursion states tighter than the batch).
TEST(CouplingExactUpdateTest, WithinStationCrossTermsAgainstTheBatch) {
    const Net n = makeNet(Scenario{});
    const BatchOut bt = runBatch(n);
    for (const int mode : {1, 3}) {
        const RecOut rc = runRecursion(n, mode);
        double rmin = 1e300, rmax = 0.0, emax = 0.0;
        for (int s = 1; s < kS; ++s) {
            const auto [err, ratio] = levelGap(n, bt, rc, s);
            const int bi = bt.col_of.at(BKey{true, s, 0, n.clkSeg(n.sc.K, s)});
            const double sig = std::sqrt(bt.cov(bi, bi));
            rmin = std::min(rmin, ratio);
            rmax = std::max(rmax, ratio);
            emax = std::max(emax, std::abs(err) / sig);
            std::printf("[WSX] mode %d sat %d  level: err %.3e m (batch sigma %.3e m), variance "
                        "ratio %.4f\n", mode, s, err, sig, ratio);
        }
        const Eigen::Index nb = static_cast<Eigen::Index>(rc.keys.size());
        Eigen::MatrixXd Pb(nb, nb);
        for (Eigen::Index a = 0; a < nb; ++a)
            for (Eigen::Index c = 0; c < nb; ++c)
                Pb(a, c) = bt.cov(bt.col_of.at(rc.keys[static_cast<std::size_t>(a)]),
                                  bt.col_of.at(rc.keys[static_cast<std::size_t>(c)]));
        Eigen::GeneralizedSelfAdjointEigenSolver<Eigen::MatrixXd> ge(rc.P, Pb);
        ASSERT_EQ(ge.info(), Eigen::Success);
        std::printf("[WSX] mode %d  level variance ratio min %.4f max %.4f, worst |err| %.3f "
                    "batch sigma; border generalized eigenvalues min %.4f max %.4f\n", mode,
                    rmin, rmax, emax, ge.eigenvalues().minCoeff(), ge.eigenvalues().maxCoeff());
        // The extreme directions, normalized in the batch metric (v' Pb v = 1): the keys with
        // the largest loadings (clock or phase bias, satellite, band).
        for (const Eigen::Index e : {Eigen::Index{0}, nb - 1}) {
            const Eigen::VectorXd v = ge.eigenvectors().col(e);
            std::vector<std::pair<double, Eigen::Index>> ld;
            for (Eigen::Index a = 0; a < nb; ++a) ld.emplace_back(std::abs(v(a)), a);
            std::sort(ld.rbegin(), ld.rend());
            std::printf("[WSX] mode %d  eigenvalue %.4f, largest loadings:", mode, ge.eigenvalues()(e));
            for (std::size_t t = 0; t < std::min<std::size_t>(6, ld.size()); ++t) {
                const BKey& k = rc.keys[static_cast<std::size_t>(ld[t].second)];
                std::printf(" %s s%d%s %+.3f", k.clk ? "clk" : "pb", k.s,
                            k.clk ? "" : (k.j == 0 ? " b1" : " b2"), v(ld[t].second));
            }
            std::printf("\n");
        }
        // Measured (card WSX section 6): both forms keep the level with the batch (variance
        // ratio 1.0000 / 1.0001); the shipped form states no border direction tighter than
        // the batch (eigenvalues 1.0000 to 1.163, the loosest the pivot satellite's
        // phase-visible clock against the others'); the within-station half alone states one
        // tighter (0.9445, the between-satellite contrast of the phase-visible clock D).
        // These bounds hold for this three-station network only.  With kR raised by hand the
        // smallest eigenvalue falls with the stations per satellite (the dropped cross-station
        // half): shipped form 0.885 / 0.661 / 0.336 and with the within-station half 0.693 /
        // 0.518 / 0.322 at 8 / 16 / 48 stations, always along a between-satellite
        // phase-visible contrast, the level staying with the batch (card WSX section 6).
        EXPECT_GT(rmin, 0.5) << "mode " << mode;
        EXPECT_LT(rmax, 2.0) << "mode " << mode;
        EXPECT_LT(emax, 0.5) << "mode " << mode;
        if (mode == 3)
            EXPECT_GT(ge.eigenvalues().minCoeff(), 1.0 - 1e-6)
                << "the shipped form states a border direction tighter than the batch";
        else
            EXPECT_LT(ge.eigenvalues().minCoeff(), 1.0 - 1e-3)
                << "the within-station half alone no longer over-states any direction";
    }
}

// The rows are built before the fold runs, so the exact update reads the anchor's
// decision, made before PASS B without side effects (card PRM phase 1b, which replaced card
// CTU's twin of the fold with it).  The decision must be what the fold applies, in every
// state the fold distinguishes, and must not advance the state.
TEST(CouplingExactUpdateTest, AnchorDecisionIsTheFoldWithoutItsSideEffects) {
    Loaded L("peek");
    NetworkRunContext ctx(L.cfg, L.cfg.getNetworkConfig());
    ASSERT_TRUE(ctx.knobs.cta_on);
    const auto both = [&](const std::string& sat, double var, double dt, double now,
                          double seed) {
        const ClockAnchorSat before = ctx.carry.clk_anchor.sat.count(sat)
                                          ? ctx.carry.clk_anchor.sat[sat] : ClockAnchorSat{};
        const ClockAnchorFold pk = clockAnchorFoldDecide(ctx, sat, var, dt, now, seed);
        const ClockAnchorSat after_peek = ctx.carry.clk_anchor.sat.count(sat)
                                              ? ctx.carry.clk_anchor.sat[sat] : ClockAnchorSat{};
        EXPECT_EQ(after_peek.t_last, before.t_last);
        EXPECT_EQ(after_peek.anchored, before.anchored);
        clockAnchorBeginEpoch(ctx);
        const ClockAnchorFold fd = clockAnchorFoldVariance(ctx, sat, var, dt, now, 0.1, seed);
        EXPECT_EQ(pk.var_eff, fd.var_eff) << sat << " at " << now;   // bit equality
        EXPECT_EQ(pk.anchored, fd.anchored);
        EXPECT_EQ(pk.floor_hit, fd.floor_hit);
        EXPECT_EQ(pk.decoupled, fd.decoupled);
    };
    both("G01", 1e-3, 30.0, 1000.0, 0.0);                    // first sighting: LOOSE
    ctx.carry.clk_anchor.sat["G01"].anchored = true;
    both("G01", 1e-3, 30.0, 1030.0, 0.0);                    // anchored
    both("G01", 1e-7, 30.0, 1060.0, 0.0);                    // anchored, at the floor
    both("G01", 1e-3, 30.0, 1400.0, 0.0);                    // anchored and stale: decoupled
    ctx.carry.clk_anchor.sat["G01"].anchored = true;
    both("G01", 1e-3, 30.0, 4900.0, 3900.0);                 // a re-admission seed
    both("G02", 2e-3, 0.0, 5000.0, 0.0);                     // no elapsed time
}
