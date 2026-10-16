/**
 * @file test_coupling_long_horizon.cpp
 * @brief Gate U1 of the level-residue study (docs/refound/SCL2_level_residue_under_the_clone_2026-10-06.md,
 *        sections 3.6, 4, 5 and 6): the satellite clock level of the coupling recursions at
 *        long horizons, in the unit network of test_coupling_exact_update.cpp at runtime size.
 *
 * The network: R stations, S satellites (satellite 0 the clock pivot), two-band phase and code
 * per line of sight, a receiver clock and a slant ionosphere per station and epoch (eliminated
 * per station, exactly), two phase-bias columns per satellite, float ambiguities carried at
 * every station except the satellite's phase-datum anchor (satellite s anchored at station
 * s mod R, the rotated layout of section 3.6 of that document), and the non-pivot clocks a random
 * walk at q.  The batch is an exact Kalman filter over [border, every carried ambiguity].
 *
 * The recursions (the fixture's algebra):
 *   - mode 3: the shipped per-row form (production couplingExactRow, one harvest block per
 *     station);
 *   - mode 4: the one-epoch clock clone with the joint harvest (exact by construction; X1);
 *   - mode 5: the clone with the per-station harvest (the clock clone's B1 as shipped);
 *   - mode 6: X2, the shared history.  After the solve, with Y = [clones; history columns],
 *     F_full = Cov(N, Y | s) Sigma_Y|s^(-1/2) holds the rows' shared conditional term and
 *     D0 = Cov(N | s, Y) is block diagonal by station.  The leading right singular directions
 *     of F_full, up to a cap of k columns per cloned clock, become the next epoch's history
 *     columns (prior N(0, I), independent of the border); the remainder's per-station blocks
 *     join D0 and its cross-station blocks are dropped.  Exact when the cap is not binding.
 *
 * Two geometries.  The static one repeats the same rows and weights every epoch; there the
 * carried ambiguities depend on the past clocks only through their sum, so the shared term
 * has rank nclk and one history column per clock is exact by construction.  The time-varying
 * one scales every row of satellite s at station r by 1 / sin(e_rs(k)) with e_rs sweeping
 * 15 to 75 degrees over a 12 h cycle (the anchors and the row structure unchanged), which
 * lets the shared term's rank grow and makes the cap a real truncation.
 *
 * The cases: U1 (a) mode 5 states the level too tightly at the broadcast-grade walk while
 * mode 4 equals the batch; (b) mode 6 with no cap equals the batch; (c) mode 6 at gate U2's
 * selected cap keeps the stated level within 5 % of the batch's on the time-varying
 * geometry; and the static geometry's rank property, so that a pass on the static fixture is
 * never read as evidence for the cap.
 *
 * Gate U4 (amendment A1, build X2b): the smoothed hold's row, the fixed-point
 * target c_s(k0) carried with no observations, at R 8 and S 6.  Under mode 6 at the selected
 * cap, the row projected onto the carried rows' kept history directions ("form p") gives the
 * walk share Phi = c(t|t) - c(k0|t) of the batch's fixed-point smoother; the per-row form (its
 * shared term marginalized onto its own variance) loses the history's revision and its walk
 * share error is many times the batch's.
 */

#include <gtest/gtest.h>

#include "network/carry/CouplingExact.hpp"

#include <Eigen/Dense>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace GnssProcessor;

namespace {

constexpr double kDt         = 30.0;
constexpr double kSigPh      = 0.003;
constexpr double kSigCd      = 0.30;
constexpr double kQBroadcast = 5e-6;     ///< m^2/s, the broadcast-grade anchor walk
constexpr double kQGolden    = 2.9e-8;   ///< m^2/s, the golden posture's anchor walk
constexpr double kPi         = 3.14159265358979323846;
constexpr int    kNoCap      = std::numeric_limits<int>::max();
const double     kAlpha[2]   = {1.0, 1.6469444444444444};

/// Gate U2's selection: the history columns kept per cloned clock.  The
/// fixture grid of 2026-10-06 passed every k from 1 up; the decision is recorded in the design note.
constexpr int kSelectedHistoryPerClock = 1;

Eigen::MatrixXd spdInverse(const Eigen::MatrixXd& A) {
    if (A.rows() == 0) return Eigen::MatrixXd(0, 0);
    const Eigen::LDLT<Eigen::MatrixXd> f(0.5 * (A + A.transpose()));
    return f.solve(Eigen::MatrixXd::Identity(A.rows(), A.cols()));
}

/**
 * @brief The unit network: index maps and each station's information reduced onto
 *        [border, its carried ambiguities] at the current epoch.
 *
 * Border columns: the nclk = S - 1 non-pivot clocks, then two phase-bias columns per
 * satellite (nb = nclk + 2 S).  Carried ambiguities are numbered satellite-major.
 */
struct UnitNetwork {
    int R{3}, S{3}, nclk{2}, nb{8}, nN{0};
    bool tv{false};
    std::vector<int> staOf;                  ///< carried ambiguity -> station
    std::vector<std::vector<int>> nlist;     ///< station -> its carried ambiguities
    std::vector<Eigen::MatrixXd> Hg0, Hl0;   ///< per station: designs on [border, its N] and the locals
    std::vector<Eigen::MatrixXd> Lam, T, THc;
    std::vector<Eigen::VectorXd> sigr;       ///< per station: per-row sigma at the current epoch
    Eigen::VectorXd sig;                     ///< per-row base sigma
    std::vector<double> phi;                 ///< per (station, satellite): elevation phase

    int m(int r) const { return static_cast<int>(nlist[r].size()); }

    /**
     * @brief Builds the network.
     * @param R_       Station count.
     * @param S_       Satellite count (satellite 0 is the clock pivot).
     * @param rotated  true: satellite s anchored at station s mod R; false: every satellite at station 0.
     * @param tv_      true: the time-varying elevation weights (see setEpoch).
     */
    UnitNetwork(int R_, int S_, bool rotated, bool tv_)
        : R(R_), S(S_), nclk(S_ - 1), nb(S_ - 1 + 2 * S_), tv(tv_) {
        std::vector<int> anchor(S);
        for (int s = 0; s < S; ++s) anchor[s] = rotated ? s % R : 0;
        std::vector<std::vector<std::array<int, 2>>> nIdx(R, std::vector<std::array<int, 2>>(S, {-1, -1}));
        for (int s = 0; s < S; ++s)
            for (int r = 0; r < R; ++r) {
                if (r == anchor[s]) continue;
                for (int j = 0; j < 2; ++j) {
                    nIdx[r][s][j] = nN++;
                    staOf.push_back(r);
                }
            }
        nlist.assign(R, {});
        for (int r = 0; r < R; ++r)
            for (int s = 0; s < S; ++s)
                for (int j = 0; j < 2; ++j)
                    if (nIdx[r][s][j] >= 0) nlist[r].push_back(nIdx[r][s][j]);
        const int nrow = 4 * S, nl = 1 + S;
        sig.resize(nrow);
        for (int r = 0; r < R; ++r) {
            const int ng = nb + m(r);
            std::vector<int> loc_of(nN, -1);
            for (int a = 0; a < m(r); ++a) loc_of[nlist[r][a]] = nb + a;
            Eigen::MatrixXd Hg = Eigen::MatrixXd::Zero(nrow, ng), Hl = Eigen::MatrixXd::Zero(nrow, nl);
            for (int s = 0; s < S; ++s)
                for (int type = 0; type < 2; ++type)        // 0 phase, 1 code
                    for (int j = 0; j < 2; ++j) {           // band
                        const int row = (s * 2 + type) * 2 + j;
                        Hl(row, 0) = 1.0;                                            // receiver clock
                        Hl(row, 1 + s) = (type == 0 ? -1.0 : 1.0) * kAlpha[j];      // slant ionosphere
                        if (s >= 1) Hg(row, s - 1) = -1.0;                           // satellite clock
                        if (type == 0) {
                            Hg(row, nclk + 2 * s + j) = 1.0;                         // phase bias
                            if (nIdx[r][s][j] >= 0) Hg(row, loc_of[nIdx[r][s][j]]) = 1.0;
                        }
                        sig(row) = (type == 0) ? kSigPh : kSigCd;
                    }
            Hg0.push_back(Hg);
            Hl0.push_back(Hl);
        }
        std::mt19937_64 pr(777);
        std::uniform_real_distribution<double> ud(0.0, 2.0 * kPi);
        phi.resize(static_cast<std::size_t>(R) * S);
        for (auto& x : phi) x = ud(pr);
        setEpoch(0);
    }

