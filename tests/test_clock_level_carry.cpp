/**
 * @file test_clock_level_carry.cpp
 * @brief Card CLV (docs/refound/CLV_clock_level_2026-09-22.md): the satellite clock
 *        LEVEL, estimated from the code alone and re-based into the carry
 *        (@c network.clock_level_carry).  Suite @c ClockLevelCarryTest.
 *
 * What is pinned here:
 *  - the level direction (+1 on the satellite clock, +1 on each of its phase biases) is
 *    blind to every phase row and seen by every code row, on the shared border the
 *    estimator actually reduces;
 *  - why the carry freezes it: Card 1d's time update shrinks a coupling row's clock
 *    term at the legacy clock rate and keeps its phase-bias term, so a carried
 *    ambiguity row states its satellite's level;
 *  - the code-only information (the ionosphere-free pair formulas, the decay, the
 *    variance and its floor);
 *  - the two-way fit separates satellite levels from station offsets exactly, screens a
 *    gross pair and drops a thin satellite;
 *  - the filter arms, accumulates, breaks and caps;
 *  - the re-base moves every carried reference of the satellite and nothing else, and
 *    every coupling row keeps its residual when the border moves with it;
 *  - with the carry tight along the level, a solve started from the re-based prior
 *    keeps the shift and the phase side;
 *  - the level step travels on the wire (the C record's lvl= token): an emitter fed the
 *    epochs directly and one fed the written-then-read records declare the same phase,
 *    code and clock discontinuities, and without the declaration the level steps would
 *    be declared as phase-bias discontinuities.
 *
 * Station rows follow the production sign convention: MeasurementModel.cpp writes the
 * satellite clock with a -1 partial on code AND phase rows and the satellite phase
 * bias with a +1 partial on phase rows only, and networkSplitStation_ copies those
 * columns verbatim into the shared border (the same convention the NetworkEstimatorTest
 * fixtures use).
 */

#include <gtest/gtest.h>

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfigTypes.hpp"
#include "filter/NetworkClockLevel.hpp"
#include "filter/NetworkCoupling.hpp"
#include "filter/NetworkEstimator.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkProductIO.hpp"
#include "products/SsrCorrectionStore.hpp"

#include <Eigen/Dense>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace GnssProcessor {
namespace {

/// A small network: S satellites with a clock and one phase bias each, R stations.
struct Net {
    int S{0}, R{0};
    std::vector<std::string> sats;
    std::vector<Eigen::Index> clk, pb;
};

Net makeNet(NetworkEstimator& est, int S, int R) {
    Net n;
    n.S = S;
    n.R = R;
    for (int s = 0; s < S; ++s) {
        const std::string sat = std::string("G0") + std::to_string(s + 1);
        n.sats.push_back(sat);
        n.clk.push_back(est.registerClock(sat));
        n.pb.push_back(est.registerPhaseBias(sat, "L1C"));
    }
    return n;
}

/// One station's rows.  Locals: [rx clock, N_0 .. N_{S-1}].  Station 0 pins its
/// ambiguities (the local half of the phase datum), exactly as the estimator tests do.
void stationRows(const Net& n, int r, const NetworkEstimator& est, bool with_code,
                 Eigen::MatrixXd& A_s, Eigen::MatrixXd& A_r, Eigen::VectorXd& w,
                 Eigen::VectorXd& y) {
    const bool pivot = (r == 0);
    // Phase-only stations carry a local prior row on the receiver clock so the local
    // block stays regular (the ambiguities absorb everything else); it touches no
    // shared column.
    const Eigen::Index m = (with_code ? 2 : 1) * n.S + (pivot ? n.S : 0) + (with_code ? 0 : 1);
    A_s = Eigen::MatrixXd::Zero(m, est.numShared());
    A_r = Eigen::MatrixXd::Zero(m, 1 + n.S);
    w = Eigen::VectorXd::Ones(m);
    y = Eigen::VectorXd::Zero(m);
    Eigen::Index row = 0;
    for (int s = 0; s < n.S; ++s) {
        if (with_code) {
            A_s(row, n.clk[s]) = -1.0;   // code: rx - clk
            A_r(row, 0) = 1.0;
            y(row) = 0.1 * (r + 1) - 0.05 * s;
            ++row;
        }
        A_s(row, n.clk[s]) = -1.0;       // phase: rx - clk + pb + N
        A_s(row, n.pb[s])  = +1.0;
        A_r(row, 0) = 1.0;
        A_r(row, 1 + s) = 1.0;
        w(row) = 1e4;
        y(row) = 0.3 * (r + 1) + 0.2 * s;
        ++row;
    }
    if (pivot)
        for (int s = 0; s < n.S; ++s) {
            A_r(row, 1 + s) = 1.0;
            w(row) = 1e10;
            ++row;
        }
    if (!with_code) {
        A_r(row, 0) = 1.0;
        w(row) = 1.0;
        ++row;
    }
}

void addStations(const Net& n, NetworkEstimator& est, bool with_code) {
    est.beginEpoch();
    for (int r = 0; r < n.R; ++r) {
        Eigen::MatrixXd A_s, A_r;
        Eigen::VectorXd w, y;
        stationRows(n, r, est, with_code, A_s, A_r, w, y);
        est.addStation(A_s, A_r, w, y);
    }
}

/// The level direction of satellite s over the whole shared border.
Eigen::VectorXd levelDir(const Net& n, const NetworkEstimator& est, int s) {
    Eigen::VectorXd u = Eigen::VectorXd::Zero(est.numShared());
    u(n.clk[s]) = 1.0;
    u(n.pb[s])  = 1.0;
    return u;
}

CoupledShared term(SharedParamKind kind, const std::string& sat, const std::string& sig,
                   double coeff, double s_prev, double p_prev) {
    CoupledShared cs;
    cs.kind   = kind;
    cs.sat    = sat;
    cs.signal = sig;
    cs.coeff  = coeff;
    cs.s_prev = s_prev;
    cs.p_prev = p_prev;
    return cs;
}

CarryPrior prior(SharedParamKind kind, const std::string& sat, const std::string& sig,
                 double value) {
    CarryPrior cp{};
    cp.kind   = kind;
    cp.sat    = sat;
    cp.signal = sig;
    cp.value  = value;
    cp.var    = 1e-4;
    return cp;
}

/// The part of a built coupling row that depends on the border: y - m^T s.
double rowResidual(const PendCplRow& row, const Eigen::VectorXd& s) {
    double r = row.y;
    for (const auto& [c, m] : row.shared) r -= m * s(c);
    return r;
}

}  // namespace

