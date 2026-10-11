/**
 * @file test_network_fixed_cycles.cpp
 * @brief Tests of the fixed-cycle fold's pure steps and of its fold with acceptance (card FCF,
 *        step FCF-2; filter/NetworkFixedCycles.hpp).
 *
 * Suite @c NetworkFixedCycles:
 *  - the chi-square quantile against tabulated values and the closed form at 2 dof, and its
 *    agreement with the Wilson-Hilferty approximation at network-sized K;
 *  - the unit statistic against its closed form, NaN on a covariance that is not positive
 *    definite;
 *  - the units: one per pair determined in both bands, band 1 the shorter wavelength, in basis
 *    order; a pair determined in one band, a band-only DD and a broken twin link give none;
 *  - the per-unit test: right integers pass, a (1, 1) error on a settled pair fails, NaN fails;
 *  - the placement: terms on the live columns, the target re-based on x_lin, the hold variance,
 *    one block per station component, a unit with a missing column left unplaced;
 *  - the fold on a synthetic arrowhead with L1/L2 pairs: right integers fold and equal the
 *    detached held solve of the placed rows; a (1, 1) pair is refused by its own test; a wrong
 *    pair forced past that test fails block acceptance and is dropped, the rest fold; with no
 *    drop pass the failing block is left float and the others fold; the shift bound refuses
 *    the epoch; a malformed placement makes the kernel throw and the epoch stays float; a
 *    released and a lapsed unit (source lattice, step FCF-3) are not placed and release nothing;
 *  - the action names are the trace's vocabulary.
 *
 * Suite name owned by this file alone (suite-hygiene rule in CLAUDE.md).
 */

#include <gtest/gtest.h>

#include "filter/NetworkFixedCycles.hpp"
#include "filter/NetworkEstimator.hpp"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace GnssProcessor {
namespace {

using fcyc::Unit;
using fcyc::UnitAction;

constexpr double kF1 = 1575.42e6;
constexpr double kF2 = 1227.60e6;
constexpr double kC  = 299792458.0;
constexpr double kL1 = kC / kF1;
constexpr double kL2 = kC / kF2;

SignalId sigL1() { return *SignalId::fromObsCode(System::GPS, "L1C"); }
SignalId sigL2() { return *SignalId::fromObsCode(System::GPS, "L2W"); }
SatId satG(int prn) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "G%02d", prn);
    return *SatId::parse(buf);
}

/// A searched DD with an identity (signal, r, p, j, k) and a one- or four-term cycle.
SearchedCycle searched(const SignalId& sig, int r, int p, int j, int k) {
    SearchedCycle s;
    s.id = ArDdIdentity{sig, StationId{r}, StationId{p}, satG(j), satG(k)};
    return s;
}

// ============================== the pure steps ==============================

TEST(NetworkFixedCycles, ChiSquareQuantileMatchesTablesAndClosedForm) {
    EXPECT_NEAR(fcyc::chi2Quantile(1, 0.95), 3.841458820694124, 1e-9);
    EXPECT_NEAR(fcyc::chi2Quantile(2, 1.0 - 1e-6), -2.0 * std::log(1e-6), 1e-9);
    EXPECT_NEAR(fcyc::chi2Quantile(10, 0.99), 23.20925117707, 1e-7);
    EXPECT_NEAR(fcyc::chi2Quantile(100, 0.95), 124.3421134, 1e-6);
    EXPECT_NEAR(fcyc::chi2Quantile(24, 1.0 - 1e-4), 58.6, 0.1);
    // Network-sized K (300 stations: about 2 840 rows): Wilson-Hilferty within 0.05 %.
    for (int K : {330, 2840, 5680}) {
        const double z = 3.719016485455709;   // the standard normal quantile at 1 - 1e-4
        const double a = 2.0 / (9.0 * K);
        const double wh = K * std::pow(1.0 - a + z * std::sqrt(a), 3.0);
        EXPECT_NEAR(fcyc::chi2Quantile(K, 1.0 - 1e-4) / wh, 1.0, 5e-4) << "K " << K;
    }
    EXPECT_THROW(fcyc::chi2Quantile(0, 0.5), std::invalid_argument);
    EXPECT_THROW(fcyc::chi2Quantile(3, 1.0), std::invalid_argument);
}