    /**
     * @brief Sets the rows' weights for epoch k: static, or every row of satellite s at
     *        station r scaled in sigma by 1 / sin(e), e = 15 + 30 (1 + sin(2 pi k / 1440 + phi_rs))
     *        degrees.  The local parameters are eliminated per station.
     * @param k Epoch index.
     */
    void setEpoch(int k) {
        if (!tv && !Lam.empty()) return;
        const int nrow = 4 * S;
        Lam.assign(R, {});
        T.assign(R, {});
        THc.assign(R, {});
        sigr.assign(R, Eigen::VectorXd(nrow));
        for (int r = 0; r < R; ++r) {
            Eigen::VectorXd w(nrow);
            for (int row = 0; row < nrow; ++row) {
                double f = 1.0;
                if (tv) {
                    const double e = (15.0 + 30.0 * (1.0 + std::sin(2.0 * kPi * k / 1440.0 +
                                                                    phi[static_cast<std::size_t>(r) * S + row / 4]))) * kPi / 180.0;
                    f = 1.0 / std::sin(e);
                }
                sigr[r](row) = sig(row) * f;
                w(row) = 1.0 / (sigr[r](row) * sigr[r](row));
            }
            const Eigen::MatrixXd HgW = Hg0[r].transpose() * w.asDiagonal();
            const Eigen::MatrixXd HlW = Hl0[r].transpose() * w.asDiagonal();
            const Eigen::MatrixXd Nll_inv = spdInverse(HlW * Hl0[r]);
            const Eigen::MatrixXd Ngl = HgW * Hl0[r];
            Lam[r] = HgW * Hg0[r] - Ngl * Nll_inv * Ngl.transpose();
            T[r] = HgW - Ngl * Nll_inv * HlW;
            THc[r] = T[r] * Hg0[r].leftCols(nclk);
        }
    }

    /// Station r's reduced column a -> the estimator's column (border at 0, N at nOff).
    int gcol(int r, int a, int nOff) const { return a < nb ? a : nOff + nlist[r][a - nb]; }

    /// Adds every station's reduced information and right-hand side.
    void addObs(const std::vector<Eigen::MatrixXd>& rhs, int nOff, Eigen::MatrixXd& L, Eigen::MatrixXd& E) const {
        for (int r = 0; r < R; ++r) {
            const int ng = static_cast<int>(Lam[r].rows());
            for (int a = 0; a < ng; ++a) {
                const int ga = gcol(r, a, nOff);
                for (int b = 0; b < ng; ++b) L(ga, gcol(r, b, nOff)) += Lam[r](a, b);
                for (int c = 0; c < E.cols(); ++c) E(ga, c) += rhs[r](a, c);
            }
        }
    }

    /// Inverse of M's per-station diagonal blocks (its cross-station blocks ignored).
    Eigen::MatrixXd blockInverse(const Eigen::MatrixXd& M) const {
        Eigen::MatrixXd W = Eigen::MatrixXd::Zero(nN, nN);
        for (int r = 0; r < R; ++r) {
            Eigen::MatrixXd B(m(r), m(r));
            for (int a = 0; a < m(r); ++a)
                for (int b = 0; b < m(r); ++b) B(a, b) = M(nlist[r][a], nlist[r][b]);
            const Eigen::MatrixXd Bi = spdInverse(B);
            for (int a = 0; a < m(r); ++a)
                for (int b = 0; b < m(r); ++b) W(nlist[r][a], nlist[r][b]) = Bi(a, b);
        }
        return W;
    }
};

/// One estimator; after an epoch with out = true, Xclk (nclk x columns) and Pclk hold the
/// non-pivot clocks' estimates and stated variances.
struct Estimator {
    Eigen::MatrixXd Xclk;
    Eigen::VectorXd Pclk;
    virtual ~Estimator() = default;
    virtual void epoch(const UnitNetwork& g, const std::vector<Eigen::MatrixXd>& rhs, double qdt, bool out) = 0;
};

/// The batch: an exact Kalman filter over [border, every carried ambiguity] in information
/// form (the clock walk's prediction as a rank-nclk Woodbury update).
struct Batch : Estimator {
    bool have{false};
    Eigen::MatrixXd Lam, eta;
    void epoch(const UnitNetwork& g, const std::vector<Eigen::MatrixXd>& rhs, double qdt, bool out) override {
        const int n = g.nb + g.nN, nc = g.nclk;
        if (!have) {
            Lam = Eigen::MatrixXd::Zero(n, n);
            eta = Eigen::MatrixXd::Zero(n, rhs[0].cols());
        } else {
            const Eigen::MatrixXd Lc = Lam.leftCols(nc);
            Eigen::MatrixXd D = Lam.topLeftCorner(nc, nc);
            D.diagonal().array() += 1.0 / qdt;
            const Eigen::MatrixXd LcDi = Lc * spdInverse(D);
            const Eigen::MatrixXd etac = eta.topRows(nc);
            Lam.noalias() -= LcDi * Lc.transpose();
            eta.noalias() -= LcDi * etac;
            Lam = (0.5 * (Lam + Lam.transpose())).eval();
        }
        g.addObs(rhs, g.nb, Lam, eta);
        have = true;
        if (out) {
            const Eigen::MatrixXd P = spdInverse(Lam);
            Xclk = P.topRows(nc) * eta;
            Pclk = P.diagonal().head(nc);
        }
    }
};

/// Telemetry every recursion carries.
struct RecursionBase : Estimator {
    int fails{0};
    int kt_max{0};            ///< mode 6: largest history width
    double d0x_max{0.0};      ///< mode 6: largest cross-station correlation of D0 (a self-check)
    double drop_max{0.0};     ///< mode 6: largest dropped singular-value mass share
};

/// The border prior of mode 3: the shipped per-row time update's epoch (production couplingExactFactor).
CouplingExactEpoch perRowEpoch(const Eigen::MatrixXd& Pprev, int nb, int nclk, double qdt, int& fails) {
    CouplingExactEpoch ep;
    ep.P = Pprev;
    ep.s_of_p.resize(nb);
    ep.route_p.assign(nb, static_cast<int>(CouplingExactDRoute::Present));
    ep.clk_p.assign(nb, 0);
    ep.cc_p.assign(nb, 'G');
    Eigen::VectorXd qs = Eigen::VectorXd::Zero(nb);
    for (int p = 0; p < nb; ++p) {
        ep.s_of_p[p] = p;
        ep.p_of_s.push_back(p);
        ep.col_of_s.push_back(p);
        ep.clk_p[p] = p < nclk ? 1 : 0;
        if (p < nclk) qs(p) = qdt;
    }
    ep.q_s = qs;
    ep.shat_s = Eigen::VectorXd::Zero(nb);
    if (!couplingExactFactor(ep)) ++fails;
    return ep;
}

/**
 * @brief Modes 3 and 5, whose carried prior is block diagonal by station: each station's
 *        ambiguity block is eliminated onto u = [border, clones] (exact for these modes; the
 *        cases read only their stated variances, which agree with the dense algebra to 1e-7).
 */
struct RecBlock : RecursionBase {
    int mode{5};
    bool have{false};
    Eigen::MatrixXd sprev, Pprev;
    std::vector<Eigen::MatrixXd> nhat, G, C;   ///< per station