// ---------------------------------------------------------------------------
// R1: the level direction on the reduced shared border.
// ---------------------------------------------------------------------------

// With phase rows only, every level direction is in the null space of the reduced
// normals: the ambiguities and receiver clocks are reduced out, the pivot's pinned
// ambiguities keep -clk + pb observed, and the level leaves -clk + pb unchanged.  The
// clock ALONE is not in it (the pinned phase rows see it), so the null space is not a
// trivially empty statement.  With code rows added, the level is observed.
TEST(ClockLevelCarryTest, LevelDirectionIsBlindToPhaseAndSeenByCode) {
    NetworkEstimator est;
    const Net n = makeNet(est, /*S=*/4, /*R=*/5);

    addStations(n, est, /*with_code=*/false);
    const Eigen::MatrixXd Nph = est.reducedSharedNormals();
    const double scale = Nph.cwiseAbs().maxCoeff();
    for (int s = 0; s < n.S; ++s) {
        const Eigen::VectorXd u = levelDir(n, est, s);
        EXPECT_LT((Nph * u).cwiseAbs().maxCoeff(), 1e-7 * scale)
            << "phase rows must not see the level of " << n.sats[s];
        Eigen::VectorXd e = Eigen::VectorXd::Zero(est.numShared());
        e(n.clk[s]) = 1.0;
        EXPECT_GT((Nph * e).cwiseAbs().maxCoeff(), 1e-3 * scale)
            << "the clock alone is phase-observable through the pinned pivot";
    }

    addStations(n, est, /*with_code=*/true);
    const Eigen::MatrixXd Nall = est.reducedSharedNormals();
    for (int s = 0; s < n.S; ++s) {
        const Eigen::VectorXd u = levelDir(n, est, s);
        EXPECT_GT(u.dot(Nall * u), 0.1) << "code rows must see the level of " << n.sats[s];
    }
}

// ---------------------------------------------------------------------------
// The mechanism: Card 1d's time update turns a carried ambiguity into a level statement.
// ---------------------------------------------------------------------------

