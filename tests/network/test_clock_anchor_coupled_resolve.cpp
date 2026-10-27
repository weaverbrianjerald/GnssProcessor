/**
 * @file test_clock_anchor_coupled_resolve.cpp
 * @brief The clock anchor's same-epoch withdraw-and-re-solve against the exact coupling rows:
 *        the attribution at unit level, and the deferral knob
 *        (`network.clock_anchor_defer_coupled`).
 *
 * ### The attribution
 *
 * A carried float ambiguity is stashed as a conditional row on the shared border,
 * @f$N + M^\top s = \hat N + M^\top \hat s@f$, and re-expressed each epoch over the columns the
 * fold carries with their inflation Q (@ref couplingExactRow).  For a clock that the anchor holds,
 * Q is tiny, so the row keeps its harvested clock coefficient: it states that the ambiguity moves
 * with the clock exactly as the epoch before, an estimation-error relation.  The guard withdraws
 * only the shared prior of a tripped clock, after those rows were folded and reduced.
 *
 * The fixture is the smallest one that carries what the rows need.  S satellites (satellite 0 is
 * the clock pivot, pinned), one phase bias each, R stations (station 0 holds the phase datum:
 * its ambiguities are known), a carried float ambiguity per other (station, satellite), a free
 * receiver clock per station and epoch, phase and code on one band.  Epoch 1 is solved densely
 * and harvested (border posterior P, ambiguity regression G, conditional covariance C).  Epoch 2
 * carries a clock step on one satellite and is solved by the real estimator (NetworkEstimator,
 * the dense border prior, the coupling rows as each station's extra rows) four ways:
 *
 *   (a) the guard's path: rows re-expressed with the clock's ANCHORED inflation, the dense prior
 *       folded, then the clock marginalized out of the dense prior (the call the guard makes,
 *       `refoldSharedPriorDenseDroppingClocks`) and the epoch re-solved;
 *   (b) the consistent reference: rows re-expressed with the clock LOOSE (the legacy rate times
 *       dt, the next epoch's decision), the LOOSE prior folded, nothing withdrawn;
 *   (c) the first solve: anchored rows, anchored prior, nothing withdrawn;
 *   (d) diagnostic: LOOSE rows, LOOSE prior, then the same withdrawal as (a).
 *
 * The cases print their numbers (`[ARS]` lines) and assert what the fixture established, and
 * nothing is tuned: the fixture's size and noise are the first values chosen, and the sweeps
 * vary one stated parameter each.
 *
 * ### The knob
 *
 * With `network.clock_anchor_defer_coupled` a tripped anchored clock whose own column the
 * coupling rows reference (a member of S of the rows' epoch, not on a clone) is decoupled and
 * marked but not withdrawn or re-solved; off, the old path runs.  A clock in D, or on its clone,
 * is not deferred.
 */

#include <gtest/gtest.h>

#include "network/state/RunContext.hpp"
#include "network/driver/Epoch.hpp"
#include "config/GnssConfig.hpp"
#include "network/carry/CouplingExact.hpp"
#include "network/solve/ClockAnchor.hpp"
#include "network/solve/Estimator.hpp"
#include "logging/LogManager.hpp"
#include "network/diag/RunTraces.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>
#include "../support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

// ---------------------------------------------------------------------------
// The synthetic border, its epoch-1 harvest and the epoch-2 data
// ---------------------------------------------------------------------------

constexpr double kSigPh = 0.003;                 // phase noise (m)
constexpr double kSigCd = 0.30;                  // code noise (m)
constexpr double kDt    = 30.0;                  // epoch spacing (s)
constexpr double kQAnchored = 2.9e-8 * kDt;      // the anchor's inflation on the sim golden (m^2)
constexpr double kQLoose    = 1.0e-3 * kDt;      // the legacy satellite-clock rate over dt (m^2)

std::string satName(int s) { return "G0" + std::to_string(s + 1); }

struct Scn {
    int    S = 5;               ///< satellites; satellite 0 is the pivot (pinned)
    int    R = 6;               ///< stations; station 0 holds the phase datum
    int    step_sat = 2;        ///< the satellite whose clock steps in epoch 2
    double step_m = 0.5;        ///< the step (m)
    double row_var_scale = 1.0; ///< multiplier on the coupling rows' covariance (1 = the producer's rows)
    unsigned seed = 11u;
};

struct Harvest {
    int nb{0}, nN{0};
    Eigen::VectorXd shat;       ///< border mean after epoch 1
    Eigen::MatrixXd P;          ///< border covariance
    Eigen::VectorXd nhat;       ///< ambiguity means
    Eigen::MatrixXd G;          ///< N = nhat + G (s - shat) + e
    Eigen::MatrixXd C;          ///< Var e
};