TEST(NetworkFixedCycles, UnitStatisticIsTheTwoByTwoQuadraticForm) {
    const double var1 = 0.04, var2 = 0.09, cov = 0.03;
    const double y1 = 10.2, y2 = -3.1;
    const Eigen::Matrix2d S = (Eigen::Matrix2d() << var1, cov, cov, var2).finished();
    const Eigen::Vector2d v(y1 - 10.0, y2 + 3.0);
    EXPECT_NEAR(fcyc::unitStatistic(y1, y2, 10, -3, var1, var2, cov), v.dot(S.inverse() * v), 1e-12);
    EXPECT_TRUE(std::isnan(fcyc::unitStatistic(y1, y2, 10, -3, 0.04, 0.09, 0.07)));   // det < 0
    EXPECT_TRUE(std::isnan(fcyc::unitStatistic(y1, y2, 10, -3, 0.0, 0.09, 0.0)));
    EXPECT_TRUE(std::isnan(fcyc::unitStatistic(y1, y2, 10, -3, std::nan(""), 0.09, 0.0)));
}

TEST(NetworkFixedCycles, UnitsArePairsDeterminedInBothBands) {
    ArEpochFixes fx;
    // 0/1: a pair, both fixed, listed L2 first; 2/3: a pair with L1 only fixed; 4: band-only;
    // 5/6: a pair, both fixed, L1 first; 7: a twin link that does not point back.
    fx.floats = {searched(sigL2(), 1, 0, 3, 5), searched(sigL1(), 1, 0, 3, 5),
                 searched(sigL1(), 2, 0, 3, 5), searched(sigL2(), 2, 0, 3, 5),
                 searched(sigL1(), 3, 0, 3, 5),
                 searched(sigL1(), 4, 0, 7, 5), searched(sigL2(), 4, 0, 7, 5),
                 searched(sigL1(), 5, 0, 3, 5)};
    auto twin = [&](int a, int b) { fx.floats[a].twin = b; fx.floats[b].twin = a; };
    twin(0, 1); twin(2, 3); twin(5, 6);
    fx.floats[7].twin = 1;
    const int fixed[] = {0, 1, 2, 4, 5, 6, 7};
    for (int a : fixed) { fx.floats[a].fixed = true; fx.floats[a].integer = 100 + a; }
    const std::vector<Unit> u = fcyc::epochUnits(fx);
    ASSERT_EQ(u.size(), 2u);
    EXPECT_EQ(u[0].l1, 1); EXPECT_EQ(u[0].l2, 0);   // band 1 is the shorter wavelength
    EXPECT_EQ(u[0].n1, 101); EXPECT_EQ(u[0].n2, 100);
    EXPECT_EQ(u[1].l1, 5); EXPECT_EQ(u[1].l2, 6);
    EXPECT_EQ(u[1].n1, 105); EXPECT_EQ(u[1].n2, 106);
}

TEST(NetworkFixedCycles, UnitTestPassesRightIntegersAndRefusesAOneOneError) {
    // A settled pair: wide lane 0.06 cycle, ionosphere-free 0.04 narrow-lane cycle (card FCFB).
    Eigen::Matrix2d M;
    M << 1.0, -1.0, kF1 / (kF1 - kF2), -kF2 / (kF1 - kF2);
    const Eigen::Matrix2d Minv = M.inverse();
    const Eigen::Matrix2d S = Minv * Eigen::Vector2d(0.06 * 0.06, 0.04 * 0.04).asDiagonal() * Minv.transpose();
    ArEpochFixes fx;
    fx.floats = {searched(sigL1(), 1, 0, 3, 5), searched(sigL2(), 1, 0, 3, 5)};
    fx.floats[0].twin = 1; fx.floats[1].twin = 0;
    fx.floats[0].var_cyc2 = S(0, 0); fx.floats[1].var_cyc2 = S(1, 1);
    fx.floats[0].cov_twin_cyc2 = fx.floats[1].cov_twin_cyc2 = S(0, 1);
    // A float error of 0.5 and -0.3 sigma along the covariance's own factor: T = 0.34.
    const Eigen::Vector2d e = Eigen::Matrix2d(S.llt().matrixL()) * Eigen::Vector2d(0.5, -0.3);
    fx.floats[0].y_cyc = 12.0 + e(0);
    fx.floats[1].y_cyc = 9.0 + e(1);
    std::vector<Unit> units(3);
    for (Unit& u : units) { u.l1 = 0; u.l2 = 1; }
    units[0].n1 = 12; units[0].n2 = 9;    // right
    units[1].n1 = 13; units[1].n2 = 10;   // a (1, 1) error: keeps the wide lane
    units[2].n1 = 13; units[2].n2 = 9;    // a +1 L1 error
    fcyc::scoreUnits(units, fx, 1e-6);
    EXPECT_FALSE(units[0].fail);
    EXPECT_NEAR(units[0].t, 0.34, 1e-9);
    EXPECT_TRUE(units[1].fail) << "T " << units[1].t;
    EXPECT_TRUE(units[2].fail) << "T " << units[2].t;
    EXPECT_GT(units[2].t, units[1].t);
    // A pair whose covariance is not positive definite cannot be tested and is refused.
    fx.floats[0].cov_twin_cyc2 = std::sqrt(S(0, 0) * S(1, 1)) * 1.01;
    std::vector<Unit> bad(1, units[0]);
    fcyc::scoreUnits(bad, fx, 1e-6);
    EXPECT_TRUE(std::isnan(bad[0].t));
    EXPECT_TRUE(bad[0].fail);
}