// An ambiguity's exact regression is on the phase side (-clock + phase bias) of its own
// satellite, plus the receiver-clock cross terms every other satellite's clock carries;
// the clock gauge makes the clock coefficients sum to zero.  Such a row is blind to
// every between-satellite level CONTRAST (each satellite's clock-plus-phase-bias
// sensitivity is the same).  With the time update at the legacy clock rate
// q = 1e-3 m^2/s against a clock carried at the Q3 anchor's variance floor (1e-5 m^2),
// kappa_clk = 1e-5 / (1e-5 + 0.03) keeps 0.03 percent of every clock coefficient, the
// family restore has nothing to put back (the family sums to zero), and the
// phase-bias coefficient keeps kappa about 1: the row now states its satellite's level
// against the others almost as tightly as its own variance.
TEST(ClockLevelCarryTest, TimeUpdateTurnsAnAmbiguityRowIntoALevelStatement) {
    NetworkEstimator est;
    const Net n = makeNet(est, /*S=*/3, /*R=*/1);
    GlobalVtecShConfig shc;
    const std::map<std::tuple<SharedParamKind, std::string, std::string>, double> hist;
    const double q_clock = 1e-3, q_cb = 1e-10, q_pb = 1e-12, gap = 30.0;

    LocalCarry lc{1.25, 1e-4, 0.0};
    lc.coupling = {
        term(SharedParamKind::Clock, "G01", "", -(1.0 - 1.0 / 3.0), 0.40, 1e-5),
        term(SharedParamKind::Clock, "G02", "", 1.0 / 3.0, -0.70, 1e-5),
        term(SharedParamKind::Clock, "G03", "", 1.0 / 3.0, 1.10, 1e-5),
        term(SharedParamKind::PhaseBias, "G01", "L1C", 1.0, -2.50, 2e-5),
    };
    auto build = [&](bool tu, PendCplRow& row) {
        StationAssembly R;
        double sigma2 = 0.0;
        CouplingRowBuilder cb{est, shc, q_clock, q_cb, q_pb,
                              /*joint=*/false, tu, /*shmarg=*/false, /*gauge=*/true,
                              /*reinit=*/false, hist, R};
        EXPECT_TRUE(cb.scalarRowIngredients(/*col=*/1, /*x_lin=*/0.05, lc, 1e-4, gap, row,
                                            sigma2));
    };
    PendCplRow exact, shrunk;
    build(/*tu=*/false, exact);
    build(/*tu=*/true, shrunk);
    const Eigen::VectorXd s0 = Eigen::VectorXd::Zero(est.numShared());
    // The contrast: G01's level up, G02's down.
    const Eigen::VectorXd c = levelDir(n, est, 0) - levelDir(n, est, 1);
    const double d = 0.5;
    EXPECT_NEAR(rowResidual(exact, s0 + d * c), rowResidual(exact, s0), 1e-12)
        << "the exact row is blind to the level contrast";
    const double kap = 1e-5 / (1e-5 + q_clock * gap);
    EXPECT_NEAR(rowResidual(shrunk, s0) - rowResidual(shrunk, s0 + d * c), d * (1.0 - kap),
                1e-9) << "the shrunk row pins it";
}

// ---------------------------------------------------------------------------
// R2: the code-only information.
// ---------------------------------------------------------------------------

TEST(ClockLevelCarryTest, InformationAccumulatesDecaysAndFloors) {
    // L1/L2: A = f1^2/(f1^2-f2^2), B = f2^2/(f1^2-f2^2); one IF pair of unit weights
    // carries 1/(A^2 + B^2), and its residual is A v1 - B v2.
    const double f1 = 1575.42e6, f2 = 1227.60e6;
    const double a = f1 * f1 / (f1 * f1 - f2 * f2);
    const double b = f2 * f2 / (f1 * f1 - f2 * f2);
    EXPECT_NEAR(clockLevelIfInformation(1.0, f1, 1.0, f2), 1.0 / (a * a + b * b), 1e-15);
    EXPECT_NEAR(clockLevelIfInformation(4.0, f1, 2.0, f2), 1.0 / (a * a / 4.0 + b * b / 2.0),
                1e-15);
    EXPECT_EQ(clockLevelIfInformation(0.0, f1, 1.0, f2), 0.0);
    EXPECT_EQ(clockLevelIfInformation(1.0, f1, 1.0, f1), 0.0);   // no geometry-free leverage
    EXPECT_NEAR(clockLevelIfResidual(0.3, f1, -0.2, f2), a * 0.3 + b * 0.2, 1e-12);
    // A level error e moves both codes by e and the free slant ionosphere absorbs a
    // share alpha_j delta of each: the IF residual sees e whatever delta is.
    const double alpha2 = (f1 / f2) * (f1 / f2);
    for (double delta : {0.0, 0.713, -2.0})
        EXPECT_NEAR(clockLevelIfResidual(1.0 - delta, f1, 1.0 - alpha2 * delta, f2), 1.0, 1e-12);

    // Decay: one horizon multiplies by 1/e; never set reads 0; no elapsed time or no
    // horizon leaves it unchanged.
    const double t0 = 2313.0 * 604800.0 + 259230.0;
    EXPECT_NEAR(clockLevelDecayedInfo(10.0, t0, t0 + 7200.0, 7200.0), 10.0 * std::exp(-1.0),
                1e-12);
    EXPECT_EQ(clockLevelDecayedInfo(10.0, 0.0, t0, 7200.0), 0.0);
    EXPECT_EQ(clockLevelDecayedInfo(10.0, t0, t0, 7200.0), 10.0);
    EXPECT_EQ(clockLevelDecayedInfo(10.0, t0, t0 + 30.0, 0.0), 10.0);

    // Variance: 1/I above the floor, the floor at or below it, the ignition variance
    // with no information.
    bool at_floor = true;
    EXPECT_NEAR(clockLevelVariance(4.0, 0.01, 1e6, &at_floor), 0.25, 1e-15);
    EXPECT_FALSE(at_floor);
    EXPECT_NEAR(clockLevelVariance(1e6, 0.01, 1e6, &at_floor), 1e-4, 1e-18);
    EXPECT_TRUE(at_floor);
    EXPECT_EQ(clockLevelVariance(0.0, 0.01, 1e6, &at_floor), 1e6);
    EXPECT_FALSE(at_floor);
}