struct Truth {
    std::vector<double> c1, c2, p;               // by satellite
    std::vector<std::vector<double>> N;          // [r][s]
    std::vector<double> rx1, rx2;                // by station
    std::vector<std::vector<double>> ph2, cd2;   // epoch-2 observations [r][s]
};

int bC(int s) { return s - 1; }                           // clock c_s (s >= 1)
int bP(const Scn& m, int s) { return (m.S - 1) + s; }   // phase bias p_s
int nIdx(const Scn& m, int r, int s) { return (r - 1) * m.S + s; }   // carried ambiguity (r >= 1)

Truth makeTruth(const Scn& m) {
    std::mt19937 rng(m.seed);
    std::normal_distribution<double> nd(0.0, 1.0);
    Truth t;
    t.c1.assign(m.S, 0.0); t.c2.assign(m.S, 0.0); t.p.assign(m.S, 0.0);
    for (int s = 1; s < m.S; ++s) t.c1[s] = 0.8 * nd(rng);
    for (int s = 0; s < m.S; ++s) t.p[s] = nd(rng);
    t.N.assign(m.R, std::vector<double>(m.S, 0.0));
    for (int r = 0; r < m.R; ++r)
        for (int s = 0; s < m.S; ++s) t.N[r][s] = 3.0 * nd(rng);
    t.rx1.resize(m.R); t.rx2.resize(m.R);
    for (int r = 0; r < m.R; ++r) { t.rx1[r] = 10.0 * nd(rng); t.rx2[r] = 10.0 * nd(rng); }
    t.c2 = t.c1;
    t.c2[m.step_sat] += m.step_m;
    // Epoch-1 observations are generated inside harvest(); epoch-2 ones here.
    t.ph2.assign(m.R, std::vector<double>(m.S)); t.cd2.assign(m.R, std::vector<double>(m.S));
    for (int r = 0; r < m.R; ++r)
        for (int s = 0; s < m.S; ++s) {
            t.ph2[r][s] = t.rx2[r] - t.c2[s] + t.p[s] + t.N[r][s] + kSigPh * nd(rng);
            t.cd2[r][s] = t.rx2[r] - t.c2[s] + kSigCd * nd(rng);
        }
    return t;
}

/// Epoch 1, solved densely over [border, ambiguities, receiver clocks], and its harvest.
Harvest harvest(const Scn& m, const Truth& t) {
    std::mt19937 rng(m.seed + 1000u);
    std::normal_distribution<double> nd(0.0, 1.0);
    Harvest h;
    h.nb = (m.S - 1) + m.S;
    h.nN = (m.R - 1) * m.S;
    const int n = h.nb + h.nN + m.R;
    Eigen::MatrixXd L = Eigen::MatrixXd::Zero(n, n);
    Eigen::VectorXd e = Eigen::VectorXd::Zero(n);
    const auto add = [&](const std::vector<std::pair<int, double>>& a, double y, double sig) {
        const double w = 1.0 / (sig * sig);
        for (const auto& [i, ai] : a)
            for (const auto& [j, aj] : a) L(i, j) += w * ai * aj;
        for (const auto& [i, ai] : a) e(i) += w * ai * y;
    };
    for (int r = 0; r < m.R; ++r)
        for (int s = 0; s < m.S; ++s) {
            const int rx = h.nb + h.nN + r;
            std::vector<std::pair<int, double>> a{{rx, 1.0}};
            if (s >= 1) a.emplace_back(bC(s), -1.0);
            add(a, t.rx1[r] - t.c1[s] + kSigCd * nd(rng), kSigCd);                   // code
            a.emplace_back(bP(m, s), 1.0);
            double y = t.rx1[r] - t.c1[s] + t.p[s] + t.N[r][s] + kSigPh * nd(rng);   // phase
            if (r >= 1) a.emplace_back(h.nb + nIdx(m, r, s), 1.0);
            else y -= t.N[0][s];                                                      // held datum
            add(a, y, kSigPh);
        }
    const Eigen::LDLT<Eigen::MatrixXd> f(L);
    const Eigen::MatrixXd Sx = f.solve(Eigen::MatrixXd::Identity(n, n));
    const Eigen::VectorXd x = Sx * e;
    h.shat = x.head(h.nb);
    h.P    = Sx.topLeftCorner(h.nb, h.nb);
    h.nhat = x.segment(h.nb, h.nN);
    const Eigen::MatrixXd Snb = Sx.block(h.nb, 0, h.nN, h.nb);
    const Eigen::LDLT<Eigen::MatrixXd> fp(h.P);
    h.G = Snb * fp.solve(Eigen::MatrixXd::Identity(h.nb, h.nb));
    h.C = Sx.block(h.nb, h.nb, h.nN, h.nN) - h.G * Snb.transpose();
    return h;
}