/// A DD cycle (r, p; j, k) in one band, with its arcs' live columns in @p fx.
arcgraph::Cycle ddCycle(ArEpochFixes& fx, const SignalId& sig, int r, int p, int j, int k,
                        std::size_t est_r, std::size_t est_p, double xlin) {
    arcgraph::Cycle c;
    const struct { int sta; std::size_t est; int sat; int coeff; } t[4] = {
        {r, est_r, j, +1}, {r, est_r, k, -1}, {p, est_p, j, -1}, {p, est_p, k, +1}};
    for (const auto& x : t) {
        const ArcId arc{StationId{x.sta}, satG(x.sat), sig};
        c.terms.push_back(arcgraph::CycleTerm{arc, x.coeff});
        fx.columns[arc] = ArcColumn{x.est, static_cast<std::ptrdiff_t>(10 * x.sat + (sig == sigL1() ? 0 : 1)),
                                    xlin * x.coeff + 0.001 * x.sat, sig.wavelengthM(0)};
    }
    std::sort(c.terms.begin(), c.terms.end(),
              [](const arcgraph::CycleTerm& a, const arcgraph::CycleTerm& b) { return a.arc < b.arc; });
    return c;
}

TEST(NetworkFixedCycles, PlacementRebasesTargetsAndBlocksStationComponents) {
    ArEpochFixes fx;
    // Unit A: stations 1-0 (estimator 1, 0); unit B: stations 2-1 (joins A's component);
    // unit C: stations 4-3 (its own component); unit D: a missing column.
    auto add = [&](int r, int p, std::size_t er, std::size_t ep) {
        const int a = static_cast<int>(fx.floats.size());
        fx.floats.push_back(searched(sigL1(), r, p, 3, 5));
        fx.floats.push_back(searched(sigL2(), r, p, 3, 5));
        fx.floats[a].cycle     = ddCycle(fx, sigL1(), r, p, 3, 5, er, ep, 0.5);
        fx.floats[a + 1].cycle = ddCycle(fx, sigL2(), r, p, 3, 5, er, ep, 0.5);
        fx.floats[a].twin = a + 1; fx.floats[a + 1].twin = a;
        Unit u; u.l1 = a; u.l2 = a + 1; u.n1 = 7; u.n2 = -4;
        return u;
    };
    std::vector<Unit> units = {add(1, 0, 1, 0), add(2, 1, 2, 1), add(4, 3, 4, 3), add(6, 5, 6, 5)};
    fx.columns.erase(ArcId{StationId{6}, satG(5), sigL2()});
    const double sigma = 0.003;
    const fcyc::FoldPlan plan = fcyc::placeUnits(units, std::vector<char>(4, 1), fx, sigma);
    ASSERT_EQ(plan.blocks.size(), 2u);
    EXPECT_EQ(plan.block_units[0], (std::vector<int>{0, 1}));
    EXPECT_EQ(plan.block_units[1], (std::vector<int>{2}));
    EXPECT_EQ(plan.unplaced, (std::vector<int>{3}));
    ASSERT_EQ(plan.blocks[0].size(), 4u);
    // Row 0 is unit A's L1: four terms on the live columns, target lambda N - sum c x_lin.
    const NetworkNormalEquations::CycleRow& row = plan.blocks[0][0];
    ASSERT_EQ(row.terms.size(), 4u);
    double lin = 0.0;
    for (const arcgraph::CycleTerm& t : fx.floats[0].cycle.terms)
        lin += t.coeff * fx.columns.at(t.arc).x_lin_m;
    EXPECT_NEAR(row.zhat_m, kL1 * 7.0 - lin, 1e-12);
    EXPECT_NEAR(row.pz_m2, (sigma * kL1) * (sigma * kL1), 1e-18);
    EXPECT_NEAR(plan.blocks[0][1].zhat_m, kL2 * -4.0
                    - [&] { double s = 0.0; for (const auto& t : fx.floats[1].cycle.terms) s += t.coeff * fx.columns.at(t.arc).x_lin_m; return s; }(),
                1e-12);
    for (const auto& term : row.terms) {
        EXPECT_TRUE(term.station == 0 || term.station == 1);
        EXPECT_TRUE(term.coeff == 1.0 || term.coeff == -1.0);
    }
    // An inactive unit is not placed at all.
    const fcyc::FoldPlan p2 = fcyc::placeUnits(units, std::vector<char>{0, 1, 1, 0}, fx, sigma);
    EXPECT_EQ(p2.blocks.size(), 2u);
    EXPECT_TRUE(p2.unplaced.empty());
}