// ---------------------------------------------------------------------------
// The two-way fit.
// ---------------------------------------------------------------------------

// Residuals built as e_sat - c_station (plus one gross pair): the fit returns the
// satellite effects up to the constellation mean, which the constraint pins at zero,
// screens the gross pair, and drops a satellite left with too few pairs.
TEST(ClockLevelCarryTest, TwoWayFitSeparatesSatelliteLevelsFromStationOffsets) {
    const std::map<std::string, double> e = {{"G01", 0.9}, {"G02", -0.4}, {"G03", 0.2},
                                             {"G04", -0.7}};
    const std::vector<double> c = {0.3, -1.2, 0.05, 2.0, -0.6};
    double mean_e = 0.0;
    for (const auto& [s, v] : e) mean_e += v;
    mean_e /= static_cast<double>(e.size());
    std::vector<ClockLevelPair> pairs;
    for (int r = 0; r < static_cast<int>(c.size()); ++r)
        for (const auto& [s, v] : e)
            pairs.push_back({s, r, v - c[static_cast<std::size_t>(r)], 1.0 + 0.1 * r});
    // A gross pair on G02 at station 1 (a code blunder the screen must drop).
    pairs.push_back({"G02", 1, 25.0, 1.0});
    // G05 is seen by two stations only.
    pairs.push_back({"G05", 0, 0.1, 1.0});
    pairs.push_back({"G05", 1, 0.2, 1.0});

    const ClockLevelTwoWay tw = clockLevelTwoWay(pairs, kClvMinPairs, kClvClipK, kClvClipFloorM);
    ASSERT_TRUE(tw.ok);
    EXPECT_EQ(tw.clipped, 1);
    EXPECT_EQ(tw.few, 1);
    EXPECT_EQ(tw.effect.count("G05"), 0u);
    EXPECT_EQ(tw.used, 20);
    for (const auto& [s, v] : e) {
        ASSERT_TRUE(tw.effect.count(s));
        EXPECT_NEAR(tw.effect.at(s), v - mean_e, 1e-9) << s;
        EXPECT_EQ(tw.n_pairs.at(s), 5);
    }
    double sum = 0.0;
    for (const auto& [s, v] : tw.effect) sum += v;
    EXPECT_NEAR(sum, 0.0, 1e-9) << "the common level is the datum, never fitted";

    // Each constellation is pinned on its own: a Galileo satellite's effect is not
    // pulled by the GPS mean.
    std::vector<ClockLevelPair> mixed = pairs;
    for (int r = 0; r < 3; ++r) {
        mixed.push_back({"E11", r, 1.5 - c[static_cast<std::size_t>(r)], 1.0});
        mixed.push_back({"E12", r, -0.5 - c[static_cast<std::size_t>(r)], 1.0});
    }
    const ClockLevelTwoWay tm = clockLevelTwoWay(mixed, kClvMinPairs, kClvClipK, kClvClipFloorM);
    ASSERT_TRUE(tm.ok);
    EXPECT_NEAR(tm.effect.at("E11"), 1.0, 1e-9);
    EXPECT_NEAR(tm.effect.at("E12"), -1.0, 1e-9);
    EXPECT_NEAR(tm.effect.at("G01"), 0.9 - mean_e, 1e-9);
}

// ---------------------------------------------------------------------------
// The filter.
// ---------------------------------------------------------------------------