// ---------------------------------------------------------------------------
// Epoch 2 on the real estimator
// ---------------------------------------------------------------------------

struct Epoch2 {
    NetworkEstimator est;
    std::vector<NetworkEstimator::SharedPrior> keys;
    Eigen::MatrixXd cov_eff;
    NetworkEstimator::Datum datum;
    Eigen::VectorXd x;
    Eigen::MatrixXd cov;
    double own_coeff_mean{0.0};   ///< mean |clock coefficient| of the step satellite's own rows
    double row_var_mean{0.0};     ///< mean added row variance M'BM (m^2)
};

/// Build epoch 2: the coupling epoch with the step satellite's clock at inflation @p q_step,
/// the dense border prior over the same inflation, every station's observation rows and
/// coupling rows.
void build(Epoch2& E, const Scn& m, const Harvest& h, const Truth& t, double q_step) {
    for (int s = 0; s < m.S; ++s) E.est.registerClock(satName(s));
    for (int s = 0; s < m.S; ++s) E.est.registerPhaseBias(satName(s), "L1C");
    E.est.beginEpoch();
    E.datum = {{E.est.clockColumn(satName(0)), 0.0}};
    const Eigen::Index nsh = E.est.numShared();

    // The coupling epoch: every stashed column is in S; the step satellite's clock has
    // inflation q_step, the other clocks the anchor's.
    CouplingExactEpoch ep;
    ep.P = h.P;
    ep.s_of_p.assign(h.nb, -1);
    ep.route_p.assign(h.nb, static_cast<int>(CouplingExactDRoute::Present));
    ep.clk_p.assign(h.nb, 0);
    ep.cc_p.assign(h.nb, 'G');
    std::vector<double> qs, sh;
    E.keys.clear();
    for (int p = 0; p < h.nb; ++p) {
        const bool clk = p < m.S - 1;
        const int sat = clk ? p + 1 : p - (m.S - 1);
        ep.clk_p[p] = clk ? 1 : 0;
        ep.s_of_p[p] = static_cast<int>(ep.p_of_s.size());
        ep.p_of_s.push_back(p);
        ep.col_of_s.push_back(clk ? E.est.clockColumn(satName(sat))
                                  : E.est.phaseBiasColumn(satName(sat), "L1C"));
        qs.push_back(clk ? (sat == m.step_sat ? q_step : kQAnchored) : 0.0);
        sh.push_back(h.shat(p));
        NetworkEstimator::SharedPrior k;
        k.kind  = clk ? SharedParamKind::Clock : SharedParamKind::PhaseBias;
        k.sat   = satName(sat);
        k.signal = clk ? std::string{} : std::string("L1C");
        k.value = h.shat(p);
        k.info  = 1.0;
        E.keys.push_back(k);
    }
    ep.q_s    = Eigen::Map<Eigen::VectorXd>(qs.data(), static_cast<Eigen::Index>(qs.size()));
    ep.shat_s = Eigen::Map<Eigen::VectorXd>(sh.data(), static_cast<Eigen::Index>(sh.size()));
    ASSERT_TRUE(couplingExactFactor(ep));

    // The dense border prior N(shat, P + Q).
    E.cov_eff = h.P;
    for (int p = 0; p < h.nb; ++p) E.cov_eff(p, p) += qs[static_cast<std::size_t>(p)];

    // The rows of every other station: one per ambiguity.
    std::vector<CouplingExactRow> rows(static_cast<std::size_t>(h.nN));
    int n_own = 0;
    for (int i = 0; i < h.nN; ++i) {
        std::vector<std::pair<int, double>> terms;
        for (int p = 0; p < h.nb; ++p)
            if (h.G(i, p) != 0.0) terms.emplace_back(p, -h.G(i, p));
        rows[static_cast<std::size_t>(i)] = couplingExactRow(ep, terms);
        ASSERT_TRUE(rows[static_cast<std::size_t>(i)].ok);
        E.row_var_mean += rows[static_cast<std::size_t>(i)].var_add / h.nN;
        if (i % m.S == m.step_sat) {   // the rows of the step satellite itself
            E.own_coeff_mean += std::abs(rows[static_cast<std::size_t>(i)].m_new(bC(m.step_sat)));
            ++n_own;
        }
    }
    if (n_own > 0) E.own_coeff_mean /= n_own;

    for (int r = 0; r < m.R; ++r) {
        const int nloc = 1 + (r >= 1 ? m.S : 0);
        const int nobs = 2 * m.S, ncpl = (r >= 1 ? m.S : 0), mrow = nobs + ncpl;
        Eigen::MatrixXd As = Eigen::MatrixXd::Zero(mrow, nsh);
        Eigen::MatrixXd Ar = Eigen::MatrixXd::Zero(mrow, nloc);
        Eigen::MatrixXd W  = Eigen::MatrixXd::Zero(mrow, mrow);
        Eigen::VectorXd y(mrow);
        for (int s = 0; s < m.S; ++s) {
            const int rc = 2 * s, rp = 2 * s + 1;
            const Eigen::Index cc = E.est.clockColumn(satName(s));
            const Eigen::Index cp = E.est.phaseBiasColumn(satName(s), "L1C");
            Ar(rc, 0) = 1.0; As(rc, cc) = -1.0;
            y(rc) = t.cd2[r][s];            W(rc, rc) = 1.0 / (kSigCd * kSigCd);
            Ar(rp, 0) = 1.0; As(rp, cc) = -1.0; As(rp, cp) = 1.0;
            y(rp) = t.ph2[r][s];            W(rp, rp) = 1.0 / (kSigPh * kSigPh);
            if (r >= 1) Ar(rp, 1 + s) = 1.0; else y(rp) -= t.N[0][s];
        }
        if (r >= 1) {
            // The station's coupling rows: N_i + (m_new . s) = nhat_i + m_new . shat, with the
            // rows' covariance C (the station's block) plus each row's added variance.
            Eigen::MatrixXd Sig(m.S, m.S);
            for (int a = 0; a < m.S; ++a)
                for (int b = 0; b < m.S; ++b)
                    Sig(a, b) = h.C(nIdx(m, r, a), nIdx(m, r, b));
            for (int s = 0; s < m.S; ++s) {
                const int i = nIdx(m, r, s);
                const CouplingExactRow& cr = rows[static_cast<std::size_t>(i)];
                const int row = nobs + s;
                Ar(row, 1 + s) = 1.0;
                y(row) = h.nhat(i);
                for (Eigen::Index j = 0; j < ep.nS(); ++j) {
                    As(row, ep.col_of_s[static_cast<std::size_t>(j)]) += cr.m_new(j);
                    y(row) += cr.m_new(j) * ep.shat_s(j);
                }
                Sig(s, s) += cr.var_add;
            }
            Sig *= m.row_var_scale;
            const Eigen::MatrixXd Wc = Eigen::LDLT<Eigen::MatrixXd>(Sig)
                                           .solve(Eigen::MatrixXd::Identity(m.S, m.S));
            W.block(nobs, nobs, m.S, m.S) = 0.5 * (Wc + Wc.transpose());
        }
        E.est.addStation(As, Ar, W, y);
    }
    E.est.addSharedPriorDense(E.keys, E.cov_eff);
}