// ============================== the fold on an arrowhead ==============================

/// A synthetic network epoch: per station, local columns (2p, 2p+1) are pair p's L1 and L2
/// ambiguities (metres), with a random shared block and a few extra locals; the float pair
/// covariance is shaped to a settled pair.
struct Bed {
    NetworkEstimator::Datum datum;
    Eigen::Index n_shared{0};
    std::vector<Eigen::MatrixXd> A_s, A_r;
    std::vector<Eigen::VectorXd> w, y;
    Eigen::VectorXd x_s_true;
    std::vector<Eigen::VectorXd> x_r_true;
    std::vector<std::pair<long long, long long>> ints;   // per (station, pair), station-major
    int n_sta{0}, pairs{0};
};

NetworkEstimator estimatorOf(const Bed& b) {
    NetworkEstimator est;
    for (Eigen::Index j = 0; j < b.n_shared; ++j) est.registerClock("S" + std::to_string(j));
    est.beginEpoch();
    for (int r = 0; r < b.n_sta; ++r) est.addStation(b.A_s[r], b.A_r[r], b.w[r], b.y[r]);
    const Eigen::MatrixXd prior_A_s = Eigen::MatrixXd::Identity(b.n_shared, b.n_shared);
    const Eigen::MatrixXd prior_A_r(b.n_shared, 0);
    const Eigen::VectorXd prior_w = Eigen::VectorXd::Constant(b.n_shared, 0.5);
    est.addStation(prior_A_s, prior_A_r, prior_w, b.x_s_true);
    return est;
}