    explicit RecBlock(int mode_) : mode(mode_) {}

    void epoch(const UnitNetwork& g, const std::vector<Eigen::MatrixXd>& rhs, double qdt, bool) override {
        const int nb = g.nb, nclk = g.nclk, R = g.R, ncols = static_cast<int>(rhs[0].cols());
        const int ncl = (have && mode == 5) ? nclk : 0;
        const int nu = nb + ncl;
        Eigen::MatrixXd Luu = Eigen::MatrixXd::Zero(nu, nu), Eu = Eigen::MatrixXd::Zero(nu, ncols);
        std::vector<Eigen::MatrixXd> Arr(R), Aru(R), Er(R);
        for (int r = 0; r < R; ++r) {
            const int m = g.m(r);
            Luu.topLeftCorner(nb, nb) += g.Lam[r].topLeftCorner(nb, nb);
            Aru[r] = Eigen::MatrixXd::Zero(m, nu);
            Aru[r].leftCols(nb) = g.Lam[r].bottomLeftCorner(m, nb);
            Arr[r] = g.Lam[r].bottomRightCorner(m, m);
            Eu.topRows(nb) += rhs[r].topRows(nb);
            Er[r] = rhs[r].bottomRows(m);
        }
        if (have) {
            const auto vcol = [&](int p) { return (mode == 5 && p < nclk) ? nb + p : p; };
            CouplingExactEpoch ep;
            if (mode == 5) {
                const Eigen::MatrixXd Lm = spdInverse(Pprev);
                const Eigen::MatrixXd LmS = Lm * sprev;
                for (int a = 0; a < nb; ++a) {
                    for (int c = 0; c < nb; ++c) Luu(vcol(a), vcol(c)) += Lm(a, c);
                    Eu.row(vcol(a)) += LmS.row(a);
                }
                const double w = 1.0 / qdt;
                for (int p = 0; p < nclk; ++p) {
                    Luu(p, p) += w; Luu(nb + p, nb + p) += w; Luu(p, nb + p) -= w; Luu(nb + p, p) -= w;
                }
            } else {
                ep = perRowEpoch(Pprev, nb, nclk, qdt, fails);
                Eigen::MatrixXd PS = Pprev;
                for (int p = 0; p < nclk; ++p) PS(p, p) += qdt;
                const Eigen::MatrixXd Lf = spdInverse(PS);
                Luu.topLeftCorner(nb, nb) += Lf;
                Eu.topRows(nb).noalias() += Lf * sprev;
            }
            for (int r = 0; r < R; ++r) {
                const int m = g.m(r);
                Eigen::MatrixXd Mb(m, nb), Sig = C[r];   // rows N_r + Mb s_prev = Y + e
                if (mode == 5) {
                    Mb = -G[r];
                } else {
                    for (int a = 0; a < m; ++a) {
                        std::vector<std::pair<int, double>> terms;
                        for (int p = 0; p < nb; ++p)
                            if (G[r](a, p) != 0.0) terms.emplace_back(p, -G[r](a, p));
                        const CouplingExactRow xr = couplingExactRow(ep, terms);
                        if (!xr.ok) { ++fails; Mb.row(a).setZero(); continue; }
                        Mb.row(a) = xr.m_new.transpose();
                        Sig(a, a) += couplingExactCross(ep, xr, xr);
                    }
                }
                Eigen::MatrixXd Mu = Eigen::MatrixXd::Zero(m, nu);
                for (int p = 0; p < nb; ++p) Mu.col(vcol(p)) += Mb.col(p);
                Eigen::MatrixXd Y = nhat[r];
                Y.noalias() += Mb * sprev;
                const Eigen::MatrixXd W = spdInverse(Sig);
                const Eigen::MatrixXd WMu = W * Mu;
                Luu.noalias() += Mu.transpose() * WMu;
                Aru[r] += WMu;
                Arr[r] += W;
                Eu.noalias() += WMu.transpose() * Y;
                Er[r].noalias() += W * Y;
            }
        }
        // Eliminate each station's block onto u.
        std::vector<Eigen::MatrixXd> F(R), Ai(R);
        Eigen::MatrixXd Ls = Luu, Es = Eu;
        for (int r = 0; r < R; ++r) {
            Ai[r] = spdInverse(Arr[r]);
            F[r] = Ai[r] * Aru[r];
            Ls.noalias() -= Aru[r].transpose() * F[r];
            Es.noalias() -= F[r].transpose() * Er[r];
        }
        const Eigen::MatrixXd Suu = spdInverse(Ls);
        const Eigen::MatrixXd Xu = Suu * Es;
        sprev = Xu.topRows(nb);
        Pprev = Suu.topLeftCorner(nb, nb);
        const Eigen::MatrixXd Pinv = spdInverse(Pprev);
        Eigen::MatrixXd Scc;   // mode 5: the clones given the border
        if (ncl > 0) {
            const Eigen::MatrixXd Scb = Suu.block(nb, 0, ncl, nb);
            Scc = Suu.block(nb, nb, ncl, ncl) - Scb * Pinv * Scb.transpose();
        }
        nhat.assign(R, {});
        G.assign(R, {});
        C.assign(R, {});
        for (int r = 0; r < R; ++r) {
            nhat[r] = Ai[r] * Er[r];
            nhat[r].noalias() -= F[r] * Xu;
            G[r] = -(F[r] * Suu.leftCols(nb)) * Pinv;
            C[r] = Ai[r];   // Cov(N_r | u): the per-station harvest keeps the clone term on its own block
            if (ncl > 0) C[r].noalias() += F[r].rightCols(ncl) * Scc * F[r].rightCols(ncl).transpose();
        }
        have = true;
        Xclk = sprev.topRows(nclk);
        Pclk = Pprev.diagonal().head(nclk);
    }
};

/**
 * @brief Modes 4 and 6, dense: the border's marginal (sprev, Pprev) plus the carried
 *        ambiguities' rows N = nhat + G (s - sprev) [+ Fh H] + e, e ~ N(0, C), solved with the
 *        full inverse (whose rounding matches the batch's to 1e-8; an arrow-head elimination
 *        agrees with it only to about 1e-6 in variance and 1e-5 sigma in value).
 */
struct Recursion : RecursionBase {
    int mode{4};
    int cap{kNoCap};          ///< mode 6: history columns kept
    bool have{false};
    Eigen::MatrixXd sprev, Pprev, nhat, G, C, Fh;