TEST(ClockLevelCarryTest, FilterArmsAccumulatesBreaksAndCaps) {
    // Arming: gain 1, the level is set to this epoch's code estimate.
    ClockLevelUpdate u = clockLevelFilterUpdate(0.0, /*armed=*/false, 8.0, 0.6, kClvBreakK, 0.01);
    EXPECT_TRUE(u.restarted);
    EXPECT_FALSE(u.broke);
    EXPECT_DOUBLE_EQ(u.gain, 1.0);
    EXPECT_DOUBLE_EQ(u.info, 8.0);
    // Accumulating: the running mean, gain = i / (I + i).
    u = clockLevelFilterUpdate(24.0, /*armed=*/true, 8.0, 0.1, kClvBreakK, 0.01);
    EXPECT_FALSE(u.restarted);
    EXPECT_DOUBLE_EQ(u.info, 32.0);
    EXPECT_DOUBLE_EQ(u.gain, 0.25);
    // Breaking: a residual beyond k sqrt(1/I + 1/i) restarts the filter.
    const double spred = std::sqrt(1.0 / 24.0 + 1.0 / 8.0);
    u = clockLevelFilterUpdate(24.0, true, 8.0, 1.01 * kClvBreakK * spred, kClvBreakK, 0.01);
    EXPECT_TRUE(u.restarted);
    EXPECT_TRUE(u.broke);
    EXPECT_DOUBLE_EQ(u.gain, 1.0);
    u = clockLevelFilterUpdate(24.0, true, 8.0, 0.99 * kClvBreakK * spred, kClvBreakK, 0.01);
    EXPECT_FALSE(u.restarted);
    // The floor caps the information at 1 / floor^2, which keeps a minimum gain.
    u = clockLevelFilterUpdate(9999.0, true, 8.0, 0.0, kClvBreakK, 0.01);
    EXPECT_TRUE(u.at_floor);
    EXPECT_DOUBLE_EQ(u.info, 1e4);
    EXPECT_DOUBLE_EQ(u.gain, 8.0 / 1e4);
    // No information: nothing to do.
    u = clockLevelFilterUpdate(24.0, true, 0.0, 0.3, kClvBreakK, 0.01);
    EXPECT_EQ(u.info, 0.0);
    EXPECT_EQ(u.gain, 0.0);
}

// ---------------------------------------------------------------------------
// R3 / R5: the re-base moves every carried reference of the satellite, nothing else.
// ---------------------------------------------------------------------------

TEST(ClockLevelCarryTest, ReBaseMovesEveryCarriedReferenceAlongTheLevel) {
    NetworkCarryState carry;
    carry.prev_carry = {
        prior(SharedParamKind::Clock, "G01", "", 0.8),
        prior(SharedParamKind::PhaseBias, "G01", "L1C", -0.4),
        prior(SharedParamKind::PhaseBias, "G01", "L2W", 0.6),
        prior(SharedParamKind::CodeBias, "G01", "C1C", 1.5),
        prior(SharedParamKind::Clock, "G02", "", -1.1),
        prior(SharedParamKind::PhaseBias, "G02", "L1C", 0.2),
    };
    LocalCarry amb{1.25, 1e-4, 0.0};
    amb.coupling = {term(SharedParamKind::Clock, "G01", "", -1.0, 0.8, 1e-5),
                    term(SharedParamKind::PhaseBias, "G01", "L1C", 1.0, -0.4, 1e-5),
                    term(SharedParamKind::CodeBias, "G01", "C1C", 0.01, 1.5, 1e-4),
                    term(SharedParamKind::Clock, "G02", "", 0.2, -1.1, 1e-5)};
    carry.local_carry[{3, "G01", "L1C"}] = amb;
    LocalCarry zwd{0.1, 1e-4, 0.0};
    zwd.coupling = {term(SharedParamKind::Clock, "G01", "", 0.3, 0.8, 1e-5)};
    carry.zwd_carry[3] = zwd;
    carry.prev_clk["G01"] = {0.8, 1.0};
    carry.prev_clk["G02"] = {-1.1, 1.0};
    carry.cb_exc_prev_clk["G01"] = 0.8;
    carry.pb_hold_prev_clock_m["G01"] = 0.8;
    carry.pb_level_prev["G01_L1C"] = -0.4;
    carry.pb_level_prev["G02_L1C"] = 0.2;
    carry.pb_hold["G01|L5Q"] = {};   // held but not carried: its transfer must follow too

    const double d = 0.37;
    const long long moved = clockLevelShiftCarry(carry, {{"G01", d}});
    // 3 border keys (clock, two phase biases), 3 coupling terms (two on the ambiguity,
    // one on the ZWD), 3 clock baselines, 1 last-solved phase bias.
    EXPECT_EQ(moved, 10);
    EXPECT_DOUBLE_EQ(carry.prev_carry[0].value, 0.8 + d);
    EXPECT_DOUBLE_EQ(carry.prev_carry[1].value, -0.4 + d);
    EXPECT_DOUBLE_EQ(carry.prev_carry[2].value, 0.6 + d);
    EXPECT_DOUBLE_EQ(carry.prev_carry[3].value, 1.5) << "code biases are not on the level";
    EXPECT_DOUBLE_EQ(carry.prev_carry[4].value, -1.1) << "other satellites are untouched";
    EXPECT_DOUBLE_EQ(carry.prev_carry[5].value, 0.2);
    const LocalCarry& a = carry.local_carry.at({3, "G01", "L1C"});
    EXPECT_DOUBLE_EQ(a.coupling[0].s_prev, 0.8 + d);
    EXPECT_DOUBLE_EQ(a.coupling[1].s_prev, -0.4 + d);
    EXPECT_DOUBLE_EQ(a.coupling[2].s_prev, 1.5);
    EXPECT_DOUBLE_EQ(a.coupling[3].s_prev, -1.1);
    EXPECT_DOUBLE_EQ(a.value_abs, 1.25) << "the ambiguity itself does not move";
    EXPECT_DOUBLE_EQ(carry.zwd_carry.at(3).coupling[0].s_prev, 0.8 + d);
    EXPECT_DOUBLE_EQ(carry.prev_clk.at("G01").first, 0.8 + d);
    EXPECT_DOUBLE_EQ(carry.prev_clk.at("G02").first, -1.1);
    EXPECT_DOUBLE_EQ(carry.cb_exc_prev_clk.at("G01"), 0.8 + d);
    EXPECT_DOUBLE_EQ(carry.pb_hold_prev_clock_m.at("G01"), 0.8 + d);
    EXPECT_DOUBLE_EQ(carry.pb_level_prev.at("G01_L1C"), -0.4 + d);
    EXPECT_DOUBLE_EQ(carry.pb_level_prev.at("G02_L1C"), 0.2);
    const auto& xf = carry.clk_level.sat.at("G01").xfer;
    EXPECT_DOUBLE_EQ(xf.at("L1C"), d);
    EXPECT_DOUBLE_EQ(xf.at("L2W"), d);
    EXPECT_DOUBLE_EQ(xf.at("L5Q"), d);
    EXPECT_EQ(carry.clk_level.sat.count("G02"), 0u);

    // A second shift accumulates; an empty shift is a no-op.
    clockLevelShiftCarry(carry, {{"G01", -0.07}});
    EXPECT_NEAR(carry.clk_level.sat.at("G01").xfer.at("L1C"), d - 0.07, 1e-15);
    EXPECT_EQ(clockLevelShiftCarry(carry, {}), 0);
}