Bed makeBed(std::uint64_t seed, int n_sta, int pairs) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> n01(0.0, 1.0);
    Bed b;
    b.n_sta = n_sta;
    b.pairs = pairs;
    b.n_shared = 12;
    b.x_s_true = Eigen::VectorXd::NullaryExpr(b.n_shared, [&] { return n01(rng); });
    const Eigen::Index nl = 2 * pairs + 2;
    for (int r = 0; r < n_sta; ++r) {
        const Eigen::Index m = nl + 4;
        b.A_s.push_back(Eigen::MatrixXd::NullaryExpr(m, b.n_shared, [&] { return n01(rng); }));
        b.A_r.push_back(Eigen::MatrixXd::NullaryExpr(m, nl, [&] { return n01(rng); }));
        b.w.push_back(Eigen::VectorXd::Constant(m, 1.0));
        Eigen::VectorXd xr = Eigen::VectorXd::NullaryExpr(nl, [&] { return n01(rng); });
        for (int p = 0; p < pairs; ++p) {
            const long long n1 = static_cast<long long>(std::llround(20.0 * n01(rng)));
            const long long n2 = static_cast<long long>(std::llround(20.0 * n01(rng)));
            b.ints.emplace_back(n1, n2);
            xr(2 * p)     = kL1 * static_cast<double>(n1);
            xr(2 * p + 1) = kL2 * static_cast<double>(n2);
        }
        b.x_r_true.push_back(xr);
    }
    b.datum.push_back(NetworkEstimator::Pin{0, b.x_s_true(0)});
    // Shape each pair's float covariance to a settled pair (wide lane 0.06, ionosphere-free
    // 0.04 narrow-lane cycle) with a 2x2 change of the pair's columns.
    Eigen::Matrix2d M;
    M << 1.0, -1.0, kF1 / (kF1 - kF2), -kF2 / (kF1 - kF2);
    const Eigen::Matrix2d Minv = M.inverse();
    const Eigen::Matrix2d tgt_cyc = Minv * Eigen::Vector2d(0.06 * 0.06, 0.04 * 0.04).asDiagonal() * Minv.transpose();
    const Eigen::Matrix2d lam = Eigen::Vector2d(kL1, kL2).asDiagonal();
    const Eigen::Matrix2d Q = Eigen::Matrix2d(lam * tgt_cyc * lam).llt().matrixL();
    for (int r = 0; r < n_sta; ++r) b.y.push_back(Eigen::VectorXd::Zero(b.A_s[r].rows()));
    {
        NetworkEstimator est = estimatorOf(b);
        est.solve(b.datum);
        for (int r = 0; r < n_sta; ++r)
            for (int p = 0; p < pairs; ++p) {
                const Eigen::MatrixXd J = est.jointConditionalCovariance(
                    {{static_cast<std::size_t>(r), 2 * p}, {static_cast<std::size_t>(r), 2 * p + 1}}, b.datum);
                const Eigen::Matrix2d R = Eigen::Matrix2d(J).llt().matrixL();
                const Eigen::Matrix2d T = R * Q.inverse();
                b.A_r[r].middleCols(2 * p, 2) = Eigen::MatrixXd(b.A_r[r].middleCols(2 * p, 2) * T);
            }
    }
    for (int r = 0; r < n_sta; ++r) {
        b.y[r] = b.A_s[r] * b.x_s_true + b.A_r[r] * b.x_r_true[r];
        for (Eigen::Index i = 0; i < b.y[r].size(); ++i) b.y[r](i) += n01(rng);
    }
    return b;
}

/// The search's hand-over of a bed: one single-arc "DD" per (station, pair, band) at x_lin 0,
/// with the float and its pair covariance from the solved estimator, every pair determined at
/// the bed's integers.
ArEpochFixes handOver(const Bed& b, const NetworkEstimator& est, const Eigen::VectorXd& x_s) {
    ArEpochFixes fx;
    fx.searched = true;
    for (int r = 0; r < b.n_sta; ++r) {
        const Eigen::VectorXd xr = est.reconstructStation(static_cast<std::size_t>(r), x_s);
        for (int p = 0; p < b.pairs; ++p) {
            const Eigen::MatrixXd J = est.jointConditionalCovariance(
                {{static_cast<std::size_t>(r), 2 * p}, {static_cast<std::size_t>(r), 2 * p + 1}}, b.datum);
            const auto [n1, n2] = b.ints[static_cast<std::size_t>(r * b.pairs + p)];
            const int a = static_cast<int>(fx.floats.size());
            const SignalId sig[2] = {sigL1(), sigL2()};
            const double lam[2] = {kL1, kL2};
            for (int k = 0; k < 2; ++k) {
                SearchedCycle s = searched(sig[k], r, r, p + 1, p + 1);
                const ArcId arc{StationId{r}, satG(p + 1), sig[k]};
                s.cycle.terms.push_back(arcgraph::CycleTerm{arc, +1});
                fx.columns[arc] = ArcColumn{static_cast<std::size_t>(r), 2 * p + k, 0.0, lam[k]};
                s.y_cyc         = xr(2 * p + k) / lam[k];
                s.var_cyc2      = J(k, k) / (lam[k] * lam[k]);
                s.cov_twin_cyc2 = J(0, 1) / (kL1 * kL2);
                s.twin          = a + 1 - k;
                s.fixed         = true;
                s.integer       = k == 0 ? n1 : n2;
                fx.floats.push_back(s);
            }
        }
    }
    return fx;
}

/// The default knobs with a shift bound to the bed's scale: its shared block is unit-scale
/// random, not metres of a network, so the 0.5 m bound of the knob is no guard there.
fcyc::FoldKnobs bedKnobs() {
    fcyc::FoldKnobs k;
    k.max_shift_m = 1e3;
    return k;
}