/// The first solve of an epoch built by @ref build.
void firstSolve(Epoch2& E) {
    E.x = E.est.solve(E.datum);
    E.cov = E.est.sharedCovariance();
}

/// The guard's withdrawal of @p sat's clock prior from the dense block, and the re-solve.
void withdrawAndResolve(Epoch2& E, const std::string& sat) {
    E.est.refoldSharedPriorDenseDroppingClocks(E.keys, E.cov_eff, {sat});
    E.x = E.est.solve(E.datum);
    E.cov = E.est.sharedCovariance();
}

struct Four {
    Eigen::VectorXd a, b, c, d;
    double sd_b{0.0};
    double own_coeff_anch{0.0}, own_coeff_loose{0.0};
    double row_var_anch{0.0}, row_var_loose{0.0};
    Eigen::Index col_step{0};
    double truth_step_clock{0.0};
};

Four runFour(const Scn& m) {
    const Truth t = makeTruth(m);
    const Harvest h = harvest(m, t);
    Four out;
    {   // (a) and (c): anchored rows and prior
        Epoch2 E;
        build(E, m, h, t, kQAnchored);
        firstSolve(E);
        out.c = E.x;
        out.own_coeff_anch = E.own_coeff_mean;
        out.row_var_anch = E.row_var_mean;
        withdrawAndResolve(E, satName(m.step_sat));
        out.a = E.x;
    }
    {   // (b) and (d): LOOSE rows and prior
        Epoch2 E;
        build(E, m, h, t, kQLoose);
        firstSolve(E);
        out.b = E.x;
        out.col_step = E.est.clockColumn(satName(m.step_sat));
        out.sd_b = std::sqrt(E.cov(out.col_step, out.col_step));
        out.own_coeff_loose = E.own_coeff_mean;
        out.row_var_loose = E.row_var_mean;
        withdrawAndResolve(E, satName(m.step_sat));
        out.d = E.x;
    }
    out.truth_step_clock = t.c2[m.step_sat];
    return out;
}