// R4: every coupling row keeps its residual when the border and its s_prev move
// together along the level, including a row the Card-1d time update shrank and a row
// with terms on another satellite, so the re-base never pulls a carried row.
TEST(ClockLevelCarryTest, CouplingRowsKeepTheirResidualUnderTheReBase) {
    NetworkEstimator est;
    const Net n = makeNet(est, /*S=*/3, /*R=*/1);
    GlobalVtecShConfig shc;
    const std::map<std::tuple<SharedParamKind, std::string, std::string>, double> hist;
    const double q_clock = 1e-3, q_cb = 1e-10, q_pb = 1e-12, gap = 30.0;

    const Eigen::VectorXd s0 = (Eigen::VectorXd(est.numShared()) << 0.35, -2.4, -0.72, 0.31,
                                1.12, 0.05).finished();
    const std::map<std::string, double> shift = {{"G01", 0.42}, {"G02", -0.15}};
    Eigen::VectorXd s1 = s0;
    for (int s = 0; s < n.S; ++s) {
        const auto it = shift.find(n.sats[static_cast<std::size_t>(s)]);
        if (it != shift.end()) s1 += it->second * levelDir(n, est, s);
    }
    for (const auto& [tu, gauge] : std::vector<std::pair<bool, bool>>{
             {false, false}, {true, false}, {true, true}}) {
        NetworkCarryState carry;
        LocalCarry lc{1.25, 1e-4, 0.0};
        lc.coupling = {
            term(SharedParamKind::Clock, "G01", "", -(1.0 - 1.0 / 3.0), 0.40, 1e-5),
            term(SharedParamKind::Clock, "G02", "", 1.0 / 3.0, -0.70, 1e-5),
            term(SharedParamKind::Clock, "G03", "", 1.0 / 3.0, 1.10, 1e-5),
            term(SharedParamKind::PhaseBias, "G01", "L1C", 1.0, -2.50, 2e-5),
            term(SharedParamKind::PhaseBias, "G02", "L1C", -0.1, 0.30, 2e-5),
        };
        carry.local_carry[{0, "G01", "L1C"}] = lc;
        auto build = [&](PendCplRow& row) {
            StationAssembly R;
            double sigma2 = 0.0;
            CouplingRowBuilder cb{est, shc, q_clock, q_cb, q_pb,
                                  /*joint=*/false, tu, /*shmarg=*/false, gauge,
                                  /*reinit=*/false, hist, R};
            EXPECT_TRUE(cb.scalarRowIngredients(/*col=*/1, /*x_lin=*/0.05,
                                                carry.local_carry.at({0, "G01", "L1C"}), 1e-4,
                                                gap, row, sigma2));
        };
        PendCplRow r0;
        build(r0);
        const double res0 = rowResidual(r0, s0);
        clockLevelShiftCarry(carry, shift);
        PendCplRow r1;
        build(r1);
        EXPECT_NEAR(rowResidual(r1, s1), res0, 1e-12) << "tu " << tu << " gauge " << gauge;
    }
}