int countAction(const fcyc::EpochFold& f, UnitAction a) {
    return static_cast<int>(std::count_if(f.fate.begin(), f.fate.end(),
                                          [&](const fcyc::UnitFate& x) { return x.action == a; }));
}

TEST(NetworkFixedCycles, RightIntegersFoldAndEqualTheDetachedHeldSolve) {
    const Bed b = makeBed(11, 4, 3);
    NetworkEstimator est = estimatorOf(b);
    const Eigen::VectorXd x_s = est.solve(b.datum);
    const ArEpochFixes fx = handOver(b, est, x_s);
    std::vector<Unit> units = fcyc::epochUnits(fx);
    ASSERT_EQ(units.size(), 12u);
    fcyc::scoreUnits(units, fx, 1e-6);
    for (const Unit& u : units) EXPECT_FALSE(u.fail) << "T " << u.t;
    const fcyc::FoldKnobs k = bedKnobs();
    const fcyc::EpochFold f = fcyc::foldUnits(est, b.datum, x_s, units, fx, k);
    ASSERT_TRUE(f.accepted);
    EXPECT_EQ(f.passes, 1);
    EXPECT_EQ(countAction(f, UnitAction::Folded), 12);
    EXPECT_EQ(f.plan.blocks.size(), 4u);   // one per station
    for (const fcyc::BlockFigures& fig : f.blocks) {
        EXPECT_EQ(fig.k, 6);
        EXPECT_LE(fig.chi2, fig.bar);
        EXPECT_GT(fig.chi2_expect, 0.0);
    }
    // The fold is the detached held solve of the placed rows.
    const NetworkEstimator::HeldSolve h = est.solveHeldDetached(b.datum, x_s, f.plan.blocks);
    ASSERT_TRUE(h.ok);
    EXPECT_EQ((h.x_s - f.held.x_s).lpNorm<Eigen::Infinity>(), 0.0);
    EXPECT_GT((f.held.x_s - x_s).lpNorm<Eigen::Infinity>(), 0.0);
    EXPECT_LE(f.held.max_shift_m, k.max_shift_m);
    // The held estimate is nearer the truth on the shared block than the float.
    EXPECT_LT((f.held.x_s - b.x_s_true).norm(), (x_s - b.x_s_true).norm());
}

TEST(NetworkFixedCycles, AOneOnePairIsRefusedByItsOwnTest) {
    const Bed b = makeBed(12, 4, 3);
    NetworkEstimator est = estimatorOf(b);
    const Eigen::VectorXd x_s = est.solve(b.datum);
    ArEpochFixes fx = handOver(b, est, x_s);
    fx.floats[4].integer += 1;   // station 0, pair 2: a (1, 1) error
    fx.floats[5].integer += 1;
    std::vector<Unit> units = fcyc::epochUnits(fx);
    fcyc::scoreUnits(units, fx, 1e-6);
    EXPECT_TRUE(units[2].fail) << "T " << units[2].t;
    const fcyc::EpochFold f = fcyc::foldUnits(est, b.datum, x_s, units, fx, bedKnobs());
    ASSERT_TRUE(f.accepted);
    EXPECT_EQ(f.fate[2].action, UnitAction::UnitRefused);
    EXPECT_EQ(f.fate[2].block, -1);
    EXPECT_EQ(countAction(f, UnitAction::Folded), 11);
}