void report(const char* tag, const Scn& m, const Four& f) {
    const Eigen::Index k = f.col_step;
    std::printf("[ARS] %-22s S=%d R=%d step=%+.2f m rowvar x%-5g | clock error vs truth (m): "
                "(a) guard %+9.4f  (b) loose %+9.4f  (c) first %+9.4f  (d) loose+withdraw %+9.4f | "
                "|a-b| %8.4f  |c-b| %8.4f  |d-b| %8.4f | sd(b) %.4f | own-row clock coeff "
                "anch %.3f loose %.3f, added var anch %.2e loose %.2e\n",
                tag, m.S, m.R, m.step_m, m.row_var_scale, f.a(k) - f.truth_step_clock,
                f.b(k) - f.truth_step_clock, f.c(k) - f.truth_step_clock,
                f.d(k) - f.truth_step_clock, std::abs(f.a(k) - f.b(k)), std::abs(f.c(k) - f.b(k)),
                std::abs(f.d(k) - f.b(k)), f.sd_b, f.own_coeff_anch, f.own_coeff_loose,
                f.row_var_anch, f.row_var_loose);
}

}  // namespace

// ===========================================================================
// The attribution
// ===========================================================================

namespace {

/// The clock of the step satellite estimated in a run, relative to the same run without the step.
double recovered(const Scn& m, const Four& f, const Scn& m0, const Four& f0, char which) {
    const Eigen::VectorXd& a  = which == 'a' ? f.a  : which == 'b' ? f.b  : which == 'c' ? f.c  : f.d;
    const Eigen::VectorXd& a0 = which == 'a' ? f0.a : which == 'b' ? f0.b : which == 'c' ? f0.c : f0.d;
    (void)m; (void)m0;
    return a(f.col_step) - a0(f0.col_step);
}

Scn withStep(double step_m) { Scn m; m.step_m = step_m; return m; }

}  // namespace

/// Without a step every solve agrees: the control that the four paths differ only on an event.
TEST(ClockAnchorCoupledResolve, WithoutAStepEverySolveAgrees) {
    const Scn m = withStep(0.0);
    const Four f = runFour(m);
    report("no step", m, f);
    const Eigen::Index k = f.col_step;
    EXPECT_LT(std::abs(f.a(k) - f.b(k)), 0.01);
    EXPECT_LT(std::abs(f.c(k) - f.b(k)), 0.01);
    EXPECT_LT(std::abs(f.d(k) - f.b(k)), 0.01);
}

/// The consistent reference recovers the step: the clock it estimates moves by the step when
/// the data's step is added (the same noise draws), and its error against truth stays inside
/// the data's formal precision at a 0.5 m and a 3 m step.
TEST(ClockAnchorCoupledResolve, ConsistentRowsRecoverTheStepToTheDatasPrecision) {
    const Scn m0 = withStep(0.0);
    const Four f0 = runFour(m0);
    for (const double step : {0.5, 3.0}) {
        const Scn m = withStep(step);
        const Four f = runFour(m);
        report(step < 1.0 ? "reference, 0.5 m" : "reference, 3 m", m, f);
        EXPECT_NEAR(recovered(m, f, m0, f0, 'b'), step, 2e-3) << "step " << step;
        const double err = f.b(f.col_step) - f.truth_step_clock;
        EXPECT_LT(std::abs(err), 4.0 * f.sd_b) << "step " << step;
    }
}

/// The guard's path departs from the consistent reference, linearly in the step, while the
/// same withdrawal on LOOSE rows changes nothing: the departure is the rows' conditioning on
/// the anchored clock, not the withdrawal.  The first solve is pulled toward the stale prior.
TEST(ClockAnchorCoupledResolve, TheGuardsPathDepartsFromTheConsistentReferenceAndTheRowsExplainIt) {
    const Scn m1 = withStep(0.5), m2 = withStep(3.0);
    const Four f1 = runFour(m1), f2 = runFour(m2);
    report("guard path, 0.5 m", m1, f1);
    report("guard path, 3 m", m2, f2);
    const Eigen::Index k = f1.col_step;
    const double d1 = std::abs(f1.a(k) - f1.b(k)), d2 = std::abs(f2.a(k) - f2.b(k));
    EXPECT_GT(d1, 0.1 * m1.step_m) << "departure at 0.5 m";
    EXPECT_GT(d2, 0.1 * m2.step_m) << "departure at 3 m";
    EXPECT_GT(d2 / d1, 5.5);
    EXPECT_LT(d2 / d1, 6.5);   // linear in the step (3 / 0.5 = 6)
    EXPECT_LT(std::abs(f1.d(k) - f1.b(k)), 1e-3) << "withdrawal alone, on consistent rows";
    EXPECT_LT(std::abs(f2.d(k) - f2.b(k)), 1e-3);
    EXPECT_GT(std::abs(f2.c(k) - f2.b(k)), 0.2 * m2.step_m) << "the first solve is pulled by the prior";
    EXPECT_LT(f1.own_coeff_loose, 0.01) << "the LOOSE rows carry almost no clock coefficient";
    EXPECT_GT(f1.own_coeff_anch, 0.5) << "the anchored rows keep most of it";
}