// With the carry tight along the level (the frozen posture), a solve started from the
// re-based prior keeps the shift: the clock and the phase bias move together by it and
// the phase side -clock + phase bias, which the phase data fix, does not move.
TEST(ClockLevelCarryTest, SolveKeepsTheReBasedLevelAndThePhaseSide) {
    auto solve = [](double shift, Eigen::VectorXd& x) {
        NetworkEstimator est;
        const Net n = makeNet(est, /*S=*/3, /*R=*/4);
        addStations(n, est, /*with_code=*/true);
        std::vector<NetworkEstimator::SharedPrior> keys;
        const double clk0[3] = {0.8, -1.1, 0.3};
        const double pb0[3]  = {-0.4, 0.2, 1.7};
        for (int s = 0; s < n.S; ++s) {
            NetworkEstimator::SharedPrior c;
            c.kind = SharedParamKind::Clock;
            c.sat = n.sats[static_cast<std::size_t>(s)];
            c.value = clk0[s] + (s == 0 ? shift : 0.0);
            c.info = 1e10;                    // the carry states the clock tightly
            NetworkEstimator::SharedPrior p;
            p.kind = SharedParamKind::PhaseBias;
            p.sat = c.sat;
            p.signal = "L1C";
            p.value = pb0[s] + (s == 0 ? shift : 0.0);
            p.info = 1e10;
            keys.push_back(c);
            keys.push_back(p);
        }
        est.addSharedPrior(keys);
        x = est.solve(NetworkEstimator::Datum{});
        return n;
    };
    Eigen::VectorXd xa, xb;
    const Net n = solve(0.0, xa);
    solve(0.4, xb);
    EXPECT_NEAR(xb(n.clk[0]) - xa(n.clk[0]), 0.4, 1e-6);
    EXPECT_NEAR(xb(n.pb[0]) - xa(n.pb[0]), 0.4, 1e-6);
    EXPECT_NEAR((xb(n.pb[0]) - xb(n.clk[0])) - (xa(n.pb[0]) - xa(n.clk[0])), 0.0, 1e-9);
    for (int s = 1; s < n.S; ++s) {
        EXPECT_NEAR(xb(n.clk[s]), xa(n.clk[s]), 1e-6);
        EXPECT_NEAR(xb(n.pb[s]), xa(n.pb[s]), 1e-6);
    }
}

// ---------------------------------------------------------------------------
// The wire: the level step is declared, and a replay reproduces every counter.
// ---------------------------------------------------------------------------