    Recursion(int mode_, int cap_ = kNoCap) : mode(mode_), cap(cap_) {}

    void epoch(const UnitNetwork& g, const std::vector<Eigen::MatrixXd>& rhs, double qdt, bool) override {
        const int nb = g.nb, nclk = g.nclk, nN = g.nN;
        const int ncl = have ? nclk : 0;
        const int nh = (have && mode == 6) ? static_cast<int>(Fh.cols()) : 0;
        const int n0 = nb + ncl + nh, nx = n0 + nN, ncols = static_cast<int>(rhs[0].cols());
        Eigen::MatrixXd L = Eigen::MatrixXd::Zero(nx, nx), E = Eigen::MatrixXd::Zero(nx, ncols);
        if (have) {
            // The previous border, its clocks on the clone columns; the walk links each clone to
            // its current clock.
            const Eigen::MatrixXd Lm = spdInverse(Pprev);
            const auto vcol = [&](int p) { return p < nclk ? nb + p : p; };
            const Eigen::MatrixXd LmS = Lm * sprev;
            for (int a = 0; a < nb; ++a) {
                for (int c = 0; c < nb; ++c) L(vcol(a), vcol(c)) += Lm(a, c);
                E.row(vcol(a)) += LmS.row(a);
            }
            const double w = 1.0 / qdt;
            for (int p = 0; p < nclk; ++p) {
                L(p, p) += w; L(nb + p, nb + p) += w; L(p, nb + p) -= w; L(nb + p, p) -= w;
            }
            // The carried rows N + Mu u = Y + e (u = the first n0 columns), e ~ N(0, C).
            Eigen::MatrixXd Mu = Eigen::MatrixXd::Zero(nN, n0), Y = nhat;
            for (int p = 0; p < nb; ++p) Mu.col(vcol(p)) -= G.col(p);
            if (nh > 0) {   // mode 6: the history columns, prior N(0, I) independent of the border
                Mu.middleCols(nb + ncl, nh) = -Fh;
                for (int p = 0; p < nh; ++p) L(nb + ncl + p, nb + ncl + p) += 1.0;
            }
            Y.noalias() -= G * sprev;
            const Eigen::MatrixXd W = (mode == 4) ? spdInverse(C) : g.blockInverse(C);
            const Eigen::MatrixXd WM = W * Mu;
            const Eigen::MatrixXd WY = W * Y;
            L.topLeftCorner(n0, n0).noalias() += Mu.transpose() * WM;
            L.block(n0, 0, nN, n0) += WM;
            L.block(0, n0, n0, nN) += WM.transpose();
            L.bottomRightCorner(nN, nN) += W;
            E.topRows(n0).noalias() += Mu.transpose() * WY;
            E.bottomRows(nN) += WY;
        }
        g.addObs(rhs, n0, L, E);
        const Eigen::MatrixXd Sx = spdInverse(L);
        const Eigen::MatrixXd X = Sx * E;
        sprev = X.topRows(nb);
        Pprev = Sx.topLeftCorner(nb, nb);
        const Eigen::MatrixXd Pinv = spdInverse(Pprev);
        const Eigen::MatrixXd Snb = Sx.block(n0, 0, nN, nb);
        G = Snb * Pinv;
        C = Sx.block(n0, n0, nN, nN) - Snb * Pinv * Snb.transpose();   // Cov(N | s), exact
        if (mode == 6) harvestHistory(g, Sx, Pinv, Snb, ncl + nh, n0);
        nhat = X.middleRows(n0, nN);
        have = true;
        Xclk = sprev.topRows(nclk);
        Pclk = Pprev.diagonal().head(nclk);
    }