/// The departure grows with the rows' clock coefficient (more satellites in the rows' elimination
/// of the receiver clock).
TEST(ClockAnchorCoupledResolve, DepartureGrowsWithTheRowsClockCoefficient) {
    double prev_dep = 0.0, prev_coeff = 0.0;
    for (const int S : {3, 5, 8, 12}) {
        Scn m; m.S = S;
        const Four f = runFour(m);
        report("sweep satellites", m, f);
        const double dep = std::abs(f.a(f.col_step) - f.b(f.col_step));
        EXPECT_GT(f.own_coeff_anch, prev_coeff) << "S " << S;
        EXPECT_GT(dep, prev_dep) << "S " << S;
        prev_dep = dep; prev_coeff = f.own_coeff_anch;
    }
}

/// The departure grows as the rows' residual variance shrinks (an explicit multiplier on the
/// coupling rows' covariance; 1 is the producer's rows).
TEST(ClockAnchorCoupledResolve, DepartureGrowsAsTheRowsResidualVarianceShrinks) {
    double prev = 0.0;
    for (const double sc : {100.0, 10.0, 1.0, 0.1, 0.01}) {
        Scn m; m.row_var_scale = sc;
        const Four f = runFour(m);
        report("sweep row variance", m, f);
        const double dep = std::abs(f.a(f.col_step) - f.b(f.col_step));
        EXPECT_GT(dep, prev) << "variance x" << sc;
        prev = dep;
    }
}

/// The departure grows with the number of stations whose rows reference the clock.
TEST(ClockAnchorCoupledResolve, DepartureGrowsWithTheNumberOfStationsCarryingRows) {
    double prev = 0.0;
    for (const int R : {3, 4, 6, 12, 24, 48}) {
        Scn m; m.R = R;
        const Four f = runFour(m);
        report("sweep stations", m, f);
        const double dep = std::abs(f.a(f.col_step) - f.b(f.col_step));
        EXPECT_GT(dep, prev) << "R " << R;
        prev = dep;
    }
}

// ===========================================================================
// The knob
// ===========================================================================

namespace {

/// A loaded config with the anchor, the exact coupling update (and what it requires) and,
/// when asked, the deferral.
struct DeferLoaded {
    LogManager log;
    GnssConfig cfg;
    fs::path   path;
    DeferLoaded(const std::string& tag, bool defer, bool exact = true) {
        const std::string body = std::string("{\n") +
            "  \"processing\": { \"mode\": \"ppp-rtk-network\", \"active_systems\": [ \"GPS\" ],\n"
            "                    \"signal_priorities\": { \"GPS\": \"WC\" } },\n"
            "  \"estimation\": { \"estimate_ifb\": true },\n"
            "  \"filter\": { \"process_noise\": { \"sat_clock_q\": 1e-3 } },\n"
            "  \"network\": {\n"
            "    \"clock_datum_station\": \"AAAA\",\n"
            "    \"clock_temporal_anchor\": true,\n"
            "    \"clock_anchor_q\": 1e-5,\n"
            "    \"clock_anchor_var_floor_m2\": 1e-6,\n"
            "    \"clock_anchor_max_gap_s\": 120.0,\n"
            "    \"clock_anchor_k\": 6.0,\n"
            "    \"clock_anchor_abs_m\": 2.0,\n"
            "    \"clock_anchor_sigma_floor_m\": 0.02,\n"
            "    \"clock_anchor_recouple_epochs\": 3,\n"
            + (exact ? std::string("    \"shared_carry_full_cov\": true,\n"
                                   "    \"carry_conditional_coupling\": true,\n"
                                   "    \"carry_coupling_time_update\": true,\n"
                                   "    \"carry_coupling_exact_update\": true,\n")
                     : std::string{})
            + (defer ? "    \"clock_anchor_defer_coupled\": true,\n" : "") +
            "    \"stations\": [\n"
            "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
            "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
            "    ]\n"
            "  }\n"
            "}\n";
        path = gnss_test::tempDir() / ("ars_cfg_" + tag + ".json");
        { std::ofstream f(path); f << body; }
        EXPECT_TRUE(cfg.loadFromJson(path, log));
    }
    ~DeferLoaded() { std::error_code ec; fs::remove(path, ec); }
};

struct ToyNet {
    int R = 5, S = 4;
    std::vector<std::string> sats{"G01", "G02", "G03", "G04"};
    std::vector<double> rx{0.7, -1.3, 0.2, 2.1, -0.4};
};

std::shared_ptr<spdlog::logger> nullLogger() {
    static auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    return std::make_shared<spdlog::logger>("ars_test", sink);
}

/// Where the rows' epoch puts the step satellite's clock.
enum class RowsAt { OwnColumn, InD, OnClone, Unbuilt };

/// One guarded epoch of the toy network: G03's carried clock is 3 m off the data, so the
/// anchor trips it on the statistic.  The coupling epoch the guard reads is set to put G03
/// where @p at says.
struct DeferRun {
    DeferLoaded        L;
    NetworkRunContext  ctx;
    fs::path           dir;
    NetworkRunTraces   traces;
    ToyNet             t;
    NetworkEstimator   est;
    NetworkSbasis::Plan plan;
    EpochWorkspace     ws;
    Eigen::VectorXd    x;