TEST(NetworkFixedCycles, AWrongPairPastItsTestFailsAcceptanceAndIsDropped) {
    const Bed b = makeBed(13, 3, 4);
    NetworkEstimator est = estimatorOf(b);
    const Eigen::VectorXd x_s = est.solve(b.datum);
    ArEpochFixes fx = handOver(b, est, x_s);
    fx.floats[2].integer += 1;   // station 0, pair 1: +1 L1
    std::vector<Unit> units = fcyc::epochUnits(fx);
    fcyc::scoreUnits(units, fx, 1e-6);
    ASSERT_TRUE(units[1].fail);
    for (Unit& u : units) u.fail = false;   // forced past the unit test: acceptance is the guard
    const fcyc::EpochFold f = fcyc::foldUnits(est, b.datum, x_s, units, fx, bedKnobs());
    ASSERT_TRUE(f.accepted);
    EXPECT_EQ(f.passes, 2);
    EXPECT_EQ(f.failures, 1);
    EXPECT_EQ(f.fate[1].action, UnitAction::Dropped);
    EXPECT_EQ(f.fate[1].pass, 0);
    EXPECT_GT(f.fate[1].fig.chi2, f.fate[1].fig.bar);
    EXPECT_EQ(countAction(f, UnitAction::Folded), 11);

    // With no drop pass the failing block is left float; the other stations' blocks fold.
    fcyc::FoldKnobs k0 = bedKnobs();
    k0.max_iter = 0;
    const fcyc::EpochFold g = fcyc::foldUnits(est, b.datum, x_s, units, fx, k0);
    ASSERT_TRUE(g.accepted);
    EXPECT_EQ(countAction(g, UnitAction::LeftFloat), 4);   // station 0's four pairs
    EXPECT_EQ(countAction(g, UnitAction::Folded), 8);
    for (int u = 0; u < 4; ++u) EXPECT_EQ(g.fate[u].action, UnitAction::LeftFloat);
}

TEST(NetworkFixedCycles, TheShiftBoundAndAKernelThrowLeaveTheEpochFloat) {
    const Bed b = makeBed(14, 3, 2);
    NetworkEstimator est = estimatorOf(b);
    const Eigen::VectorXd x_s = est.solve(b.datum);
    ArEpochFixes fx = handOver(b, est, x_s);
    std::vector<Unit> units = fcyc::epochUnits(fx);
    fcyc::scoreUnits(units, fx, 1e-6);
    fcyc::FoldKnobs k;
    k.max_shift_m = 1e-12;
    const fcyc::EpochFold f = fcyc::foldUnits(est, b.datum, x_s, units, fx, k);
    EXPECT_FALSE(f.accepted);
    EXPECT_EQ(countAction(f, UnitAction::ShiftRefused), static_cast<int>(units.size()));
    EXPECT_GT(f.held.max_shift_m, k.max_shift_m);

    // A column outside the station's block: the kernel throws, nothing is folded.
    fx.columns.begin()->second.local_col = 999;
    const fcyc::EpochFold g = fcyc::foldUnits(est, b.datum, x_s, units, fx, bedKnobs());
    EXPECT_FALSE(g.accepted);
    EXPECT_FALSE(g.error.empty());
    EXPECT_GT(countAction(g, UnitAction::KernelError), 0);
    EXPECT_EQ(countAction(g, UnitAction::Folded), 0);
}

TEST(NetworkFixedCycles, ReleasedAndLapsedUnitsAreNotPlaced) {
    // Source lattice: a unit whose rows a release of this epoch took, and a unit the search no
    // longer determines (the lapse), are not placed; the rest fold.
    const Bed b = makeBed(11, 4, 3);
    NetworkEstimator est = estimatorOf(b);
    const Eigen::VectorXd x_s = est.solve(b.datum);
    const ArEpochFixes fx = handOver(b, est, x_s);
    std::vector<Unit> units = fcyc::epochUnits(fx);
    ASSERT_EQ(units.size(), 12u);
    fcyc::scoreUnits(units, fx, 1e-6);
    units[0].released = true;
    units[1].lapsed   = true;
    const fcyc::EpochFold f = fcyc::foldUnits(est, b.datum, x_s, units, fx, bedKnobs());
    ASSERT_TRUE(f.accepted);
    EXPECT_EQ(f.fate[0].action, UnitAction::Released);
    EXPECT_EQ(f.fate[1].action, UnitAction::Lapsed);
    EXPECT_EQ(f.fate[1].block, -1);
    EXPECT_EQ(countAction(f, UnitAction::Folded), 10);
    EXPECT_TRUE(fcyc::foldReleases(units, f, fx).empty());   // neither releases anything
}

TEST(NetworkFixedCycles, ActionNamesAreTheTraceVocabulary) {
    const char* const want[] = {"folded", "unit_refused", "unplaced", "dropped", "left_float",
                                "degenerate", "float_mismatch", "non_finite", "kernel_error",
                                "shift_refused", "released", "lapsed"};
    ASSERT_EQ(std::size(want), fcyc::kUnitActionCount);
    for (std::size_t k = 0; k < fcyc::kUnitActionCount; ++k)
        EXPECT_STREQ(fcyc::unitActionName(static_cast<UnitAction>(k)), want[k]);
}

}  // namespace
}  // namespace GnssProcessor