    /// Mode 6's harvest: the shared conditional term compressed to at most `cap` history columns.
    void harvestHistory(const UnitNetwork& g, const Eigen::MatrixXd& Sx, const Eigen::MatrixXd& Pinv,
                        const Eigen::MatrixXd& Snb, int ny, int n0) {
        const int nb = g.nb, nN = g.nN;
        // F_full = Cov(N, Y | s) Sigma_Y|s^(-1/2), Y = [clones; history].
        Eigen::MatrixXd Ff = Eigen::MatrixXd::Zero(nN, ny);
        if (ny > 0) {
            const Eigen::MatrixXd SsY = Sx.block(0, nb, nb, ny);
            const Eigen::MatrixXd PiSsY = Pinv * SsY;
            const Eigen::MatrixXd Syy = Sx.block(nb, nb, ny, ny) - SsY.transpose() * PiSsY;
            const Eigen::MatrixXd SnY = Sx.block(n0, nb, nN, ny) - Snb * PiSsY;
            const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(0.5 * (Syy + Syy.transpose()));
            const Eigen::VectorXd ev = es.eigenvalues();
            Eigen::VectorXd isq(ny);
            for (int i = 0; i < ny; ++i) isq(i) = ev(i) > 1e-15 * ev.maxCoeff() ? 1.0 / std::sqrt(ev(i)) : 0.0;
            Ff = SnY * es.eigenvectors() * isq.asDiagonal();
        }
        // D0 = Cov(N | s, Y): block diagonal by station (the self-check), then exactly so.
        Eigen::MatrixXd D0 = C - Ff * Ff.transpose();
        for (int i = 0; i < nN; ++i)
            for (int j = 0; j < nN; ++j)
                if (g.staOf[i] != g.staOf[j]) {
                    d0x_max = std::max(d0x_max, std::abs(D0(i, j)) / std::sqrt(std::abs(D0(i, i) * D0(j, j))));
                    D0(i, j) = 0.0;
                }
        // Leading right singular directions of F_full (eigenvectors of F_full' F_full).
        int kt = 0;
        Eigen::MatrixXd Vk(ny, 0), Vr(ny, ny);
        if (ny > 0) {
            const Eigen::MatrixXd Gm = Ff.transpose() * Ff;
            const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(0.5 * (Gm + Gm.transpose()));
            const Eigen::VectorXd ev = es.eigenvalues().reverse();
            const Eigen::MatrixXd V = es.eigenvectors().rowwise().reverse();
            const double emax = std::max(ev(0), 0.0);
            int rank = 0;
            for (int i = 0; i < ny; ++i)
                if (emax > 0.0 && ev(i) > 1e-14 * emax) ++rank;
            kt = std::min(cap, rank);
            Vk = V.leftCols(kt);
            Vr = V.rightCols(ny - kt);
            const double tot = ev.cwiseMax(0.0).sum();
            if (tot > 0.0) drop_max = std::max(drop_max, ev.tail(ny - kt).cwiseMax(0.0).sum() / tot);
        }
        Fh = Ff * Vk;
        // The remainder: its per-station blocks join D0, its cross-station blocks are dropped.
        const Eigen::MatrixXd Rem = Ff * Vr;
        const Eigen::MatrixXd RR = Rem * Rem.transpose();
        C = D0;
        for (int i = 0; i < nN; ++i)
            for (int j = 0; j < nN; ++j)
                if (g.staOf[i] == g.staOf[j]) C(i, j) += RR(i, j);
        kt_max = std::max(kt_max, kt);
    }
};

/// One estimator's level at the recorded epochs, plus its run-end telemetry.
struct LevelTrace {
    std::map<int, Eigen::VectorXd> P;
    std::map<int, Eigen::MatrixXd> X;
    int fails{0}, kt_max{0};
    double d0x_max{0.0}, drop_max{0.0};
};

/// The scenarios the cases share, each run once per process.
enum class Scen { Static5, StaticGolden600, Tv5, TvGolden };

/**
 * @brief Runs one scenario at R 8, S 3, rotated anchors: K epochs of one data set (the truth's
 *        non-pivot clocks walk at q), every estimator on the same rows, once per process (the
 *        cases share the runs to stay within the Debug suite's time budget).  Estimator
 *        names: "batch", "m3", "m4", "m5", "m6full" (no cap), "m6sel" (gate U2's selected cap).
 * @param sc The scenario.
 * @return Each estimator's trace by name.
 */
const std::map<std::string, LevelTrace>& scenario(Scen sc) {
    static std::map<Scen, std::map<std::string, LevelTrace>> cache;
    const auto hit = cache.find(sc);
    if (hit != cache.end()) return hit->second;
    const bool tv = (sc == Scen::Tv5 || sc == Scen::TvGolden);
    const double q = (sc == Scen::Static5 || sc == Scen::Tv5) ? kQBroadcast : kQGolden;
    const int K = (sc == Scen::StaticGolden600) ? 600 : (sc == Scen::TvGolden ? 120 : 240);
    const int ncols = (sc == Scen::StaticGolden600) ? 1 : 4;
    std::vector<std::string> names = {"batch"};
    switch (sc) {
        case Scen::Static5: names.insert(names.end(), {"m4", "m5", "m6full"}); break;
        case Scen::StaticGolden600: names.insert(names.end(), {"m3", "m5"}); break;
        case Scen::Tv5: names.insert(names.end(), {"m6full", "m6sel"}); break;
        case Scen::TvGolden: names.insert(names.end(), {"m6full"}); break;
    }
    const auto t0 = std::chrono::steady_clock::now();
    UnitNetwork g(8, 3, /*rotated=*/true, tv);
    std::vector<std::unique_ptr<Estimator>> est;
    for (const std::string& nm : names) {
        if (nm == "batch") est.push_back(std::make_unique<Batch>());
        else if (nm == "m6full") est.push_back(std::make_unique<Recursion>(6, kNoCap));
        else if (nm == "m6sel") est.push_back(std::make_unique<Recursion>(6, kSelectedHistoryPerClock * g.nclk));
        else if (nm == "m4") est.push_back(std::make_unique<Recursion>(4));
        else est.push_back(std::make_unique<RecBlock>(nm[1] - '0'));
    }
    const std::vector<int> rec = {60, 120, 240, 600};
    std::map<std::string, LevelTrace> out;
    std::mt19937_64 rng(12345);
    std::normal_distribution<double> nd(0.0, 1.0);
    const double qdt = q * kDt;
    Eigen::MatrixXd ctrue = Eigen::MatrixXd::Zero(g.nclk, ncols);
    std::vector<Eigen::MatrixXd> rhs(g.R);
    for (int k = 1; k <= K; ++k) {
        if (k >= 2)
            for (int i = 0; i < g.nclk; ++i)
                for (int j = 0; j < ncols; ++j) ctrue(i, j) += std::sqrt(qdt) * nd(rng);
        g.setEpoch(k);
        for (int r = 0; r < g.R; ++r) {
            Eigen::MatrixXd zo(4 * g.S, ncols);
            for (int i = 0; i < 4 * g.S; ++i)
                for (int j = 0; j < ncols; ++j) zo(i, j) = g.sigr[r](i) * nd(rng);
            rhs[r] = g.THc[r] * ctrue + g.T[r] * zo;
        }
        const bool rk = std::find(rec.begin(), rec.end(), k) != rec.end();
        for (std::size_t e = 0; e < est.size(); ++e) {
            est[e]->epoch(g, rhs, qdt, rk);
            if (rk) {
                out[names[e]].P[k] = est[e]->Pclk;
                out[names[e]].X[k] = est[e]->Xclk;
            }
        }
    }
    for (std::size_t e = 0; e < est.size(); ++e)
        if (const auto* rc = dynamic_cast<const RecursionBase*>(est[e].get())) {
            LevelTrace& t = out[names[e]];
            t.fails = rc->fails;
            t.kt_max = rc->kt_max;
            t.d0x_max = rc->d0x_max;
            t.drop_max = rc->drop_max;
        }
    std::printf("[U1] scenario %d (tv %d, q %.1e, K %d, %d estimators) ran in %.1f s\n", static_cast<int>(sc), tv ? 1 : 0,
                q, K, static_cast<int>(est.size()),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return cache.emplace(sc, std::move(out)).first->second;
}

/// Smallest stated level variance over the batch's, over the non-pivot satellites.
double worstRatio(const LevelTrace& e, const LevelTrace& b, int k) {
    return (e.P.at(k).array() / b.P.at(k).array()).minCoeff();
}
/// Largest |stated variance ratio - 1| and level value difference (in the batch's sigma)
/// over the recorded epochs up to kmax.
std::pair<double, double> diffToBatch(const LevelTrace& e, const LevelTrace& b, int kmax) {
    double v = 0.0, x = 0.0;
    for (const auto& kv : b.P) {
        const int k = kv.first;
        if (k > kmax) continue;
        const Eigen::VectorXd& pb = kv.second;
        v = std::max(v, (e.P.at(k).array() / pb.array() - 1.0).abs().maxCoeff());
        for (int c = 0; c < pb.size(); ++c)
            x = std::max(x, (e.X.at(k).row(c) - b.X.at(k).row(c)).cwiseAbs().maxCoeff() / std::sqrt(pb(c)));
    }
    return {v, x};
}

// ---------------------------------------------------------------------------------------------
// Gate U4 (amendment A1): the smoothed hold's row against the shared history.
// The target is c_s(k0) for every non-pivot satellite, born at the stash of epoch k0 as one row
// on the border (T = c_s, residual 0) and carried with no observations, as the smoothed hold's row; its
// walk share is Phi = c(t|t) - c(k0|t).
// ---------------------------------------------------------------------------------------------

constexpr int kHoldK0    = 20;    ///< the target's birth epoch
constexpr int kHoldK     = 240;   ///< the graded epoch
constexpr int kHoldSeeds = 32;    ///< realizations (columns of one right-hand side)

/// The batch with the fixed-point target: the exact filter over [border, every ambiguity]
/// with the old clocks kept (appended) at the k0 -> k0 + 1 prediction, so c_s(k0 | t) is exact.
struct HoldBatch {
    bool have{false};
    Eigen::MatrixXd Lam, eta, Xclk, Xt;
    void epoch(const UnitNetwork& g, const std::vector<Eigen::MatrixXd>& rhs, double qdt, int k, bool out) {
        const int nc = g.nclk;
        if (!have) {
            Lam = Eigen::MatrixXd::Zero(g.nb + g.nN, g.nb + g.nN);
            eta = Eigen::MatrixXd::Zero(g.nb + g.nN, rhs[0].cols());
        } else if (k == kHoldK0 + 1) {
            const int nn = static_cast<int>(Lam.rows());
            const auto mp = [&](int i) { return i < nc ? nn + i : i; };
            Eigen::MatrixXd Le = Eigen::MatrixXd::Zero(nn + nc, nn + nc), Ee = Eigen::MatrixXd::Zero(nn + nc, eta.cols());
            for (int i = 0; i < nn; ++i) {
                for (int j = 0; j < nn; ++j) Le(mp(i), mp(j)) = Lam(i, j);
                Ee.row(mp(i)) = eta.row(i);
            }
            const double w = 1.0 / qdt;
            for (int p = 0; p < nc; ++p) {
                Le(p, p) += w; Le(nn + p, nn + p) += w; Le(p, nn + p) -= w; Le(nn + p, p) -= w;
            }
            Lam = Le;
            eta = Ee;
        } else {
            const Eigen::MatrixXd Lc = Lam.leftCols(nc);
            Eigen::MatrixXd D = Lam.topLeftCorner(nc, nc);
            D.diagonal().array() += 1.0 / qdt;
            const Eigen::MatrixXd LcDi = Lc * spdInverse(D);
            const Eigen::MatrixXd etac = eta.topRows(nc);
            Lam.noalias() -= LcDi * Lc.transpose();
            eta.noalias() -= LcDi * etac;
            Lam = (0.5 * (Lam + Lam.transpose())).eval();
        }
        g.addObs(rhs, g.nb, Lam, eta);
        have = true;
        if (out && k > kHoldK0) {
            const Eigen::MatrixXd X = spdInverse(Lam) * eta;
            Xclk = X.topRows(nc);
            Xt = X.middleRows(g.nb + g.nN, nc);
        }
    }
};

/// The hold row in the recursion's convention: T = That + g (s_prev-terms) + f (history terms).
struct HoldTarget {
    bool on{false};
    Eigen::MatrixXd That;   ///< nclk x columns
    Eigen::MatrixXd g;      ///< nclk x nb: coefficients on the previous border
    Eigen::MatrixXd f;      ///< nclk x nh: coefficients on the history columns of the last compression
    void birth(const Eigen::MatrixXd& sprev, int nb, int nclk, int nh) {
        on = true;
        That = sprev.topRows(nclk);
        g = Eigen::MatrixXd::Zero(nclk, nb);
        for (int p = 0; p < nclk; ++p) g(p, p) = 1.0;
        f = Eigen::MatrixXd::Zero(nclk, nh);
    }
};

/**
 * @brief Mode 6 (X2) at a cap, with two hold rows: the per-row form (production X2 with
 *        the smoothed hold before amendment A1: the row's Y coefficients regressed onto the border, its
 *        conditional term marginalized onto its own variance, no history kept) and form p (the
 *        row's F_T = Cov(T, Y | s) Sigma_Y|s^(-1/2) projected onto the carried rows' kept
 *        directions V_kt, its history coefficients F_T V_kt revised with the history columns
 *        next epoch; the compression unchanged).
 *
 * The carried ambiguities' prior is block diagonal by station (D0 plus the remainder's
 * within-station blocks), so each station's block is eliminated onto u = [border, clones,
 * history] exactly; the covariance blocks the harvest reads (per station: Cov(N_r, u),
 * Cov(N_r)) follow from the elimination.  Mode 6's dense algebra above is the reference for
 * the recursion itself; the target's values are linear in the data, so only they are carried.
 */
struct HoldX2 {
    int cap;
    bool have{false};
    int nh{0};
    Eigen::MatrixXd sprev, Pprev, Xclk;
    std::vector<Eigen::MatrixXd> nhat, G, C, Fh;   ///< per station
    HoldTarget ta, tp;                             ///< the per-row form, form p
    double tdrop_max{0.0};                         ///< form p: the largest remainder share of a target row
    int kt_max{0};

    explicit HoldX2(int cap_) : cap(cap_) {}

    void epoch(const UnitNetwork& g, const std::vector<Eigen::MatrixXd>& rhs, double qdt, int k) {
        const int nb = g.nb, nclk = g.nclk, R = g.R, ncols = static_cast<int>(rhs[0].cols());
        const int ncl = have ? nclk : 0;
        const int nhh = have ? nh : 0;
        const int n0 = nb + ncl + nhh, ny = ncl + nhh;
        const auto vcol = [&](int p) { return (have && p < nclk) ? nb + p : p; };
        Eigen::MatrixXd Luu = Eigen::MatrixXd::Zero(n0, n0), Eu = Eigen::MatrixXd::Zero(n0, ncols);
        std::vector<Eigen::MatrixXd> Arr(R), Aru(R), Er(R);
        for (int r = 0; r < R; ++r) {
            const int m = g.m(r);
            Luu.topLeftCorner(nb, nb) += g.Lam[r].topLeftCorner(nb, nb);
            Aru[r] = Eigen::MatrixXd::Zero(m, n0);
            Aru[r].leftCols(nb) = g.Lam[r].bottomLeftCorner(m, nb);
            Arr[r] = g.Lam[r].bottomRightCorner(m, m);
            Eu.topRows(nb) += rhs[r].topRows(nb);
            Er[r] = rhs[r].bottomRows(m);
        }
        if (have) {
            const Eigen::MatrixXd Lm = spdInverse(Pprev);
            const Eigen::MatrixXd LmS = Lm * sprev;
            for (int a = 0; a < nb; ++a) {
                for (int c = 0; c < nb; ++c) Luu(vcol(a), vcol(c)) += Lm(a, c);
                Eu.row(vcol(a)) += LmS.row(a);
            }
            const double w = 1.0 / qdt;
            for (int p = 0; p < nclk; ++p) {
                Luu(p, p) += w; Luu(nb + p, nb + p) += w; Luu(p, nb + p) -= w; Luu(nb + p, p) -= w;
            }
            for (int j = 0; j < nhh; ++j) Luu(nb + ncl + j, nb + ncl + j) += 1.0;   // N(0, I)
            for (int r = 0; r < R; ++r) {
                const int m = g.m(r);
                Eigen::MatrixXd Mu = Eigen::MatrixXd::Zero(m, n0);
                for (int p = 0; p < nb; ++p) Mu.col(vcol(p)) -= G[r].col(p);
                if (nhh > 0) Mu.middleCols(nb + ncl, nhh) = -Fh[r];
                Eigen::MatrixXd Y = nhat[r];
                Y.noalias() -= G[r] * sprev;
                const Eigen::MatrixXd W = spdInverse(C[r]);
                const Eigen::MatrixXd WMu = W * Mu;
                Luu.noalias() += Mu.transpose() * WMu;
                Aru[r] += WMu;
                Arr[r] += W;
                Eu.noalias() += WMu.transpose() * Y;
                Er[r].noalias() += W * Y;
            }
        }
        std::vector<Eigen::MatrixXd> F(R), Ai(R);
        Eigen::MatrixXd Ls = Luu, Es = Eu;
        for (int r = 0; r < R; ++r) {
            Ai[r] = spdInverse(Arr[r]);
            F[r] = Ai[r] * Aru[r];
            Ls.noalias() -= Aru[r].transpose() * F[r];
            Es.noalias() -= F[r].transpose() * Er[r];
        }
        const Eigen::MatrixXd Suu = spdInverse(Ls);
        const Eigen::MatrixXd Xu = Suu * Es;
        // The targets' value update: no observations, so the revision of the border they hold.
        Eigen::MatrixXd aTa, aTp;
        const auto advance = [&](HoldTarget& tg, Eigen::MatrixXd& aT) {
            if (!tg.on) return;
            aT = Eigen::MatrixXd::Zero(nclk, n0);
            Eigen::MatrixXd up = Eigen::MatrixXd::Zero(n0, ncols);
            for (int p = 0; p < nb; ++p) {
                aT.col(vcol(p)) += tg.g.col(p);
                up.row(vcol(p)) = sprev.row(p);
            }
            if (nhh > 0 && tg.f.cols() == nhh) aT.middleCols(nb + ncl, nhh) += tg.f;
            tg.That.noalias() += aT * (Xu - up);
        };
        advance(ta, aTa);
        advance(tp, aTp);
        sprev = Xu.topRows(nb);
        Pprev = Suu.topLeftCorner(nb, nb);
        const Eigen::MatrixXd Pinv = spdInverse(Pprev);
        // The harvest: Y = [clones; history], F_full = Cov(N, Y | s) Sigma_Y|s^(-1/2).
        Eigen::MatrixXd SsY, PiSsY, Syy, Bm;
        if (ny > 0) {
            SsY = Suu.block(0, nb, nb, ny);
            PiSsY = Pinv * SsY;
            Syy = Suu.block(nb, nb, ny, ny) - SsY.transpose() * PiSsY;
            const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(0.5 * (Syy + Syy.transpose()));
            const Eigen::VectorXd ev = es.eigenvalues();
            Eigen::VectorXd isq(ny);
            for (int i = 0; i < ny; ++i) isq(i) = ev(i) > 1e-15 * ev.maxCoeff() ? 1.0 / std::sqrt(ev(i)) : 0.0;
            Bm = es.eigenvectors() * isq.asDiagonal();
        }
        std::vector<Eigen::MatrixXd> Ff(R);
        Eigen::MatrixXd Gm = Eigen::MatrixXd::Zero(ny, ny);
        nhat.assign(R, {});
        G.assign(R, {});
        C.assign(R, {});
        for (int r = 0; r < R; ++r) {
            const Eigen::MatrixXd SNu = -F[r] * Suu;                 // Cov(N_r, u)
            const Eigen::MatrixXd Snb = SNu.leftCols(nb);
            G[r] = Snb * Pinv;
            C[r] = Ai[r];                                             // Cov(N_r | s)
            C[r].noalias() -= SNu * F[r].transpose();
            C[r].noalias() -= G[r] * Snb.transpose();
            nhat[r] = Ai[r] * Er[r];
            nhat[r].noalias() -= F[r] * Xu;
            if (ny > 0) {
                Ff[r] = (SNu.middleCols(nb, ny) - Snb * PiSsY) * Bm;
                C[r].noalias() -= Ff[r] * Ff[r].transpose();         // D0_r = Cov(N_r | s, Y)
                Gm.noalias() += Ff[r].transpose() * Ff[r];
            }
        }
        int kt = 0;
        Eigen::MatrixXd Vk(ny, 0), Vr(ny, ny);
        if (ny > 0) {
            const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(0.5 * (Gm + Gm.transpose()));
            const Eigen::VectorXd ev = es.eigenvalues().reverse();
            const Eigen::MatrixXd V = es.eigenvectors().rowwise().reverse();
            const double emax = std::max(ev(0), 0.0);
            int rank = 0;
            for (int i = 0; i < ny; ++i)
                if (emax > 0.0 && ev(i) > 1e-14 * emax) ++rank;
            kt = std::min(cap, rank);
            Vk = V.leftCols(kt);
            Vr = V.rightCols(ny - kt);
        }
        Fh.assign(R, {});
        for (int r = 0; r < R; ++r) {
            if (ny > 0) {
                Fh[r] = Ff[r] * Vk;
                const Eigen::MatrixXd Rem = Ff[r] * Vr;
                C[r].noalias() += Rem * Rem.transpose();
            } else {
                Fh[r] = Eigen::MatrixXd::Zero(g.m(r), 0);
            }
        }
        nh = kt;
        kt_max = std::max(kt_max, kt);
        // The targets re-expressed on the new border: the per-row form drops its Y residual
        // (onto its own variance), form p keeps its coordinates on the kept directions.
        const auto reexpress = [&](HoldTarget& tg, const Eigen::MatrixXd& aT, bool formp) {
            if (!tg.on) return;
            Eigen::MatrixXd gn = aT.leftCols(nb);
            tg.f = Eigen::MatrixXd::Zero(nclk, kt);
            if (ny > 0) {
                const Eigen::MatrixXd aY = aT.middleCols(nb, ny);
                gn.noalias() += aY * SsY.transpose() * Pinv;
                if (formp) {
                    const Eigen::MatrixXd FT = aY * Syy * Bm;
                    tg.f = FT * Vk;
                    const double tt = FT.squaredNorm();
                    if (tt > 0.0) tdrop_max = std::max(tdrop_max, (FT * Vr).squaredNorm() / tt);
                }
            }
            tg.g = gn;
        };
        reexpress(ta, aTa, false);
        reexpress(tp, aTp, true);
        have = true;
        if (k == kHoldK0) {
            ta.birth(sprev, nb, nclk, kt);
            tp.birth(sprev, nb, nclk, kt);
        }
        Xclk = sprev.topRows(nclk);
    }
};

/// U4's readings at one geometry: the walk share's MSE over the batch's (mean over the
/// non-pivot satellites) for both hold forms, and the current clock's.
struct HoldReading {
    double phi_rowform{0.0}, phi_formp{0.0}, clk{0.0}, tdrop_max{0.0};
    int kt_max{0};
};

/**
 * @brief Runs U4's scenario once per process: R 8, S 6, rotated anchors, q 5e-6, K 240, the
 *        target born at epoch 20, static (@p tv false) or swept weights; the batch and X2 at
 *        gate U2's selected cap with both hold forms on the same data.
 */
const HoldReading& holdScenario(bool tv) {
    static std::map<bool, HoldReading> cache;
    const auto hit = cache.find(tv);
    if (hit != cache.end()) return hit->second;
    const auto t0 = std::chrono::steady_clock::now();
    UnitNetwork g(8, 6, /*rotated=*/true, tv);
    HoldBatch batch;
    HoldX2 x2(kSelectedHistoryPerClock * g.nclk);
    std::mt19937_64 rng(12345);
    std::normal_distribution<double> nd(0.0, 1.0);
    const double qdt = kQBroadcast * kDt;
    Eigen::MatrixXd ctrue = Eigen::MatrixXd::Zero(g.nclk, kHoldSeeds), ttrue;
    std::vector<Eigen::MatrixXd> rhs(g.R);
    for (int k = 1; k <= kHoldK; ++k) {
        if (k >= 2)
            for (int i = 0; i < g.nclk; ++i)
                for (int j = 0; j < kHoldSeeds; ++j) ctrue(i, j) += std::sqrt(qdt) * nd(rng);
        if (k == kHoldK0) ttrue = ctrue;
        g.setEpoch(k);
        for (int r = 0; r < g.R; ++r) {
            Eigen::MatrixXd zo(4 * g.S, kHoldSeeds);
            for (int i = 0; i < 4 * g.S; ++i)
                for (int j = 0; j < kHoldSeeds; ++j) zo(i, j) = g.sigr[r](i) * nd(rng);
            rhs[r] = g.THc[r] * ctrue + g.T[r] * zo;
        }
        batch.epoch(g, rhs, qdt, k, k == kHoldK);
        x2.epoch(g, rhs, qdt, k);
    }
    HoldReading out;
    const Eigen::MatrixXd phit = ctrue - ttrue;
    const Eigen::MatrixXd eb = (batch.Xclk - batch.Xt) - phit;
    const Eigen::MatrixXd ea = (x2.Xclk - x2.ta.That) - phit;
    const Eigen::MatrixXd ep = (x2.Xclk - x2.tp.That) - phit;
    for (int p = 0; p < g.nclk; ++p) {
        const double b = eb.row(p).squaredNorm();
        out.phi_rowform += ea.row(p).squaredNorm() / b;
        out.phi_formp += ep.row(p).squaredNorm() / b;
        out.clk += (x2.Xclk.row(p) - ctrue.row(p)).squaredNorm() / (batch.Xclk.row(p) - ctrue.row(p)).squaredNorm();
    }
    out.phi_rowform /= g.nclk;
    out.phi_formp /= g.nclk;
    out.clk /= g.nclk;
    out.tdrop_max = x2.tdrop_max;
    out.kt_max = x2.kt_max;
    std::printf("[U4] hold scenario (R 8, S 6, tv %d, q 5e-6, K %d, %d seeds) ran in %.1f s\n", tv ? 1 : 0, kHoldK,
                kHoldSeeds, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return cache.emplace(tv, out).first->second;
}

}  // namespace

// U1 (a): the per-station harvest (mode 5) states the level too tightly at the broadcast-grade
// walk under rotated anchors (R 8, S 3, K 240), while the joint harvest (mode 4) equals the
// batch.  The golden-q line at K 600 is printed, not asserted.
TEST(CouplingLongHorizonTest, PerStationHarvestStatesTheLevelTooTightly) {
    const auto& s5 = scenario(Scen::Static5);
    const double r5 = worstRatio(s5.at("m5"), s5.at("batch"), 240);
    const auto d4 = diffToBatch(s5.at("m4"), s5.at("batch"), 240);
    std::printf("[U1a] R 8 S 3 rotated q 5e-6 K 240: mode 5 worst stated level variance / batch %.4f; "
                "mode 4 against the batch: max |var ratio - 1| %.2e, max |dx| %.2e sigma\n", r5, d4.first, d4.second);
    EXPECT_EQ(s5.at("m4").fails + s5.at("m5").fails, 0);
    EXPECT_LT(r5, 0.5);
    EXPECT_LE(d4.first, 1e-7);
    EXPECT_LE(d4.second, 1e-7);

    const auto& s0 = scenario(Scen::StaticGolden600);
    std::printf("[U1a] golden q 2.9e-8, K 600 (printed, not asserted): worst stated level variance / batch, "
                "mode 3 %.4f, mode 5 %.4f\n", worstRatio(s0.at("m3"), s0.at("batch"), 600),
                worstRatio(s0.at("m5"), s0.at("batch"), 600));
    EXPECT_EQ(s0.at("m3").fails + s0.at("m5").fails, 0);
}

// U1 (b): mode 6 with no cap equals the batch in level value and variance (both q: the
// static geometry at the broadcast grade over 240 epochs, the time-varying one at the
// broadcast grade over 240 and at the golden q over 120); D0 = Cov(N | s, Y) is block
// diagonal by station to rounding.
TEST(CouplingLongHorizonTest, SharedHistoryAtFullCapEqualsTheBatch) {
    for (const Scen sc : {Scen::Static5, Scen::Tv5, Scen::TvGolden}) {
        const auto& s = scenario(sc);
        const auto d = diffToBatch(s.at("m6full"), s.at("batch"), 240);
        std::printf("[U1b] %s: mode 6 (no cap) against the batch: max |var ratio - 1| %.2e, max |dx| %.2e sigma; "
                    "history width max %d; D0 cross-station corr max %.1e\n",
                    sc == Scen::Static5 ? "static q 5e-6" : (sc == Scen::Tv5 ? "time-varying q 5e-6" : "time-varying q 2.9e-8"),
                    d.first, d.second, s.at("m6full").kt_max, s.at("m6full").d0x_max);
        EXPECT_LE(d.first, 1e-7);
        EXPECT_LE(d.second, 1e-7);
        EXPECT_LE(s.at("m6full").d0x_max, 1e-6);
    }
}

// U1 (c): mode 6 at gate U2's selected cap keeps the stated level within 5 % of the batch's at
// the broadcast-grade walk (R 8, S 3, K 240) on the time-varying geometry, where the cap
// truncates.  On the static geometry one column per clock is the uncapped recursion (the
// rank case below), which (b) already holds to the batch.
TEST(CouplingLongHorizonTest, SharedHistoryAtTheSelectedCapKeepsTheLevel) {
    const auto& s = scenario(Scen::Tv5);
    const double r = worstRatio(s.at("m6sel"), s.at("batch"), 240);
    std::printf("[U1c] time-varying: mode 6 at k = %d: worst stated level variance / batch %.4f; history width max %d; "
                "dropped mass max %.2e\n", kSelectedHistoryPerClock, r, s.at("m6sel").kt_max, s.at("m6sel").drop_max);
    EXPECT_GT(r, 0.95);
    EXPECT_EQ(s.at("m6sel").kt_max, kSelectedHistoryPerClock * 2);
}

// The static geometry's rank property: with the same rows every epoch the shared term has rank
// nclk at every epoch (one history column per clock is exact there by construction), while
// the time-varying geometry needs more.  A pass on the static fixture is no evidence for a cap.
TEST(CouplingLongHorizonTest, StaticGeometryLimitsTheSharedTermToOneColumnPerClock) {
    const int kt_static = scenario(Scen::Static5).at("m6full").kt_max;
    const int kt_tv = scenario(Scen::Tv5).at("m6full").kt_max;
    std::printf("[U1d] history width with no cap, R 8 S 3, K 240: static %d, time-varying %d (nclk 2)\n", kt_static, kt_tv);
    EXPECT_EQ(kt_static, 2);
    EXPECT_GT(kt_tv, 2);
}

// U4 (amendment A1): the smoothed hold's row in X2's shared history.  At gate
// U2's selected cap (k = 1), R 8, S 6, q 5e-6, K 240, static and swept weights, the hold row
// projected onto the carried rows' kept directions (form p) gives the walk share within 2 % of
// the batch's fixed-point smoother, while the per-row form (the row's Y residual marginalized
// onto its own variance, production X2 before the amendment) is at least 5 times the batch's.
TEST(CouplingLongHorizonTest, HoldRowProjectedOntoTheHistoryIsTheFixedPointSmoother) {
    for (const bool tv : {false, true}) {
        const HoldReading& h = holdScenario(tv);
        std::printf("[U4] %s, k = %d: walk-share MSE over the batch's at epoch %d: form p %.4f, per-row form %.2f; "
                    "current clock %.4f; history width max %d; target remainder share max %.2e\n",
                    tv ? "swept weights" : "static", kSelectedHistoryPerClock, kHoldK, h.phi_formp, h.phi_rowform,
                    h.clk, h.kt_max, h.tdrop_max);
        EXPECT_NEAR(h.phi_formp, 1.0, 0.02);
        EXPECT_GE(h.phi_rowform, 5.0);
        EXPECT_EQ(h.kt_max, kSelectedHistoryPerClock * 5);
    }
}