    DeferRun(const std::string& tag, bool defer, RowsAt at, bool exact = true)
        : L(tag, defer, exact), ctx(L.cfg, L.cfg.getNetworkConfig()),
          dir(gnss_test::tempDir() / ("ars_traces_" + tag)),
          traces((fs::create_directories(dir), dir)) {
        for (const auto& s : t.sats) est.registerClock(s);
        plan.shared_datum = {{est.clockColumn(t.sats[0]), 0.0}};
        // The rows' epoch as PASS B would have left it.
        CouplingExactEpoch xe;
        xe.ok = (at != RowsAt::Unbuilt);
        xe.p_of_key[sharedCarryKey(SharedParamKind::Clock, "G03", std::string{})] = 0;
        xe.s_of_p = {at == RowsAt::InD ? -1 : 0};
        xe.clk_p = {1};
        xe.clone_s = {static_cast<char>(at == RowsAt::OnClone ? 1 : 0)};
        ctx.carry.cxu_epoch = xe;
        if (at == RowsAt::OnClone) {
            ctx.carry.ccl.rows_on = true;
            ctx.carry.ccl.rows_epoch = xe;
        }
        // G02..G04 anchored from an earlier fold, as the guard's own tests set them.
        clockAnchorBeginEpoch(ctx);
        for (const char* sat : {"G02", "G03", "G04"}) {
            clockAnchorFoldVariance(ctx, sat, 1e-4, 30.0, 1000.0, 0.0);
            ctx.carry.clk_anchor.sat[sat].anchored = true;
        }
        const std::vector<double> truth{0.0, 1.0, -0.5, 2.0};
        const std::map<std::string, double> carried{{"G02", 1.0}, {"G03", -3.5}, {"G04", 2.0}};
        clockAnchorBeginEpoch(ctx);
        std::vector<NetworkEstimator::SharedPrior> priors;
        for (const auto& [sat, value] : carried) {
            const ClockAnchorFold f = clockAnchorFoldVariance(ctx, sat, 1e-4, 30.0, 1030.0, value);
            NetworkEstimator::SharedPrior p;
            p.kind = SharedParamKind::Clock; p.sat = sat; p.value = value; p.info = 1.0 / f.var_eff;
            priors.push_back(p);
        }
        networkCapturePriors(ws.cap_priors, ws.cap_prior_kinds, priors,
                             ContributionKind::BorderCarryDiagonal);
        est.beginEpoch();
        for (int r = 0; r < t.R; ++r) {
            Eigen::MatrixXd A_s = Eigen::MatrixXd::Zero(t.S, t.S);
            Eigen::MatrixXd A_r = Eigen::MatrixXd::Zero(t.S, 1);
            Eigen::VectorXd w(t.S), y(t.S);
            for (int s = 0; s < t.S; ++s) {
                A_s(s, s) = 1.0; A_r(s, 0) = 1.0; w(s) = 100.0;
                y(s) = truth[static_cast<std::size_t>(s)] + t.rx[static_cast<std::size_t>(r)];
            }
            est.addStation(A_s, A_r, w, y);
        }
        est.addSharedPrior(priors);
        x = est.solve(plan.shared_datum);
        std::vector<std::string> reg(t.sats.begin(), t.sats.end());
        GnssTime tm; tm.week = 2300; tm.tow = 1030.0;
        networkClockAnchorGuard(ctx, L.cfg, nullLogger(), traces, tm, est, plan, x, ws, reg, {},
                                false, {}, EpochEvents{});
    }
    ~DeferRun() { std::error_code ec; fs::remove_all(dir, ec); }
};

}  // namespace