// Four epochs of two satellites.  G05's clock and both of its phase biases move
// together by a level step (0.30 m, then -0.20 m, then none) and at the last epoch its
// L2W phase bias also takes a genuine 0.50 m jump; G07 is steady.  The live emitter is
// fed the epochs directly; the same epochs are written with the S contract, read back
// and replayed through NetworkProductReader::replay into a replay emitter built from
// the parsed contract.  Every phase, code and clock counter must agree, the genuine
// jump must be declared once and the level steps never; the same epochs with the
// declaration stripped declare the level steps.
TEST(ClockLevelCarryTest, LevelStepRoundTripsTheWireAndKeepsTheDeclarations) {
    namespace fs = std::filesystem;
    SbasisContract contract;
    contract.decl.reference_receiver_id = "MBAR00UGA";
    contract.disc_threshold_m           = 0.05;
    contract.emit_clock_disc            = true;
    contract.present                    = true;

    const double level[4] = {0.0, 0.30, -0.20, 0.0};
    std::vector<NetworkSolutionEpoch> epochs;
    double acc = 0.0;
    for (int k = 0; k < 4; ++k) {
        acc += level[k];
        NetworkSolutionEpoch sol;
        sol.epoch_time  = GnssTime{2313, 259200.0 + 30.0 * k};
        sol.iod_ssr     = 3;
        sol.provider_id = 42;
        sol.solution_id = 1;
        NetworkSatSolution g05;
        g05.sat          = "G05";
        g05.has_clock    = true;
        g05.clock_c0_m   = 1.25 + acc;
        g05.level_step_m = level[k];
        g05.code_bias_m["C1C"] = -3.2;
        g05.code_bias_m["C2W"] = -5.3;
        g05.phase_bias["L1C"] = NetworkPhaseBias{-10.24 + acc, true, 1};
        g05.phase_bias["L2W"] = NetworkPhaseBias{-9.33 + acc + (k == 3 ? 0.50 : 0.0), true, 1};
        NetworkSatSolution g07;
        g07.sat        = "G07";
        g07.has_clock  = true;
        g07.clock_c0_m = -0.4;
        g07.code_bias_m["C1C"] = 0.8;
        g07.code_bias_m["C2W"] = 1.1;
        g07.phase_bias["L1C"] = NetworkPhaseBias{2.1, true, 1};
        g07.phase_bias["L2W"] = NetworkPhaseBias{3.4, true, 1};
        sol.sats = {g05, g07};
        epochs.push_back(sol);
    }

    const fs::path p = fs::temp_directory_path() / "clv_level_step_roundtrip.txt";
    std::error_code ec;
    fs::remove(p, ec);
    NetworkProductEmitter live = NetworkProductReader::makeReplayEmitter(contract);
    SsrCorrectionStore store_live;
    {
        NetworkProductWriter w(p.string(), contract);
        ASSERT_TRUE(w.good());
        for (const NetworkSolutionEpoch& sol : epochs) {
            live.emitToStore(sol, store_live);
            w.write(sol);
        }
    }
    // The token is on the wire only where a step happened.
    {
        std::ifstream f(p);
        std::string ln;
        int n_lvl = 0;
        while (std::getline(f, ln))
            if (ln.rfind("C,", 0) == 0 && ln.find(",lvl=") != std::string::npos) ++n_lvl;
        EXPECT_EQ(n_lvl, 2) << "one C record per nonzero level step";
    }
    bool ok = false;
    SbasisContract parsed;
    const std::vector<NetworkProductRecord> recs = NetworkProductReader::read(p.string(), &parsed, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(recs.size(), epochs.size());
    for (std::size_t k = 0; k < recs.size(); ++k) {
        ASSERT_EQ(recs[k].epoch.sats.size(), 2u);
        EXPECT_DOUBLE_EQ(recs[k].epoch.sats[0].level_step_m, level[k]);
        EXPECT_DOUBLE_EQ(recs[k].epoch.sats[1].level_step_m, 0.0);
    }
    NetworkProductEmitter replayed = NetworkProductReader::makeReplayEmitter(parsed);
    SsrCorrectionStore store_replay;
    for (const NetworkProductRecord& r : recs) NetworkProductReader::replay(r, store_replay, replayed);

    for (const char* sat : {"G05", "G07"}) {
        EXPECT_EQ(replayed.clockDiscontinuityCounter(sat), live.clockDiscontinuityCounter(sat)) << sat;
        for (const char* sig : {"L1C", "L2W"})
            EXPECT_EQ(replayed.discontinuityCounter(sat, sig), live.discontinuityCounter(sat, sig))
                << sat << " " << sig;
        for (const char* sig : {"C1C", "C2W"})
            EXPECT_EQ(replayed.codeBiasDiscontinuityCounter(sat, sig),
                      live.codeBiasDiscontinuityCounter(sat, sig)) << sat << " " << sig;
    }
    EXPECT_EQ(live.discontinuityCounter("G05", "L1C"), 0) << "a level step is not a discontinuity";
    EXPECT_EQ(live.discontinuityCounter("G05", "L2W"), 1) << "the genuine jump is declared once";
    EXPECT_EQ(live.clvJumpExcluded(), 4) << "two signals over the bar at each of two level steps";
    EXPECT_EQ(replayed.clvJumpExcluded(), live.clvJumpExcluded());

    // Without the declaration the same values declare the level as phase discontinuities.
    NetworkProductEmitter blind = NetworkProductReader::makeReplayEmitter(contract);
    SsrCorrectionStore store_blind;
    for (NetworkSolutionEpoch sol : epochs) {
        for (NetworkSatSolution& s : sol.sats) s.level_step_m = 0.0;
        blind.emitToStore(sol, store_blind);
    }
    EXPECT_EQ(blind.discontinuityCounter("G05", "L1C"), 2);
    EXPECT_EQ(blind.discontinuityCounter("G05", "L2W"), 3);
    EXPECT_EQ(blind.clvJumpExcluded(), 0);
    fs::remove(p, ec);
}

}  // namespace GnssProcessor