/// A tripped anchored clock whose own column the rows reference is deferred under the knob: it
/// stays decoupled and marked and is recorded as moved, the epoch emits the anchored solve (no
/// re-solve), and the count is its own.
TEST(ClockAnchorDeferCoupled, ATrippedClockInSIsDeferred) {
    DeferRun r("in_s", /*defer=*/true, RowsAt::OwnColumn);
    ASSERT_TRUE(r.ctx.knobs.cta_defer_coupled);
    ASSERT_TRUE(r.ctx.knobs.carry_cpl_exact);
    EXPECT_GE(r.ctx.counters.cta_trip_nis + r.ctx.counters.cta_trip_abs, 1) << "the trip itself is unchanged";
    EXPECT_EQ(r.ctx.counters.cta_resolve_deferred_coupled, 1);
    EXPECT_EQ(r.ctx.counters.cta_resolve_deferred_fault, 1) << "a statistical trip, not the backstop";
    EXPECT_EQ(r.ctx.counters.cta_resolves, 0);
    EXPECT_FALSE(r.ctx.carry.clk_anchor.sat.at("G03").anchored) << "decoupled";
    EXPECT_TRUE(net::hasSat(r.ctx.entities.sats, "G03", net::Disc::ClockAnchor)) << "marked";
    EXPECT_EQ(r.ctx.counters.cta_marks, 1);
    EXPECT_EQ(r.ctx.carry.clk_anchor.stepped.count("G03"), 1u);
    // The anchored solve stands: G03 is pulled toward its stale prior, 3 m off the data.
    const double rel = r.x(r.est.clockColumn("G03")) - r.x(r.est.clockColumn("G02"));
    EXPECT_LT(rel, -2.0) << "the epoch emits the anchored solve (truth -1.5)";
}

/// Off, the same trip is withdrawn and re-solved as before.
TEST(ClockAnchorDeferCoupled, WithTheKnobOffTheOldPathRuns) {
    DeferRun r("off", /*defer=*/false, RowsAt::OwnColumn);
    ASSERT_FALSE(r.ctx.knobs.cta_defer_coupled);
    EXPECT_EQ(r.ctx.counters.cta_resolve_deferred_coupled, 0);
    EXPECT_EQ(r.ctx.counters.cta_resolve_deferred_fault, 0);
    EXPECT_EQ(r.ctx.counters.cta_resolves, 1);
    const double rel = r.x(r.est.clockColumn("G03")) - r.x(r.est.clockColumn("G02"));
    EXPECT_NEAR(rel, -1.5, 0.1) << "the re-solve emits the data's value";
}

/// A clock the rows carry in D (no coefficient of its own), one on its clone, and an unbuilt
/// coupling epoch are not deferred: the old path runs.
TEST(ClockAnchorDeferCoupled, OnlyAClockOnItsOwnColumnInSIsDeferred) {
    {
        DeferRun r("in_d", true, RowsAt::InD);
        EXPECT_EQ(r.ctx.counters.cta_resolve_deferred_coupled, 0);
        EXPECT_EQ(r.ctx.counters.cta_resolves, 1);
    }
    {
        DeferRun r("on_clone", true, RowsAt::OnClone);
        EXPECT_EQ(r.ctx.counters.cta_resolve_deferred_coupled, 0)
            << "the rows reference the clone: its withdrawal is the clone's";
        EXPECT_EQ(r.ctx.counters.cta_resolves, 1);
    }
    {
        DeferRun r("unbuilt", true, RowsAt::Unbuilt);
        EXPECT_EQ(r.ctx.counters.cta_resolve_deferred_coupled, 0);
        EXPECT_EQ(r.ctx.counters.cta_resolves, 1);
    }
}

/// Without the exact coupling update there are no such rows, and the knob is inert.
TEST(ClockAnchorDeferCoupled, WithoutTheExactUpdateTheKnobIsInert) {
    DeferRun r("no_exact", true, RowsAt::OwnColumn, /*exact=*/false);
    EXPECT_FALSE(r.ctx.knobs.carry_cpl_exact);
    EXPECT_EQ(r.ctx.counters.cta_resolve_deferred_coupled, 0);
    EXPECT_EQ(r.ctx.counters.cta_resolves, 1);
}

TEST(ClockAnchorDeferCoupled, TheKeyDefaultsOffParsesAndNeedsTheResolve) {
    {
        DeferLoaded L("def", /*defer=*/false);
        EXPECT_FALSE(L.cfg.networkClockAnchorDeferCoupled());
    }
    {
        DeferLoaded L("on", /*defer=*/true);
        EXPECT_TRUE(L.cfg.networkClockAnchorDeferCoupled());
        NetworkRunContext ctx(L.cfg, L.cfg.getNetworkConfig());
        EXPECT_TRUE(ctx.knobs.cta_defer_coupled);
    }
}
